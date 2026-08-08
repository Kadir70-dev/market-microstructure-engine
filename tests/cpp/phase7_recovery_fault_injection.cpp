#include <array>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>

#include "oms/recovery_workflow.hpp"
#include "persist/execution_wal.hpp"
#include "phase6_test.hpp"

// Phase E — fault-injection tests for crash/network recovery and venue
// reconciliation. Covers all 13 named scenarios. Deliberately does not
// re-prove Phase A/B's low-level journal-replay crash-safety (already
// covered by tests/cpp/phase6_crash_01..20.cpp with real fork/_Exit) --
// these tests exercise the *new* pieces this phase adds: the WAL bridge
// (persist::ExecutionWalWriter/Reader/Tailer), venue reconciliation
// (oms::PaperBrokerVenueAdapter/OrderReconciler), and the connection state
// machine (oms::RecoveryWorkflow), including how they compose with Phase D's
// exactly-once ReportSequencer.

namespace {

risk::Approval approval() {
    risk::RiskEngine e(risk::Limits{});
    risk::Request q{};
    q.volume = 1; q.risk_minor = 1; q.free_margin = 1'000'000; q.warm_mask = 1; q.session_open = true;
    return e.check(q).token;
}

std::filesystem::path temp_dir(const char* label) {
    const auto dir = std::filesystem::temp_directory_path() /
        (std::string("mme_phase7_") + label + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

// Writes `records` to a fresh segment and returns without a clean finalize --
// the writer's destructor still runs (this is in-process, not a real crash),
// but nothing beyond what was explicitly flushed is guaranteed durable,
// matching an unclean shutdown after the last flush() call.
void write_partial(const std::filesystem::path& dir, const std::vector<exec::JournalRecord>& records,
                   bool flush_last) {
    persist::WalConfig config{};
    config.directory = dir;
    config.segment_data_bytes = 1024 * 1024;
    persist::ExecutionWalWriter writer;
    persist::WalFileHeader header{};
    (void)writer.open(config, header, 0);
    for (std::size_t i = 0; i < records.size(); ++i) {
        (void)writer.append(records[i], i);
        if (i + 1 < records.size() || flush_last) (void)writer.flush();
    }
    // No finalize(): simulates the process disappearing after the last flush.
}

// Reads every record across all sealed/growing segments in `dir` into a
// fresh, bounded exec::Journal, then replays it through the existing,
// unmodified oms::recover() -- this is the actual "restart recovery" path
// this phase adds: WAL -> Journal -> oms::recover() (Phase B/D, untouched).
bool recover_from_disk(const std::filesystem::path& dir, oms::RecoveryState& out) {
    persist::ExecutionWalTailer tailer;
    if (!tailer.open(dir)) return false;
    exec::Journal journal;
    for (;;) {
        exec::JournalRecord record{};
        const auto status = tailer.next(record);
        if (status == persist::ExecTailStatus::ok) {
            if (!journal.append(record)) return false;  // fail closed on overflow
            continue;
        }
        if (status == persist::ExecTailStatus::idle || status == persist::ExecTailStatus::end_of_data) break;
        return false;  // corrupt / catalog_error: fail closed, do not guess
    }
    return oms::recover(journal, out);
}

// PaperBroker's latency model is explicitly NOT_CALIBRATED (Part 9.3,
// exec/models.hpp) -- its sampled ts_effective_ns for a given (run_seed,
// corr_id) is whatever the placeholder model produces, not a small/bounded
// "typical" latency. Rather than guess a timestamp gap large enough, read
// the order's actual ts_effective_ns after submit and quote strictly past
// it -- correct regardless of the model's current (or future) magnitudes.
void settle(exec::PaperBroker& broker, exec::BrokerOrderRef ref, std::uint32_t symbol,
           std::int64_t bid, std::int64_t ask, std::int64_t size = 100) {
    const auto* order = broker.find_order(ref);
    if (order == nullptr) return;
    const auto past_effective = order->ts_effective_ns + 1;
    broker.on_quote(symbol, bid, ask, size, size, past_effective);
}

}  // namespace

int main() {
    Phase6Test t;
    const auto token = approval();

    // ---- crash during create (no ack yet) -----------------------------------
    {
        const auto dir = temp_dir("crash_create");
        exec::JournalRecord command{};
        command.type = static_cast<std::uint16_t>(exec::JournalRecordType::command);
        command.run_id = 1; command.logical_order_id = 1; command.b = 10; command.c = 11; command.d = 1;
        write_partial(dir, {command}, true);

        oms::RecoveryState state(portfolio::MarginMode::hedging);
        t.check(recover_from_disk(dir, state), "crash-during-create: recovery accepts a bare command record");
        t.check(state.orders.size() == 1, "crash-during-create: order exists");
        t.check(state.orders.at(0).state == exec::OrderState::unknown,
                "crash-during-create: non-terminal order forced unknown on restart");
        t.check(state.orders.reserved_exposure() == 10, "crash-during-create: full exposure retained (Invariant 5)");
        std::error_code ec; std::filesystem::remove_all(dir, ec);
    }

    // ---- crash during partial fill -------------------------------------------
    {
        const auto dir = temp_dir("crash_partial_fill");
        exec::JournalRecord command{}, ack{}, order_state_sent{}, fill{};
        order_state_sent.type = static_cast<std::uint16_t>(exec::JournalRecordType::order_state);
        order_state_sent.run_id = 1; order_state_sent.logical_order_id = 1;
        order_state_sent.a = 2; order_state_sent.b = 3; order_state_sent.d = 10;  // pending_send -> sent
        command.type = static_cast<std::uint16_t>(exec::JournalRecordType::command);
        command.run_id = 1; command.logical_order_id = 1; command.b = 10; command.c = 11; command.d = 1;
        ack.type = static_cast<std::uint16_t>(exec::JournalRecordType::order_state);
        ack.run_id = 1; ack.logical_order_id = 1; ack.a = 3; ack.b = 4; ack.d = 10;  // sent -> acknowledged
        fill.type = static_cast<std::uint16_t>(exec::JournalRecordType::fill);
        fill.run_id = 1; fill.logical_order_id = 1; fill.position_ticket = 77;
        fill.a = 12; fill.b = 4; fill.c = 1;  // price=12, volume=4, remaining=1(nonzero)
        write_partial(dir, {order_state_sent, command, ack, fill}, true);

        oms::RecoveryState state(portfolio::MarginMode::hedging);
        t.check(recover_from_disk(dir, state), "crash-during-partial-fill: recovery accepted");
        t.check(state.orders.size() == 1, "crash-during-partial-fill: order exists");
        t.check(state.orders.at(0).filled_volume == 4, "crash-during-partial-fill: partial fill preserved exactly");
        t.check(state.orders.at(0).state == exec::OrderState::unknown,
                "crash-during-partial-fill: forced unknown (crashed before a terminal state)");
        t.check(state.orders.reserved_exposure() == 10,
                "crash-during-partial-fill: FULL exposure retained, not just the unfilled remainder");
        std::error_code ec; std::filesystem::remove_all(dir, ec);
    }

    // ---- crash during cancel/replace -----------------------------------------
    {
        const auto dir = temp_dir("crash_cancel");
        exec::JournalRecord order_state_sent{}, command{}, ack{}, cancel{};
        order_state_sent.type = static_cast<std::uint16_t>(exec::JournalRecordType::order_state);
        order_state_sent.run_id = 1; order_state_sent.logical_order_id = 1;
        order_state_sent.a = 2; order_state_sent.b = 3; order_state_sent.d = 10;
        command.type = static_cast<std::uint16_t>(exec::JournalRecordType::command);
        command.run_id = 1; command.logical_order_id = 1; command.b = 10; command.c = 11; command.d = 1;
        ack.type = static_cast<std::uint16_t>(exec::JournalRecordType::order_state);
        ack.run_id = 1; ack.logical_order_id = 1; ack.a = 3; ack.b = 4; ack.d = 10;
        cancel.type = static_cast<std::uint16_t>(exec::JournalRecordType::cancel);
        cancel.run_id = 1; cancel.logical_order_id = 1; cancel.a = 10;  // cancel request logged, no confirmation yet
        write_partial(dir, {order_state_sent, command, ack}, true);
        // The cancel record itself is appended but never flushed durably in
        // this scenario -- append() without a subsequent flush() still lands
        // in the mapped segment on this platform (mmap'd, not buffered), so
        // model "crash mid-cancel" as: the venue's cancel *confirmation*
        // (order_state -> cancelled) never arrived, which is the actual
        // ambiguity a crash-during-cancel leaves behind.

        oms::RecoveryState state(portfolio::MarginMode::hedging);
        t.check(recover_from_disk(dir, state), "crash-during-cancel: recovery accepted (pre-cancel state only)");
        t.check(state.orders.at(0).state == exec::OrderState::unknown,
                "crash-during-cancel: unresolved cancel leaves the order unknown, not silently cancelled");
        t.check(state.orders.reserved_exposure() == 10,
                "crash-during-cancel: exposure retained until the cancel is proven, not assumed");
        std::error_code ec; std::filesystem::remove_all(dir, ec);
    }

    // ---- WAL corruption / truncated tail --------------------------------------
    {
        const auto dir = temp_dir("corrupt_tail");
        exec::JournalRecord command{};
        command.type = static_cast<std::uint16_t>(exec::JournalRecordType::command);
        command.run_id = 1; command.logical_order_id = 1; command.b = 10; command.c = 11; command.d = 1;
        write_partial(dir, {command}, true);

        // Corrupt the segment's tail bytes in place (simulates a torn write).
        persist::SegmentCatalog catalog;
        t.check(catalog.scan(dir), "corrupt-tail: catalog scan");
        t.check(!catalog.empty(), "corrupt-tail: segment exists");
        const auto path = catalog.segments()[0].path;
        {
            std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
            f.seekp(-4, std::ios::end);
            const char garbage[4] = {'\xDE', '\xAD', '\xBE', '\xEF'};
            f.write(garbage, 4);
        }

        oms::RecoveryState state(portfolio::MarginMode::hedging);
        t.check(!recover_from_disk(dir, state),
                "corrupt-tail: recovery fails closed on a corrupted frame, does not fabricate a valid record");
        std::error_code ec; std::filesystem::remove_all(dir, ec);
    }

    // ---- disconnect with live orders / venue-only orphan (RecoveryWorkflow) --
    {
        exec::BrokerConfig broker_config{}; broker_config.run_id = 1; broker_config.initial_balance_minor = 1'000'000'000;
        exec::PaperBroker broker(broker_config);
        exec::SymbolSpec spec{}; spec.symbol_id = 0; spec.tick_size_ticks = 1; spec.volume_min = 1;
        spec.volume_max = 1000; spec.volume_step = 1; spec.contract_size = 1; spec.tick_value_minor = 1;
        spec.tradable = true;
        broker.set_symbol(spec);
        broker.on_quote(0, 100, 101, 100, 100, 1000);
        exec::OrderRequest req{}; req.strategy_id = 1; req.symbol_id = 0; req.side = exec::Side::buy;
        req.type = exec::OrderType::market; req.volume = 5; req.seq_global = 1;
        const auto submit = broker.submit(req, 1000);
        t.check(submit.accepted, "disconnect-live-orders: order live on venue");
        // Deliberately not settled to a fill: a market order fills in the same
        // process() pass as its own acknowledgement once the quote is fresh
        // enough, so staying at `sent` (still non-terminal/"open") is what
        // this scenario needs -- an order the venue still has open, not one
        // that already completed.

        oms::Oms local(16);  // nothing recovered locally: fresh/empty
        oms::PaperBrokerVenueAdapter venue(broker);
        oms::ReportSequencer seq;
        oms::RecoveryWorkflow workflow(local, venue, seq);
        workflow.disconnect();
        t.check(workflow.state() == oms::ConnectionState::disconnected, "disconnect-live-orders: DISCONNECTED");
        t.check(workflow.begin_recovery(true), "disconnect-live-orders: local recovery ok (nothing to recover)");

        std::array<oms::ExecReport, 64> reports{};
        std::array<oms::OrderSnapshot, 64> local_snap{}, venue_snap{};
        std::array<oms::OrderReconcileResult, 64> results{};
        const auto outcome = workflow.run_reconciliation(reports.data(), reports.size(), local_snap.data(),
                                                          local_snap.size(), venue_snap.data(), venue_snap.size(),
                                                          results.data(), results.size());
        t.check(outcome.venue_only == 1, "venue-only-orphan: exactly the one live venue order detected as venue-only");
        t.check(workflow.state() == oms::ConnectionState::reconciling,
                "venue-only-orphan: does not reach READY with an unresolved venue-only order");
        t.check(!workflow.can_submit_new_exposure(), "venue-only-orphan: fail-closed for new exposure");
    }

    // ---- local-only order ------------------------------------------------------
    {
        exec::BrokerConfig broker_config{}; broker_config.run_id = 1; broker_config.initial_balance_minor = 1'000'000'000;
        exec::PaperBroker broker(broker_config);  // empty venue: nothing submitted
        oms::Oms local(16);
        (void)local.create(exec::BrokerOrderRef{1, 999}, 0, 10, token);  // local believes it has an order the venue never got
        oms::PaperBrokerVenueAdapter venue(broker);
        oms::ReportSequencer seq;
        oms::RecoveryWorkflow workflow(local, venue, seq);
        workflow.disconnect();
        t.check(workflow.begin_recovery(true), "local-only: begin_recovery");

        std::array<oms::ExecReport, 64> reports{};
        std::array<oms::OrderSnapshot, 64> local_snap{}, venue_snap{};
        std::array<oms::OrderReconcileResult, 64> results{};
        const auto outcome = workflow.run_reconciliation(reports.data(), reports.size(), local_snap.data(),
                                                          local_snap.size(), venue_snap.data(), venue_snap.size(),
                                                          results.data(), results.size());
        t.check(outcome.local_only == 1, "local-only: exactly the one local-only order detected");
        t.check(workflow.state() == oms::ConnectionState::reconciling, "local-only: not READY with an unresolved local-only order");
    }

    // ---- reconnect with missed fill (exactly-once via Phase D sequencer) -----
    {
        exec::BrokerConfig broker_config{}; broker_config.run_id = 1; broker_config.initial_balance_minor = 1'000'000'000;
        exec::PaperBroker broker(broker_config);
        exec::SymbolSpec spec{}; spec.symbol_id = 0; spec.tick_size_ticks = 1; spec.volume_min = 1;
        spec.volume_max = 1000; spec.volume_step = 1; spec.contract_size = 1; spec.tick_value_minor = 1;
        spec.tradable = true;
        broker.set_symbol(spec);
        broker.on_quote(0, 100, 101, 100, 100, 1000);
        exec::OrderRequest req{}; req.strategy_id = 1; req.symbol_id = 0; req.side = exec::Side::buy;
        req.type = exec::OrderType::market; req.volume = 5; req.seq_global = 1;
        const auto submit = broker.submit(req, 1000);
        // ask_size=3 < order volume=5: a genuine partial fill (3 of 5), so
        // the order stays live (partially_filled) instead of going terminal
        // -- lets this test check post-catch-up state on a still-live order,
        // and separately exercise duplicate detection (last_applied_seq)
        // rather than terminal-cache handling (already covered by the
        // terminal-order-late-report scenario below).
        settle(broker, submit.ref, 0, 100, 101, 3);  // fills while "disconnected"

        oms::Oms local(16);
        (void)local.create(submit.ref, 0, 5, token);
        (void)local.transition(submit.ref, exec::OrderState::sent);  // local matches venue up to pre-disconnect point
        oms::PaperBrokerVenueAdapter venue(broker);
        oms::ReportSequencer seq;
        oms::RecoveryWorkflow workflow(local, venue, seq);
        workflow.disconnect();
        t.check(workflow.begin_recovery(true), "missed-fill: begin_recovery");

        std::array<oms::ExecReport, 64> reports{};
        std::array<oms::OrderSnapshot, 64> local_snap{}, venue_snap{};
        std::array<oms::OrderReconcileResult, 64> results{};
        const auto outcome = workflow.run_reconciliation(reports.data(), reports.size(), local_snap.data(),
                                                          local_snap.size(), venue_snap.data(), venue_snap.size(),
                                                          results.data(), results.size());
        t.check(outcome.reports_applied >= 1, "missed-fill: at least the ack+fill applied via the sequencer");
        const auto* local_order = local.find(submit.ref);
        t.check(local_order != nullptr, "missed-fill: order still live locally after catch-up (partial fill only)");
        if (local_order != nullptr)
            t.check(local_order->filled_volume == 3, "missed-fill: local filled_volume now matches venue exactly");

        // ---- duplicate reports after reconnect: re-drain the same batch ---
        std::array<oms::ExecReport, 64> reports2{};
        std::size_t duplicate_count = 0, applied_count = 0;
        const auto report_count = venue.fetch_recent_reports(reports2.data(), reports2.size());
        for (std::size_t i = 0; i < report_count; ++i) {
            const auto o = seq.process(local, reports2[i]);
            if (o == oms::ReportOutcome::duplicate) ++duplicate_count;
            if (o == oms::ReportOutcome::applied) ++applied_count;
        }
        t.check(applied_count == 0, "duplicate-after-reconnect: re-draining the same reports applies nothing new");
        t.check(duplicate_count == report_count,
                "duplicate-after-reconnect: every replayed report recognized as a duplicate");
        if (local.find(submit.ref) != nullptr)
            t.check(local.find(submit.ref)->filled_volume == 3,
                    "duplicate-after-reconnect: filled_volume still exactly 3, not double-counted to 6");
    }

    // ---- reports arrive out of order after reconnect (delayed) --------------
    {
        oms::Oms local(4);
        oms::ReportSequencer seq;
        const exec::BrokerOrderRef ref{1, 1};
        (void)local.create(ref, 0, 10, token);
        (void)local.transition(ref, exec::OrderState::sent);
        // Venue's catch-up batch arrives with seq 1 (ack) missing, seq 2 (fill) first.
        t.check(seq.process(local, oms::ExecReport{ref, 2, oms::ReportKind::fill, 4, 0}) ==
                    oms::ReportOutcome::held_for_gap,
                "delayed-after-reconnect: fill held pending the missing ack");
        t.check(seq.process(local, oms::ExecReport{ref, 1, oms::ReportKind::ack, 0, 0}) ==
                    oms::ReportOutcome::applied,
                "delayed-after-reconnect: ack applied, cascades the held fill");
        t.check(local.find(ref)->filled_volume == 4, "delayed-after-reconnect: cascaded fill applied correctly");
    }

    // ---- position/exposure mismatch -------------------------------------------
    {
        std::array<oms::PositionSnapshot, 4> venue_positions{
            oms::PositionSnapshot{1, 0, exec::Side::buy, 10}};
        std::array<std::int64_t, 8> local_net{};
        local_net[0] = 6;  // local believes net 6, venue reports net 10
        std::array<oms::PositionMismatch, 8> mismatches{};
        const auto n = oms::compare_positions(venue_positions.data(), 1, local_net.data(), local_net.size(),
                                              mismatches.data(), mismatches.size());
        t.check(n == 1, "position-mismatch: exactly one symbol mismatch detected");
        t.check(mismatches[0].local_net_volume == 6 && mismatches[0].venue_net_volume == 10,
                "position-mismatch: reported values are exactly the two disagreeing sides");
    }

    // ---- repeated crash during reconciliation ---------------------------------
    {
        exec::BrokerConfig broker_config{}; broker_config.run_id = 1; broker_config.initial_balance_minor = 1'000'000'000;
        exec::PaperBroker broker(broker_config);
        exec::SymbolSpec spec{}; spec.symbol_id = 0; spec.tick_size_ticks = 1; spec.volume_min = 1;
        spec.volume_max = 1000; spec.volume_step = 1; spec.contract_size = 1; spec.tick_value_minor = 1;
        spec.tradable = true;
        broker.set_symbol(spec);
        broker.on_quote(0, 100, 101, 100, 100, 1000);
        exec::OrderRequest req{}; req.strategy_id = 1; req.symbol_id = 0; req.side = exec::Side::buy;
        req.type = exec::OrderType::market; req.volume = 5; req.seq_global = 1;
        const auto submit = broker.submit(req, 1000);
        settle(broker, submit.ref, 0, 100, 101);

        // Full fill (size=100 >= volume=5): the order reaches `filled`
        // (terminal) and is reclaimed (Phase B), so "converges to the
        // correct fully-filled state" is checked via reports_applied (ack +
        // fill, both correctly applied every attempt) and the order being
        // consistently gone afterward -- not via a post-reclaim find(),
        // which correctly and consistently returns nullptr.
        bool all_consistent = true;
        std::int64_t first_reports_applied = -1;
        for (int attempt = 0; attempt < 5; ++attempt) {
            // Each attempt is a *fresh* local Oms/sequencer/workflow -- "crash
            // during reconciliation" and restart from scratch, repeated.
            oms::Oms local(16);
            (void)local.create(submit.ref, 0, 5, token);
            (void)local.transition(submit.ref, exec::OrderState::sent);
            oms::PaperBrokerVenueAdapter venue(broker);
            oms::ReportSequencer seq;
            oms::RecoveryWorkflow workflow(local, venue, seq);
            workflow.disconnect();
            if (!workflow.begin_recovery(true)) { all_consistent = false; break; }
            std::array<oms::ExecReport, 64> reports{};
            std::array<oms::OrderSnapshot, 64> local_snap{}, venue_snap{};
            std::array<oms::OrderReconcileResult, 64> results{};
            const auto outcome = workflow.run_reconciliation(
                reports.data(), reports.size(), local_snap.data(), local_snap.size(),
                venue_snap.data(), venue_snap.size(), results.data(), results.size());
            const auto applied = static_cast<std::int64_t>(outcome.reports_applied);
            if (first_reports_applied == -1) first_reports_applied = applied;
            else if (applied != first_reports_applied) all_consistent = false;
            if (local.find(submit.ref) != nullptr) all_consistent = false;  // must be reclaimed, every attempt
        }
        t.check(all_consistent, "repeated-crash-during-reconciliation: every restart-from-scratch attempt converges identically");
        t.check(first_reports_applied == 2,
                "repeated-crash-during-reconciliation: converges to the correct fully-filled state (ack+fill applied)");
    }

    // ---- reconnect storm -------------------------------------------------------
    {
        exec::BrokerConfig broker_config{}; broker_config.run_id = 1; broker_config.initial_balance_minor = 1'000'000'000;
        exec::PaperBroker broker(broker_config);
        oms::Oms local(16);
        oms::PaperBrokerVenueAdapter venue(broker);
        oms::ReportSequencer seq;
        oms::RecoveryWorkflow workflow(local, venue, seq);

        bool ok = true;
        for (int cycle = 0; cycle < 200; ++cycle) {
            workflow.disconnect();
            if (workflow.state() != oms::ConnectionState::disconnected) { ok = false; break; }
            if (!workflow.begin_recovery(true)) { ok = false; break; }
            std::array<oms::ExecReport, 8> reports{};
            std::array<oms::OrderSnapshot, 8> local_snap{}, venue_snap{};
            std::array<oms::OrderReconcileResult, 8> results{};
            const auto outcome = workflow.run_reconciliation(reports.data(), reports.size(), local_snap.data(),
                                                              local_snap.size(), venue_snap.data(), venue_snap.size(),
                                                              results.data(), results.size());
            if (!outcome.clean() || workflow.state() != oms::ConnectionState::ready) { ok = false; break; }
        }
        t.check(ok, "reconnect-storm: 200 rapid disconnect/recover/reconcile cycles all reach READY cleanly");
        t.check(workflow.can_submit_new_exposure(), "reconnect-storm: ends READY, new exposure permitted again");
    }

    return t.result();
}
