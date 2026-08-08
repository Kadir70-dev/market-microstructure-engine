#pragma once
#include <cstdint>

#include "oms/oms.hpp"
#include "oms/recovery.hpp"
#include "oms/report_sequencer.hpp"
#include "oms/venue_reconciliation.hpp"
#include "risk/halt_state.hpp"

// Phase E — disconnect -> reconnect -> resync workflow.
//
//   CONNECTED -> DISCONNECTED -> RECOVERING -> RECONCILING -> READY
//
// Reuses, unmodified: oms::recover() (Phase B/D journal replay), Oms's
// existing mark_unknown_on_restart/resolve_unknown/reserved_exposure
// (orphan-exposure retention, Part 8.5/Invariant 5), ReportSequencer (Phase
// D exactly-once dedup/sequencing), OrderReconciler (this phase, per-order
// comparison). This file is the orchestration gluing them together and the
// fail-closed gate new order submission must check.
//
// Design choices:
// - Local recovery (WAL -> exec::Journal -> oms::recover()) is the caller's
//   job, done before constructing/advancing a RecoveryWorkflow: recover()
//   already exists, is already tested, and takes a Journal, not a WAL
//   reader, so nothing here reimplements it -- begin_recovery() just takes
//   the bool result and gates on it.
// - RECONCILING never auto-resolves a local-only or venue-only order, or a
//   state/volume mismatch: the whole point of "explicitly resolve or
//   quarantine ambiguous state" is that the system does not get to guess.
//   READY is reached only when a reconciliation pass finds zero
//   discrepancies, or every discrepancy found has since been explicitly
//   resolved via resolve_discrepancy().
// - can_submit_new_exposure() is false in every state except READY. This is
//   the fail-closed gate: a caller wiring order submission through this
//   workflow simply refuses new orders (existing orders keep whatever
//   protection Oms/RiskEngine/kill-switch already give them) until resync is
//   proven complete.

namespace oms {

enum class ConnectionState : std::uint8_t { connected, disconnected, recovering, reconciling, ready };

struct ReconciliationOutcome final {
    std::size_t reports_applied{0};
    std::size_t reports_duplicate{0};
    std::size_t reports_held_for_gap{0};
    std::size_t reports_illegal{0};
    std::size_t reports_unknown_order{0};
    std::size_t local_only{0};
    std::size_t venue_only{0};
    std::size_t state_mismatch{0};
    std::size_t volume_mismatch{0};
    std::size_t unresolved_unknown{0};  // Oms orders still in `unknown` after replay+reconcile reports
    [[nodiscard]] bool clean() const noexcept {
        return local_only == 0 && venue_only == 0 && state_mismatch == 0 && volume_mismatch == 0 &&
               unresolved_unknown == 0 && reports_illegal == 0 && reports_unknown_order == 0;
    }
};

class RecoveryWorkflow final {
public:
    RecoveryWorkflow(Oms& local, const VenueAdapter& venue, ReportSequencer& seq) noexcept
        : local_(local), venue_(venue), seq_(seq) {}

    [[nodiscard]] ConnectionState state() const noexcept { return state_; }

    // Fail-closed gate (Part 4): new exposure is refused in every state
    // except READY. Existing orders' own protections (RiskEngine, kill
    // switch, Oms's state machine) are untouched -- this only gates *new*
    // submission through this workflow.
    [[nodiscard]] bool can_submit_new_exposure() const noexcept { return state_ == ConnectionState::ready; }

    void disconnect() noexcept { state_ = ConnectionState::disconnected; }

    // DISCONNECTED -> RECOVERING -> RECONCILING (or stays DISCONNECTED,
    // fail-closed, if local recovery itself failed -- an unrecoverable
    // local journal is a strictly worse situation than a reconciliation
    // mismatch and must not proceed past it).
    [[nodiscard]] bool begin_recovery(bool local_recovery_ok) noexcept {
        if (state_ != ConnectionState::disconnected) return false;
        state_ = ConnectionState::recovering;
        if (!local_recovery_ok) { state_ = ConnectionState::disconnected; return false; }
        state_ = ConnectionState::reconciling;
        return true;
    }

