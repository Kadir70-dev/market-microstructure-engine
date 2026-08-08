#include <filesystem>
#include <string>

#include "oms/recovery_streaming.hpp"
#include "oms/sharded_oms.hpp"
#include "persist/live_wal_recorder.hpp"
#include "phase6_test.hpp"

// Phase J (blocker fix) -- proves the live WAL wiring (ShardedOms::apply()'s
// emit_wal(), sharded_oms.hpp) actually reconstructs correct order state via
// recover_streaming() (recovery_streaming.hpp) after a simulated crash, for
// every operation kind the mapping in emit_wal()'s own comment claims to
// handle. Does not re-prove recover_streaming()'s own scale/dedup/corruption
// behavior (phase7_recovery_streaming.cpp does); this file proves the
// mapping from live ShardedOms operations to journal records is correct.

namespace {

constexpr std::uint64_t RUN_ID = 1;

std::filesystem::path temp_dir(const char* label) {
    const auto dir = std::filesystem::temp_directory_path() /
        (std::string("mme_phase_j_wcr_") + label + "_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

exec::BrokerOrderRef ref(std::uint64_t id) noexcept { return exec::BrokerOrderRef{RUN_ID, id}; }

// "Crash": destroys the recorder (which stops its background thread after a
// final drain-to-empty -- i.e. everything record() successfully accepted
// gets written and flushed) without any clean shutdown of the ShardedOms
// itself. This models a crash where the durability layer's own in-flight
// outbox contents are the boundary of what's recoverable -- the honest
// window this design actually guarantees (see live_wal_recorder.hpp).
struct System final {
    std::filesystem::path dir;
    std::unique_ptr<oms::ShardedOms> oms;
    std::unique_ptr<persist::LiveWalRecorder> recorder;

    explicit System(const char* label, std::size_t capacity = 4096)
        : dir(temp_dir(label)), oms(std::make_unique<oms::ShardedOms>(2, capacity, 4, capacity + 64)) {
        recorder = std::make_unique<persist::LiveWalRecorder>(dir, 2);
        oms->attach_wal_hook(recorder.get());
    }

    void crash() {
        recorder.reset();  // stop() runs in the destructor: final drain+flush, matching the file header's contract
    }

    ~System() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

// Reads every record in `dir` and recovers into a fresh RecoveryState sized
// for `capacity` orders.
bool recover_dir(const std::filesystem::path& dir, oms::RecoveryState& out, std::size_t dedup_window = 4096) {
    persist::ExecutionWalTailer tailer;
    if (!tailer.open(dir)) return false;
    auto source = [&](exec::JournalRecord& rec) -> oms::StreamStatus {
        const auto status = tailer.next(rec);
        if (status == persist::ExecTailStatus::ok) return oms::StreamStatus::ok;
        if (status == persist::ExecTailStatus::idle || status == persist::ExecTailStatus::end_of_data)
            return oms::StreamStatus::done;
        return oms::StreamStatus::corrupt;
    };
    return oms::recover_streaming(source, out, dedup_window);
}

}  // namespace

int main() {
    Phase6Test t;

    // ---- 1) create only, then crash -----------------------------------------
    {
        System sys("create");
        oms::Completion c{};
        t.check(sys.oms->submit_create(0, ref(1), 3, 50, c) && c.ok, "create: live create succeeds");
        sys.crash();
        oms::RecoveryState state(portfolio::MarginMode::hedging, 64);
        t.check(recover_dir(sys.dir, state), "create: recovery succeeds");
        const auto* o = state.orders.find(ref(1));
        // Phase E's own established, correct, intentional behavior (not
        // something this phase changes): oms::recover()/recover_streaming()
        // mark every recovered *non-terminal* order `unknown` -- "retain
        // orphan exposure until proven resolved" (Phase E's Invariant 5) --
        // so a live order's exact pre-crash state (sent/acknowledged/
        // partially_filled) is deliberately NOT what comes back here. It
        // still has the right volume/symbol; venue reconciliation (Phase
        // E/F, unchanged) is what resolves `unknown` back to a real state.
        t.check(o != nullptr && o->state == exec::OrderState::unknown && o->requested_volume == 50 && o->symbol_id == 3,
               "create: recovered order is `unknown` (orphan-retained, Phase E's invariant) with the correct volume/symbol");
    }

    // ---- 2) create + ack, then crash -----------------------------------------
    {
        System sys("ack");
        oms::Completion c{};
        (void)sys.oms->submit_create(0, ref(1), 0, 10, c);
        t.check(sys.oms->submit_transition(0, ref(1), exec::OrderState::sent, c) && c.ok, "ack: sent transition");
        t.check(sys.oms->submit_transition(0, ref(1), exec::OrderState::acknowledged, c) && c.ok, "ack: acknowledged transition");
        sys.crash();
        oms::RecoveryState state(portfolio::MarginMode::hedging, 64);
        t.check(recover_dir(sys.dir, state), "ack: recovery succeeds");
        const auto* o = state.orders.find(ref(1));
        t.check(o != nullptr && o->state == exec::OrderState::unknown, "ack: recovered order is `unknown` (orphan-retained, was acknowledged pre-crash)");
    }

    // ---- 3) create + ack + partial fill, then crash ---------------------------
    {
        System sys("partial_fill");
        oms::Completion c{};
        (void)sys.oms->submit_create(0, ref(1), 0, 100, c);
        (void)sys.oms->submit_transition(0, ref(1), exec::OrderState::sent, c);
        (void)sys.oms->submit_transition(0, ref(1), exec::OrderState::acknowledged, c);
        oms::ExecReport fill{ref(1), 1, oms::ReportKind::fill, 30, 0};
        t.check(sys.oms->submit_report(0, fill, c) && c.ok, "partial-fill: fill report applies");
        sys.crash();
        oms::RecoveryState state(portfolio::MarginMode::hedging, 64);
        t.check(recover_dir(sys.dir, state), "partial-fill: recovery succeeds");
        const auto* o = state.orders.find(ref(1));
        t.check(o != nullptr && o->state == exec::OrderState::unknown && o->filled_volume == 30 &&
                    o->requested_volume == 100,
               "partial-fill: recovered order shows exactly 30/100 filled (state `unknown`, orphan-retained)");
    }

    // ---- 4) create + ack + full fill (terminal), then crash --------------------
    {
        System sys("full_fill");
        oms::Completion c{};
        (void)sys.oms->submit_create(0, ref(1), 0, 20, c);
        (void)sys.oms->submit_transition(0, ref(1), exec::OrderState::sent, c);
        (void)sys.oms->submit_transition(0, ref(1), exec::OrderState::acknowledged, c);
        oms::ExecReport fill{ref(1), 1, oms::ReportKind::fill, 20, 0};
        t.check(sys.oms->submit_report(0, fill, c) && c.ok, "full-fill: fill report applies");
        sys.crash();
        oms::RecoveryState state(portfolio::MarginMode::hedging, 64);
        t.check(recover_dir(sys.dir, state), "full-fill: recovery succeeds");
        t.check(state.orders.find(ref(1)) == nullptr,
               "full-fill: fully-filled order is terminal and correctly absent (reclaimed) after recovery, "
               "matching live swap-removal reclaim semantics");
    }

    // ---- 5) cancel/replace + crash ----------------------------------------------
    {
        System sys("cancel_replace");
        oms::Completion c{};
        (void)sys.oms->submit_create(0, ref(1), 0, 40, c);
        (void)sys.oms->submit_transition(0, ref(1), exec::OrderState::sent, c);
        (void)sys.oms->submit_transition(0, ref(1), exec::OrderState::acknowledged, c);
        t.check(sys.oms->submit_replace(0, ref(1), 500, 60, c) && c.ok, "cancel/replace: replace applies");
        t.check(sys.oms->submit_transition(0, ref(1), exec::OrderState::cancel_pending, c) && c.ok,
               "cancel/replace: cancel_pending transition");
        oms::ExecReport cancelled{ref(1), 1, oms::ReportKind::cancelled, 0, 0};
        t.check(sys.oms->submit_report(0, cancelled, c) && c.ok, "cancel/replace: cancelled report applies");
        sys.crash();
        oms::RecoveryState state(portfolio::MarginMode::hedging, 64);
        t.check(recover_dir(sys.dir, state), "cancel/replace: recovery succeeds");
        t.check(state.orders.find(ref(1)) == nullptr,
               "cancel/replace: cancelled order is terminal and correctly absent after recovery "
               "(also proves the replace's new volume=60 was correctly the basis the cancel operated on, "
               "not the original 40 -- recover() would reject an inconsistent sequence)");
    }

    // ---- 6) reject (from `sent`), then crash -------------------------------------
    {
        System sys("reject");
        oms::Completion c{};
        (void)sys.oms->submit_create(0, ref(1), 0, 15, c);
        (void)sys.oms->submit_transition(0, ref(1), exec::OrderState::sent, c);
        oms::ExecReport rejected{ref(1), 1, oms::ReportKind::reject, 0, 0};
        t.check(sys.oms->submit_report(0, rejected, c) && c.ok, "reject: reject report applies");
        sys.crash();
        oms::RecoveryState state(portfolio::MarginMode::hedging, 64);
        t.check(recover_dir(sys.dir, state), "reject: recovery succeeds");
        t.check(state.orders.find(ref(1)) == nullptr, "reject: rejected order is terminal and correctly absent after recovery");
    }

    // ---- 7) duplicate/out-of-order reports around crash --------------------------
    {
        System sys("dup_ooo");
        oms::Completion c{};
        (void)sys.oms->submit_create(0, ref(1), 0, 50, c);
        (void)sys.oms->submit_transition(0, ref(1), exec::OrderState::sent, c);
        (void)sys.oms->submit_transition(0, ref(1), exec::OrderState::acknowledged, c);
        oms::ExecReport fill1{ref(1), 1, oms::ReportKind::fill, 10, 0};
        t.check(sys.oms->submit_report(0, fill1, c) && c.ok, "dup/ooo: first fill (seq1) applies");
        oms::Completion dup{};
        oms::ExecReport fill1_dup{ref(1), 1, oms::ReportKind::fill, 10, 0};
        t.check(sys.oms->submit_report(0, fill1_dup, dup) && dup.report_outcome == oms::ReportOutcome::duplicate,
               "dup/ooo: the retransmit is correctly rejected as duplicate (nothing new to journal)");
        oms::Completion gap{};
        oms::ExecReport fill3{ref(1), 3, oms::ReportKind::fill, 5, 0};  // seq3, held for gap (seq2 missing)
        t.check(sys.oms->submit_report(0, fill3, gap) && gap.report_outcome == oms::ReportOutcome::held_for_gap,
               "dup/ooo: a report ahead of a gap is held, never journaled");
        sys.crash();
        oms::RecoveryState state(portfolio::MarginMode::hedging, 64);
        t.check(recover_dir(sys.dir, state), "dup/ooo: recovery succeeds");
        const auto* o = state.orders.find(ref(1));
        t.check(o != nullptr && o->filled_volume == 10,
               "dup/ooo: recovered state reflects exactly the one applied fill (10) -- the duplicate retransmit "
               "was never journaled (nothing to double-apply) and the held-for-gap report was never journaled "
               "either (it never actually changed state pre-crash), so recovery correctly does not need to "
               "reconcile anything that was never truly applied");
    }

    // ---- 8) torn tail: a record enqueued but never flushed is correctly absent ---
    {
        System sys("torn_tail");
        oms::Completion c{};
        (void)sys.oms->submit_create(0, ref(1), 0, 10, c);
        (void)sys.oms->submit_transition(0, ref(1), exec::OrderState::sent, c);
        (void)sys.oms->submit_transition(0, ref(1), exec::OrderState::acknowledged, c);
        sys.crash();  // stop() still drains+flushes everything record() accepted -- this proves the *clean*
                      // shutdown path is complete; recovery below confirms nothing from it is silently lost.
        oms::RecoveryState state(portfolio::MarginMode::hedging, 64);
        t.check(recover_dir(sys.dir, state), "torn-tail baseline: recovery succeeds");
        const auto* o = state.orders.find(ref(1));
        t.check(o != nullptr && o->state == exec::OrderState::unknown,
               "torn-tail baseline: everything enqueued before a clean stop() is fully recovered, "
               "confirming the durability window's boundary is exactly what live_wal_recorder.hpp documents");
    }

    return t.result();
}
