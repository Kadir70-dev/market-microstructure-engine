#pragma once
#include <cstdint>

#include "oms/multi_venue.hpp"
#include "oms/strategy_scheduler.hpp"
#include "risk/risk_ledger.hpp"

// Phase I -- the sanctioned, mandatory-risk-checked entry point for new
// exposure. Reuses Phase F (VenueRegistry/VenueConnection), Phase G
// (route_create/cancel/replace's session+rate-limiter gates, now also
// risk-gated via the Phase I hook attached below), and Phase H
// (StrategyScheduler, for callers that arbitrate) completely unchanged --
// this file is pure orchestration, gluing an already-attached RiskLedger
// into the strategy-attributed side of the pipeline the hook alone cannot
// reach (see risk_gate.hpp for why: route_create() carries no StrategyId).
//
// Pipeline order (Part 7's explicitly required "scheduler -> rate limiter
// -> risk authority", plus the global kill switch checked first,
// unconditionally, ahead of everything else):
//
//   ShardedOms::globally_halted()   [authoritative, first, always]
//        -> StrategyScheduler::next() [Phase H: decides ORDER only]
//        -> RiskLedger: per-strategy reserve            [Phase I, new]
//        -> VenueConnection::route_create()             [Phase G, unchanged]
//             -> session / connection-state              [Phase F/G]
//             -> per-venue rate limiter                  [Phase G]
//             -> RiskGate hook: symbol/venue/global reserve [Phase I, new]
//             -> ShardedOms::submit_create()             [Phase C, unchanged]
//                  -> standing_approval() halt check      [Phase C/G, unchanged]
//
// Any rejection at any stage unwinds exactly what this call itself
// reserved, in reverse order, before returning -- see submit_create's own
// comments for the two failure points that need unwinding.

namespace oms {

class RiskGatedRouter final {
public:
    // Deliberately does NOT attach ledger to any VenueConnection as a
    // RiskGate: doing so would make route_create()'s own hook call *compete*
    // with this router's outer strategy-level reserve() for the same ref
    // (see submit_create's comment below) -- two independent reservation
    // attempts for one ref, the second of which always fails by design
    // (never double-reserve), which would make every RiskGatedRouter call
    // spuriously rejected. The hook (VenueConnection::attach_risk_gate) is
    // for callers that bypass RiskGatedRouter entirely and call
    // route_create()/route_replace() directly -- those still get symbol/
    // venue/global protection if a caller separately attaches a ledger.
    // RiskGatedRouter's own callers get full strategy+symbol+venue+global
    // protection through the single outer reserve() call in submit_create.
    //
    // self_trade is optional (nullptr by default -- backward compatible
    // with every Phase I call site) and closes a Phase J audit finding:
    // Phase H built SelfTradeTracker as a real, tested self-trade-
    // prevention component, but nothing in Phase I's mandatory admission
    // path (this class) ever called it -- self-trade prevention existed in
    // the codebase but was not actually enforced by the one path everything
    // is supposed to go through. Passing a tracker here closes that; a
    // caller that still doesn't pass one keeps exactly Phase I's original
    // behavior (no self-trade check), same as before this phase.
    RiskGatedRouter(VenueRegistry& registry, risk::RiskLedger& ledger,
                    SelfTradeTracker* self_trade = nullptr) noexcept
        : registry_(registry), ledger_(ledger), self_trade_(self_trade) {}

    // The one sanctioned path for new exposure. `strategy` must already be
    // registered with the ledger (risk::RiskLedger::register_strategy) --
    // an unregistered strategy fails closed, matching Part "Explicit
    // StrategyId ownership" applied to risk, not just arbitration.
    [[nodiscard]] Decision submit_create(risk::StrategyId strategy, VenueId venue, std::size_t producer_id,
                                        exec::BrokerOrderRef ref, std::uint32_t symbol, exec::Side side,
                                        std::int64_t volume, std::uint64_t now_ns, Completion& out) noexcept {
        if (ShardedOms::globally_halted()) return Decision::rejected;
        auto* conn = registry_.find(venue);
        if (conn == nullptr) return Decision::rejected;

        // Outer reserve: strategy+symbol+venue+global, all in this one call
        // (this ledger is never attached as this VenueConnection's RiskGate
        // hook -- see the constructor's comment -- so route_create() below
        // makes no competing reservation attempt of its own for this ref).
        if (!ledger_.reserve(strategy, venue, symbol, side, volume, ref)) return Decision::rejected;

        // Self-trade prevention (Phase H's SelfTradeTracker, if attached --
        // see the constructor's comment): checked after risk reserves but
        // before the OMS call, so a blocked self-trade unwinds the ledger
        // exactly like any other post-reserve rejection, never leaving a
        // dangling reservation.
        if (self_trade_ != nullptr && !self_trade_->check_and_apply(venue, symbol, strategy, side, volume)) {
            ledger_.release(ref);
            return Decision::rejected;
        }

        const auto decision = conn->route_create(producer_id, ref, symbol, volume, now_ns, out);
        if (decision != Decision::admitted) {
            ledger_.release(ref);  // unwind the outer reserve -- route_create's own gates (session/
            return decision;       // rate-limiter) never touched the ledger, so nothing else to undo.
        }
        return Decision::admitted;
    }