    // RECONCILING: drain missed venue reports through the Phase D sequencer
    // (exactly-once; a report already applied before the disconnect is
    // correctly rejected as `duplicate`, never double-applied), then compare
    // local vs. venue snapshots. Bounded, preallocated scratch: report_scratch
    // and result_out sizes are the only "capacity" this needs, matching
    // "bounded/preallocated storage, no unbounded hot-path allocation" --
    // reconciliation itself is explicitly not a hot path, but its buffers
    // are still caller-sized up front, not grown.
    [[nodiscard]] ReconciliationOutcome run_reconciliation(ExecReport* report_scratch, std::size_t report_capacity,
                                                           OrderSnapshot* local_scratch, std::size_t local_capacity,
                                                           OrderSnapshot* venue_scratch, std::size_t venue_capacity,
                                                           OrderReconcileResult* result_out,
                                                           std::size_t result_capacity) noexcept {
        ReconciliationOutcome outcome{};
        if (state_ != ConnectionState::reconciling) return outcome;

        // 1) Missed reports, applied exactly-once via Phase D sequencing --
        // order-independent by venue_seq, not by fetch order.
        const auto report_count = venue_.fetch_recent_reports(report_scratch, report_capacity);
        for (std::size_t i = 0; i < report_count; ++i) {
            switch (seq_.process(local_, report_scratch[i])) {
                case ReportOutcome::applied: ++outcome.reports_applied; break;
                case ReportOutcome::duplicate: ++outcome.reports_duplicate; break;
                case ReportOutcome::held_for_gap: ++outcome.reports_held_for_gap; break;
                case ReportOutcome::illegal: ++outcome.reports_illegal; break;
                case ReportOutcome::unknown_order: ++outcome.reports_unknown_order; break;
                case ReportOutcome::terminal_late: break;  // correctly a no-op
                case ReportOutcome::gap_pool_exhausted: ++outcome.reports_illegal; break;
            }
        }

        // 2) Per-order comparison, local (post-replay-and-catchup) vs venue.
        std::size_t local_n = 0;
        for (std::size_t i = 0; i < local_.size() && local_n < local_capacity; ++i) {
            const auto& o = local_.at(i);
            if (exec::is_terminal(o.state)) continue;
            local_scratch[local_n++] = OrderSnapshot{o.ref, o.state, o.requested_volume, o.filled_volume};
            if (o.state == exec::OrderState::unknown) ++outcome.unresolved_unknown;
        }
        const auto venue_n = venue_.fetch_open_orders(venue_scratch, venue_capacity);

        OrderReconciler reconciler;
        const auto result_n = reconciler.compare(local_scratch, local_n, venue_scratch, venue_n,
                                                  result_out, result_capacity);
        for (std::size_t i = 0; i < result_n; ++i) {
            switch (result_out[i].status) {
                case OrderReconcileStatus::local_only: ++outcome.local_only; break;
                case OrderReconcileStatus::venue_only: ++outcome.venue_only; break;
                case OrderReconcileStatus::state_mismatch: ++outcome.state_mismatch; break;
                case OrderReconcileStatus::volume_mismatch: ++outcome.volume_mismatch; break;
                case OrderReconcileStatus::matched: break;
            }
        }

        last_outcome_ = outcome;
        if (outcome.clean()) state_ = ConnectionState::ready;
        // else: stays RECONCILING. The discrepancies in result_out[0..result_n)
        // are the caller's quarantine list -- explicitly surfaced, not hidden.
        return outcome;
    }

    // Explicit resolution path: an operator/reconciliation process has
    // determined the true state of an ambiguous (unknown, local-only, or
    // venue-only) order and applies it directly to Oms's own, existing
    // resolve_unknown()/transition() -- this file adds no new order-mutation
    // logic, it only tracks that a resolution occurred so READY can be
    // re-attempted.
    void mark_discrepancy_resolved() noexcept { ++resolutions_since_last_reconcile_; }

    // Re-run after resolving discrepancies out of band: if the *current*
    // Oms/venue state now compares clean, this reaches READY without
    // re-draining reports (already applied exactly-once and idempotent to
    // re-drain if desired, but not required here).
    [[nodiscard]] bool retry_after_resolution(OrderSnapshot* local_scratch, std::size_t local_capacity,
                                              OrderSnapshot* venue_scratch, std::size_t venue_capacity,
                                              OrderReconcileResult* result_out, std::size_t result_capacity) noexcept {
        if (state_ != ConnectionState::reconciling) return false;
        std::size_t local_n = 0;
        std::size_t unresolved_unknown = 0;
        for (std::size_t i = 0; i < local_.size() && local_n < local_capacity; ++i) {
            const auto& o = local_.at(i);
            if (exec::is_terminal(o.state)) continue;
            local_scratch[local_n++] = OrderSnapshot{o.ref, o.state, o.requested_volume, o.filled_volume};
            if (o.state == exec::OrderState::unknown) ++unresolved_unknown;
        }
        const auto venue_n = venue_.fetch_open_orders(venue_scratch, venue_capacity);
        OrderReconciler reconciler;
        const auto result_n = reconciler.compare(local_scratch, local_n, venue_scratch, venue_n,
                                                  result_out, result_capacity);
        const bool clean = result_n == 0 && unresolved_unknown == 0;
        if (clean) state_ = ConnectionState::ready;
        return clean;
    }

    [[nodiscard]] const ReconciliationOutcome& last_outcome() const noexcept { return last_outcome_; }

private:
    Oms& local_;
    const VenueAdapter& venue_;
    ReportSequencer& seq_;
    ConnectionState state_{ConnectionState::connected};
    ReconciliationOutcome last_outcome_{};
    std::size_t resolutions_since_last_reconcile_{0};
};

}  // namespace oms
