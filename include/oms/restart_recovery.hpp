#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "oms/multi_venue.hpp"
#include "oms/recovery_streaming.hpp"
#include "persist/execution_wal.hpp"
#include "risk/risk_ledger.hpp"

// Phase J (blocker fix) -- the restart orchestration: WAL replay ->
// repopulate a fresh ShardedOms -> reconstruct risk state -> venue
// reconciliation -> READY. Composes existing, mostly-unmodified components
// end to end, adding no new admission-control logic of its own:
//   - persist::ExecutionWalTailer (Phase E, unmodified): reads the durable WAL.
//   - oms::recover_streaming() (this phase's capacity-ceiling fix): reconstructs
//     order state at any scale, not capped at exec::max_journal_records.
//   - ShardedOms::submit_restore() (this phase, additive): repopulates a
//     fresh venue ShardedOms with exactly what was recovered, including
//     `unknown` states recover_streaming() itself produces for every
//     order that was live (non-terminal) at crash time (Phase E's orphan-
//     exposure-retention invariant, unchanged).
//   - risk::RiskLedger::rebuild_from_snapshot() (Phase I, unmodified):
//     reconstructs exposure/position from the now-repopulated ShardedOms's
//     live orders via the existing OpKind::snapshot mechanism.
//   - VenueConnection's existing ConnectionState machine (Phase F, entirely
//     unmodified): disconnect() -> begin_recovery() -> RECONCILING ->
//     run_reconciliation() -> READY only if outcome.clean().
//
// "No new exposure may be admitted after restart until replay +
// reconciliation reaches READY" is not new logic this function adds -- it
// is the direct, structural consequence of driving VenueConnection through
// its already-existing, already-tested state machine: route_create()/
// route_replace() already refuse new exposure in every state except
// CONNECTED/READY (VenueConnection::connection_allows_new_exposure(), Phase
// F/G, unmodified), and this function never sets the state to either of
// those itself -- only VenueConnection::run_reconciliation() can, and only
// when its own outcome.clean().
//
// ---- cross-shard ordering, found and fixed during this phase's own -------
// ---- benchmarking (not something a single-order test could expose) -------
//
// ShardedOms::emit_wal() (sharded_oms.hpp) uses ts_ns = global_seq_ -- a
// single, process-wide, strictly-increasing counter shared by every shard --
// so any ONE order's own record sequence is correctly ordered by
// construction (that order's events only ever apply on one shard, in real
// application order). But persist::LiveWalRecorder drains N shards' outbox
// rings round-robin into ONE combined WAL file: a full pass over shard 0
// then shard 1 can legitimately interleave ts_ns values non-monotonically
// in the *file's own* record order (shard 1 may have claimed a lower
// global_seq than shard 0's most recent batch, simply by winning a
// scheduling race). oms::recover_streaming()'s (and oms::recover()'s own,
// unmodified) monotonic-ts_ns check assumes its input stream already IS in
// that order -- correct for a single-shard/single-order source (every test
// in this phase that predates this fix only ever exercised one order), but
// not guaranteed by a multi-shard live WAL. The fix belongs here, not in
// recover_streaming() itself (which stays a correct, reusable, order-
// agnostic consumer of an *already-ordered* stream): read every record
// first, stable_sort by ts_ns (stable so multiple records sharing one
// operation's ts_ns, e.g. an order_state + its confirmation record, keep
// their emitted relative order), then replay. A temporary
// std::vector<JournalRecord> sized to the actual record count (not a fixed
// cap) is exactly the "bounded/preallocated-at-the-scale-that-matters, not
// a giant fixed array" storage Blocker 2 asked for -- recovery is
// explicitly not the hot path (unchanged philosophy since Phase E), and
// 1,000,000 records is 80 MB, trivial to hold once, here, for a sort.
[[nodiscard]] inline bool read_and_order_wal(const std::filesystem::path& wal_directory,
                                             std::vector<exec::JournalRecord>& out) noexcept {
    persist::ExecutionWalTailer tailer;
    if (!tailer.open(wal_directory)) return false;
    for (;;) {
        exec::JournalRecord rec{};
        const auto status = tailer.next(rec);
        if (status == persist::ExecTailStatus::ok) { out.push_back(rec); continue; }
        if (status == persist::ExecTailStatus::idle || status == persist::ExecTailStatus::end_of_data) break;
        return false;  // corrupt/catalog_error: fail closed, never replay a partially-read torn tail
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const exec::JournalRecord& a, const exec::JournalRecord& b) { return a.ts_ns < b.ts_ns; });
    return true;
}

namespace oms {

struct RestartRecoveryResult final {
    bool wal_replay_ok{false};
    std::size_t recovered_order_count{0};
    std::size_t restored_into_shard_count{0};
    ReconciliationOutcome reconciliation{};
    bool reached_ready{false};
};

[[nodiscard]] inline RestartRecoveryResult restart_recover_venue(
    const std::filesystem::path& wal_directory, VenueConnection& conn, risk::RiskLedger& ledger,
    std::size_t producer_id, std::size_t oms_recovery_capacity, std::size_t dedup_window,
    ExecReport* report_scratch, std::size_t report_capacity, exec::Order* snapshot_scratch,
    std::size_t snapshot_capacity, OrderSnapshot* local_scratch, std::size_t local_capacity,
    OrderSnapshot* venue_scratch, std::size_t venue_capacity, OrderReconcileResult* result_out,
    std::size_t result_capacity) {
    RestartRecoveryResult result{};
    conn.disconnect();  // DISCONNECTED first, unconditionally -- the fail-closed floor everything below builds on.

    std::vector<exec::JournalRecord> ordered;
    if (!read_and_order_wal(wal_directory, ordered)) return result;  // stays DISCONNECTED
    std::size_t pos = 0;
    auto source = [&](exec::JournalRecord& rec) -> StreamStatus {
        if (pos >= ordered.size()) return StreamStatus::done;
        rec = ordered[pos++];
        return StreamStatus::ok;
    };
    RecoveryState recovered(portfolio::MarginMode::hedging, oms_recovery_capacity);
    result.wal_replay_ok = recover_streaming(source, recovered, dedup_window);
    if (!result.wal_replay_ok) return result;  // stays DISCONNECTED -- a corrupt/unreplayable WAL never reaches READY
    result.recovered_order_count = recovered.orders.size();

    if (!conn.begin_recovery()) return result;  // -> RECONCILING

    for (std::size_t i = 0; i < recovered.orders.size(); ++i) {
        const auto& o = recovered.orders.at(i);
        Completion c{};
        if (conn.oms().submit_restore(producer_id, o.ref, o.symbol_id, o.requested_volume, o.filled_volume,
                                      o.limit_price_ticks, o.state, c) &&
            c.ok)
            ++result.restored_into_shard_count;
    }

    std::size_t snapshot_total = 0;
    for (std::size_t s = 0; s < conn.oms().shard_count() && snapshot_total < snapshot_capacity; ++s) {
        Completion snap{};
        if (conn.oms().submit_snapshot(producer_id, s, snapshot_scratch + snapshot_total,
                                       snapshot_capacity - snapshot_total, snap))
            snapshot_total += snap.snapshot_count;
    }
    ledger.rebuild_from_snapshot(conn.id(), snapshot_scratch, snapshot_total);

    result.reconciliation = conn.run_reconciliation(producer_id, report_scratch, report_capacity, snapshot_scratch,
                                                     snapshot_capacity, local_scratch, local_capacity, venue_scratch,
                                                     venue_capacity, result_out, result_capacity);
    result.reached_ready = (conn.state() == ConnectionState::ready);
    return result;
}

}  // namespace oms