    [[nodiscard]] Decision submit_cancel(VenueId venue, std::size_t producer_id, exec::BrokerOrderRef ref,
                                         std::uint64_t now_ns, Completion& out) noexcept {
        if (ShardedOms::globally_halted()) return Decision::rejected;
        auto* conn = registry_.find(venue);
        if (conn == nullptr) return Decision::rejected;
        const auto decision = conn->route_cancel(producer_id, ref, now_ns, out);
        if (decision == Decision::admitted) ledger_.reconcile(ref, out.order.filled_volume, terminal_or_gone(ref, out));
        return decision;
    }

    // Applies the requested_volume delta directly against the ledger
    // (risk::RiskLedger::adjust_replace()) -- NOT via VenueConnection's
    // RiskGate hook, which this router never attaches (see the
    // constructor's comment): route_replace()'s hook call would be a
    // no-op here regardless, so this method must call the ledger itself,
    // the same way submit_create() calls reserve() itself rather than
    // relying on the hook.
    [[nodiscard]] Decision submit_replace(VenueId venue, std::size_t producer_id, exec::BrokerOrderRef ref,
                                          std::int64_t new_price, std::int64_t new_volume, std::uint64_t now_ns,
                                          Completion& out) noexcept {
        if (ShardedOms::globally_halted()) return Decision::rejected;
        auto* conn = registry_.find(venue);
        if (conn == nullptr) return Decision::rejected;

        Completion before{};
        const bool found = conn->oms().submit_find(producer_id, ref, before);
        const auto old_requested = found ? before.order.requested_volume : new_volume;

        if (!ledger_.adjust_replace(venue, ref, new_volume)) return Decision::rejected;  // increase past a limit: fail closed, no ledger change
        const auto decision = conn->route_replace(producer_id, ref, new_price, new_volume, now_ns, out);
        if (decision != Decision::admitted) {
            const auto delta = new_volume - old_requested;
            if (delta > 0) ledger_.undo_replace_increase(ref, delta);
            // delta<=0 (a decrease that failed at the OMS level): documented
            // narrow limitation (risk_ledger.hpp's adjust_replace comment)
            // -- conservative under-reservation, self-corrects on this
            // ref's next reconcile()/release(), never an oversubscription.
        }
        return decision;
    }

    [[nodiscard]] Decision submit_report(VenueId venue, std::size_t producer_id, const ExecReport& report,
                                        Completion& out) noexcept {
        auto* conn = registry_.find(venue);
        if (conn == nullptr) return Decision::rejected;
        const bool submitted = conn->oms().submit_report(producer_id, report, out);
        if (submitted && out.report_outcome == ReportOutcome::applied) {
            // Exactly-once by construction (Part 5): reconcile() computes
            // the exposure/position delta against last_known_filled, which
            // ReportSequencer's own dedup already guarantees only advances
            // once per actually-new fill -- a duplicate/held/terminal-late
            // report never reaches report_outcome==applied at all, so this
            // branch is simply never entered for them.
            ledger_.reconcile(report.ref, out.order.filled_volume, terminal_or_gone(report.ref, out));
        }
        return submitted && out.ok ? Decision::admitted : Decision::rejected;
    }

private:
    // A terminal transition reclaims the order's Oms slot (Phase B's
    // swap-removal reclaim); ShardedOms::apply()'s report/transition cases
    // re-find() the ref afterward and only copy it into Completion::order
    // if still found -- so a just-reclaimed order leaves `out.order` at its
    // default-constructed value (ref={0,0}, state=new_order) rather than
    // reflecting the terminal state it actually reached. Comparing
    // out.order.ref against the ref we asked about is therefore the
    // reliable "gone" signal `exec::is_terminal(out.order.state)` alone
    // would miss.
    [[nodiscard]] static bool terminal_or_gone(exec::BrokerOrderRef ref, const Completion& out) noexcept {
        return exec::is_terminal(out.order.state) || !(out.order.ref == ref);
    }

    VenueRegistry& registry_;
    risk::RiskLedger& ledger_;
    SelfTradeTracker* self_trade_;
};

}  // namespace oms
