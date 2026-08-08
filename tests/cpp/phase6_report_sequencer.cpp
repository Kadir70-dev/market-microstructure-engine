#include <algorithm>
#include <cstring>
#include <numeric>
#include <vector>

#include "oms/report_sequencer.hpp"
#include "oms/sharded_oms.hpp"
#include "phase6_test.hpp"

// Phase D — execution-report sequencing, deduplication, deterministic
// reconciliation of arrival order (include/oms/report_sequencer.hpp).
//
// Every scenario here operates on a fresh oms::Oms + oms::ReportSequencer
// pair, standalone (no threading needed to prove sequencing logic is
// correct -- Phase C already proved the threading/queue layer is safe, and
// ShardedOms's OpKind::report wiring is covered separately below).

namespace {

using oms::ExecReport;
using oms::ReportKind;
using oms::ReportOutcome;

risk::Approval approval() {
    risk::RiskEngine e(risk::Limits{});
    risk::Request q{};
    q.volume = 1; q.risk_minor = 1; q.free_margin = 1'000'000; q.warm_mask = 1; q.session_open = true;
    return e.check(q).token;
}

// Creates an order and drives it to `acknowledged` via the internal
// (non-report) pending_send -> sent transition plus an ack report at seq 1.
// Returns the next free venue_seq (2) for the caller's own reports.
std::uint64_t setup_acknowledged(oms::Oms& o, oms::ReportSequencer& seq, exec::BrokerOrderRef ref,
                                 std::int64_t volume, const risk::Approval& token) {
    (void)o.create(ref, 0, volume, token);
    (void)o.transition(ref, exec::OrderState::sent);
    (void)seq.process(o, ExecReport{ref, 1, ReportKind::ack, 0, 0});
    return 2;
}

}  // namespace

int main() {
    Phase6Test t;
    const auto token = approval();

    // =========================================================================
    // Adversarial: permute the same report set into every possible arrival
    // order, prove identical final state every time.
    // =========================================================================
    {
        // 4 reports after ack: fill(2,3) fill(3,4) fill(4,2) cancel_pending(5)
        // -- deliberately over-fills past a clean stop so some permutations
        // apply fewer of them (the fill that would exceed remaining() at that
        // point in that ordering is `illegal` and skipped), which is itself
        // part of what must be identical across permutations: not just the
        // reports that happen to apply cleanly, but which ones do.
        const exec::BrokerOrderRef ref{1, 1};
        std::vector<ExecReport> reports{
            {ref, 2, ReportKind::fill, 3, 0},
            {ref, 3, ReportKind::fill, 4, 0},
            {ref, 4, ReportKind::fill, 2, 0},
            {ref, 5, ReportKind::cancel_pending, 0, 0},
        };
        std::vector<std::size_t> order(reports.size());
        std::iota(order.begin(), order.end(), 0);

        exec::Order baseline{};
        bool have_baseline = false;
        bool all_identical = true;
        std::size_t permutations_checked = 0;

        do {
            oms::Oms o(4);
            oms::ReportSequencer seq;
            (void)setup_acknowledged(o, seq, ref, 10, token);
            for (const auto idx : order) (void)seq.process(o, reports[idx]);

            exec::Order final_order{};
            const auto* found = o.find(ref);
            if (found) final_order = *found;
            // last_applied_seq is arrival-order-dependent bookkeeping (it is
            // whichever seq happened to be applied last in *this* permutation
            // if the order is still live), not part of the logical state --
            // exclude it from the convergence check the same way a real
            // digest would compare business fields, not processing metadata.
            final_order.last_applied_seq = 0;

            if (!have_baseline) { baseline = final_order; have_baseline = true; }
            else if (std::memcmp(&baseline, &final_order, sizeof(exec::Order)) != 0) all_identical = false;
            ++permutations_checked;
        } while (std::next_permutation(order.begin(), order.end()));

        t.check(permutations_checked == 24, "permutation: all 24 orderings of 4 reports were exercised");
        t.check(all_identical, "permutation: every arrival order converges to identical final order state");
    }

    // =========================================================================
    // Required scenario coverage
    // =========================================================================

    // ---- duplicate execution reports ---------------------------------------
    {
        oms::Oms o(4); oms::ReportSequencer seq;
        const exec::BrokerOrderRef ref{1, 1};
        auto next = setup_acknowledged(o, seq, ref, 10, token);
        t.check(seq.process(o, ExecReport{ref, next, ReportKind::fill, 5, 0}) == ReportOutcome::applied,
                "duplicate: first fill applied");
        t.check(seq.process(o, ExecReport{ref, next, ReportKind::fill, 5, 0}) == ReportOutcome::duplicate,
                "duplicate: identical report rejected");
        t.check(o.find(ref)->filled_volume == 5, "duplicate: filled_volume not double-counted");
    }

    // ---- out-of-order / partial fills arriving out of order ---------------
    {
        oms::Oms o(4); oms::ReportSequencer seq;
        const exec::BrokerOrderRef ref{1, 1};
        auto next = setup_acknowledged(o, seq, ref, 10, token);
        t.check(seq.process(o, ExecReport{ref, next + 1, ReportKind::fill, 4, 0}) == ReportOutcome::held_for_gap,
                "out-of-order: later partial fill held");
        t.check(seq.process(o, ExecReport{ref, next, ReportKind::fill, 3, 0}) == ReportOutcome::applied,
                "out-of-order: earlier partial fill fills the gap");
        t.check(o.find(ref)->filled_volume == 7, "out-of-order: both partials applied in the correct order (3+4)");
    }

    // ---- cancel/fill and replace/fill races (report-level, not thread) ----
    // Both orderings of "cancel_pending arrives before its matching fill" and
    // "fill arrives before cancel_pending" must be safe and legally-shaped.
    for (const bool cancel_first : {true, false}) {
        oms::Oms o(4); oms::ReportSequencer seq;
        const exec::BrokerOrderRef ref{1, cancel_first ? 1u : 2u};
        auto next = setup_acknowledged(o, seq, ref, 10, token);
        const ExecReport fill{ref, next, ReportKind::fill, 4, 0};
        const ExecReport cancel_pending{ref, next + 1, ReportKind::cancel_pending, 0, 0};
        if (cancel_first) {
            t.check(seq.process(o, cancel_pending) == ReportOutcome::held_for_gap, "race: cancel_pending held (gap)");
            t.check(seq.process(o, fill) == ReportOutcome::applied, "race: fill fills the gap");
        } else {
            t.check(seq.process(o, fill) == ReportOutcome::applied, "race: fill applied first");
            t.check(seq.process(o, cancel_pending) == ReportOutcome::applied,
                    "race: cancel_pending applies after (partially_filled->cancel_pending is legal)");
        }
        const auto* order = o.find(ref);
        t.check(order != nullptr, "race: order still live (cancel_pending is not terminal)");
        t.check(order->state == exec::OrderState::cancel_pending, "race: same end state regardless of arrival order");
        t.check(order->filled_volume == 4, "race: fill applied exactly once either way");
    }

    // replace vs fill: replace lowers volume below a fill that already
    // happened -- the fill must never be "un-applied"; a later, in-sequence
    // replace attempting to set volume <= filled_volume must be illegal.
    {
        oms::Oms o(4); oms::ReportSequencer seq;
        const exec::BrokerOrderRef ref{1, 1};
        auto next = setup_acknowledged(o, seq, ref, 10, token);
        t.check(seq.process(o, ExecReport{ref, next, ReportKind::fill, 6, 0}) == ReportOutcome::applied,
                "replace-race: fill 6 applied");
        t.check(seq.process(o, ExecReport{ref, next + 1, ReportKind::replace_ack, 5, 100}) == ReportOutcome::illegal,
                "replace-race: replace to volume 5 rejected (already filled 6)");
        t.check(seq.process(o, ExecReport{ref, next + 1, ReportKind::replace_ack, 8, 100}) == ReportOutcome::applied,
                "replace-race: replace to volume 8 (above filled) accepted");
        t.check(o.find(ref)->requested_volume == 8, "replace-race: new volume in effect");
    }

    // ---- stale reports after replace ---------------------------------------
    // A fill computed against the pre-replace volume (10) that would now
    // exceed the post-replace volume (4) must be rejected, not silently
    // clipped or over-applied.
    {
        oms::Oms o(4); oms::ReportSequencer seq;
        const exec::BrokerOrderRef ref{1, 1};
        auto next = setup_acknowledged(o, seq, ref, 10, token);
        t.check(seq.process(o, ExecReport{ref, next, ReportKind::replace_ack, 4, 100}) == ReportOutcome::applied,
                "stale-after-replace: replace to volume 4 applied");
        t.check(seq.process(o, ExecReport{ref, next + 1, ReportKind::fill, 9, 0}) == ReportOutcome::illegal,
                "stale-after-replace: fill computed against the old volume (9 > new remaining 4) rejected");
        t.check(o.find(ref)->filled_volume == 0, "stale-after-replace: rejected fill left filled_volume untouched");
    }

    // ---- replace chain + stale fill (named fault test) ----------------------
    {
        oms::Oms o(4); oms::ReportSequencer seq;
        const exec::BrokerOrderRef ref{1, 1};
        auto next = setup_acknowledged(o, seq, ref, 100, token);
        t.check(seq.process(o, ExecReport{ref, next++, ReportKind::replace_ack, 50, 100}) == ReportOutcome::applied,
                "replace-chain: 100 -> 50");
        t.check(seq.process(o, ExecReport{ref, next++, ReportKind::replace_ack, 20, 90}) == ReportOutcome::applied,
                "replace-chain: 50 -> 20");
        t.check(seq.process(o, ExecReport{ref, next++, ReportKind::replace_ack, 5, 80}) == ReportOutcome::applied,
                "replace-chain: 20 -> 5");
        // A stale fill for 30 (valid against an earlier link in the chain,
        // e.g. the 50 or 20 stage) must be rejected against the *current*
        // volume of 5.
        t.check(seq.process(o, ExecReport{ref, next++, ReportKind::fill, 30, 0}) == ReportOutcome::illegal,
                "replace-chain: stale fill against an earlier chain link rejected");
        t.check(o.find(ref)->requested_volume == 5, "replace-chain: final volume is the last link, unaffected");
        t.check(o.find(ref)->filled_volume == 0, "replace-chain: stale fill did not corrupt filled_volume");
    }

    // ---- reports received after terminal state / terminal-order late ------
    {
        oms::Oms o(4); oms::ReportSequencer seq;
        const exec::BrokerOrderRef ref{1, 1};
        auto next = setup_acknowledged(o, seq, ref, 5, token);
        t.check(seq.process(o, ExecReport{ref, next, ReportKind::fill, 5, 0}) == ReportOutcome::applied,
                "terminal-late: full fill terminates and reclaims the order");
        t.check(o.find(ref) == nullptr, "terminal-late: order reclaimed (Phase B behavior)");
        t.check(seq.process(o, ExecReport{ref, next + 1, ReportKind::cancel_pending, 0, 0}) ==
                    ReportOutcome::terminal_late,
                "terminal-late: a late report for a known-but-terminal order is recognized, not treated as unknown");
    }

    // ---- sequence gaps + gap pool exhaustion (fail-closed) ------------------
    {
        oms::Oms o(8);
        oms::ReportSequencer seq(/*gap_pool_capacity=*/2, /*terminal_cache_capacity=*/16);
        exec::BrokerOrderRef refs[3]{{1, 1}, {1, 2}, {1, 3}};
        std::uint64_t next[3];
        for (int i = 0; i < 3; ++i) next[i] = setup_acknowledged(o, seq, refs[i], 10, token);

        t.check(seq.process(o, ExecReport{refs[0], next[0] + 1, ReportKind::fill, 1, 0}) ==
                    ReportOutcome::held_for_gap,
                "gap: first held order fills the 2-slot pool (1/2)");
        t.check(seq.process(o, ExecReport{refs[1], next[1] + 1, ReportKind::fill, 1, 0}) ==
                    ReportOutcome::held_for_gap,
                "gap: second held order fills the 2-slot pool (2/2)");
        t.check(seq.process(o, ExecReport{refs[2], next[2] + 1, ReportKind::fill, 1, 0}) ==
                    ReportOutcome::gap_pool_exhausted,
                "gap: third gap fails closed -- pool genuinely exhausted, not silently dropped or misapplied");
        t.check(seq.gap_pool_held() == 2, "gap: pool occupancy matches exactly what was accepted");

        // Filling the first gap must free capacity and the held report must
        // now apply correctly (proving exhaustion didn't corrupt the pool).
        t.check(seq.process(o, ExecReport{refs[0], next[0], ReportKind::fill, 2, 0}) == ReportOutcome::applied,
                "gap: filling order 0's gap succeeds");
        t.check(o.find(refs[0])->filled_volume == 3, "gap: order 0's held fill cascaded correctly (2+1)");
    }

    // ---- reconnect replay of previously-seen reports -----------------------
    {
        oms::Oms o(4); oms::ReportSequencer seq;
        const exec::BrokerOrderRef ref{1, 1};
        auto next = setup_acknowledged(o, seq, ref, 10, token);
        std::vector<ExecReport> stream{
            {ref, next, ReportKind::fill, 3, 0},
            {ref, next + 1, ReportKind::fill, 2, 0},
        };
        for (const auto& r : stream) t.check(seq.process(o, r) == ReportOutcome::applied, "reconnect: initial apply");
        t.check(o.find(ref)->filled_volume == 5, "reconnect: state before replay");

        // Venue reconnects and resends its *entire* report log, including the
        // ack from setup_acknowledged.
        t.check(seq.process(o, ExecReport{ref, 1, ReportKind::ack, 0, 0}) == ReportOutcome::duplicate,
                "reconnect: replayed ack rejected as duplicate");
        for (const auto& r : stream)
            t.check(seq.process(o, r) == ReportOutcome::duplicate, "reconnect: replayed report rejected as duplicate");
        t.check(o.find(ref)->filled_volume == 5, "reconnect: state unchanged after full replay");
    }

    // ---- unknown/orphan order reports ---------------------------------------
    {
        oms::Oms o(4); oms::ReportSequencer seq;
        t.check(seq.process(o, ExecReport{exec::BrokerOrderRef{1, 999}, 1, ReportKind::ack, 0, 0}) ==
                    ReportOutcome::unknown_order,
                "unknown: a ref this Oms never created is unknown, not silently ignored or crashed on");
    }

    // ---- duplicate storm (fault test) ---------------------------------------
    {
        oms::Oms o(4); oms::ReportSequencer seq;
        const exec::BrokerOrderRef ref{1, 1};
        auto next = setup_acknowledged(o, seq, ref, 10, token);
        std::size_t applied = 0, duplicates = 0;
        for (int i = 0; i < 1000; ++i) {
            const auto outcome = seq.process(o, ExecReport{ref, next, ReportKind::fill, 5, 0});
            if (outcome == ReportOutcome::applied) ++applied;
            else if (outcome == ReportOutcome::duplicate) ++duplicates;
        }
        t.check(applied == 1, "duplicate storm: exactly one of 1000 identical submissions applied");
        t.check(duplicates == 999, "duplicate storm: the remaining 999 all rejected as duplicates");
        t.check(o.find(ref)->filled_volume == 5, "duplicate storm: filled_volume reflects exactly one application");
    }

    // ---- delayed fill after cancel (fault test) ------------------------------
    // cancel_pending and cancelled both arrive before their preceding fill.
    {
        oms::Oms o(4); oms::ReportSequencer seq;
        const exec::BrokerOrderRef ref{1, 1};
        auto next = setup_acknowledged(o, seq, ref, 10, token);
        t.check(seq.process(o, ExecReport{ref, next + 1, ReportKind::cancel_pending, 0, 0}) ==
                    ReportOutcome::held_for_gap,
                "delayed-fill-after-cancel: cancel_pending held pending the fill gap");
        t.check(seq.process(o, ExecReport{ref, next + 2, ReportKind::cancelled, 0, 0}) == ReportOutcome::held_for_gap,
                "delayed-fill-after-cancel: cancelled also held (its own gap, behind cancel_pending)");
        t.check(seq.process(o, ExecReport{ref, next, ReportKind::fill, 4, 0}) == ReportOutcome::applied,
                "delayed-fill-after-cancel: the delayed fill arrives and cascades both held reports");
        t.check(o.find(ref) == nullptr, "delayed-fill-after-cancel: order reached cancelled (terminal, reclaimed)");
    }

    // ---- fill/cancel crossing (fault test) -----------------------------------
    // A full fill and a cancel_pending cross on the wire; the fill completes
    // the order first, so the cancel_pending must correctly fail once it's
    // in sequence, not silently succeed against a gone order.
    {
        oms::Oms o(4); oms::ReportSequencer seq;
        const exec::BrokerOrderRef ref{1, 1};
        auto next = setup_acknowledged(o, seq, ref, 5, token);
        t.check(seq.process(o, ExecReport{ref, next + 1, ReportKind::cancel_pending, 0, 0}) ==
                    ReportOutcome::held_for_gap,
                "fill-cancel-crossing: cancel_pending arrives first, held");
        t.check(seq.process(o, ExecReport{ref, next, ReportKind::fill, 5, 0}) == ReportOutcome::applied,
                "fill-cancel-crossing: full fill fills the gap and terminates the order");
        // Draining attempted the held cancel_pending automatically; since the
        // order is now gone, drain() must discard it rather than crash or
        // apply it to whatever now occupies the reclaimed slot.
        t.check(o.find(ref) == nullptr, "fill-cancel-crossing: order correctly terminal (filled)");
        exec::BrokerOrderRef ref2{1, 2};
        (void)o.create(ref2, 0, 1, token);
        t.check(o.find(ref2)->state != exec::OrderState::cancel_pending,
                "fill-cancel-crossing: the discarded held cancel_pending did not leak onto a reused slot");
    }

    // ---- terminal-order late report (fault test, distinct ref from above) --
    {
        oms::Oms o(4); oms::ReportSequencer seq;
        const exec::BrokerOrderRef ref{1, 1};
        auto next = setup_acknowledged(o, seq, ref, 5, token);
        t.check(seq.process(o, ExecReport{ref, next, ReportKind::reject, 0, 0}) == ReportOutcome::illegal,
                "terminal-late (reject path): reject from acknowledged is not a legal transition");
        t.check(seq.process(o, ExecReport{ref, next, ReportKind::fill, 5, 0}) == ReportOutcome::applied,
                "terminal-late (reject path): fill terminates normally instead");
        t.check(seq.process(o, ExecReport{ref, next + 1, ReportKind::fill, 1, 0}) == ReportOutcome::terminal_late,
                "terminal-late (reject path): a further report for the now-terminal order is recognized as such");
    }

    // =========================================================================
    // ShardedOms integration: report sequencing through the real queue path,
    // preserving Phase C's single-writer-per-shard ownership.
    // =========================================================================
    {
        oms::ShardedOms s(2, 16, 1, 64);
        const exec::BrokerOrderRef ref{1, 5};
        oms::Completion c{};
        t.check(s.submit_create(0, ref, 0, 10, c) && c.ok, "sharded: create");
        oms::Completion sent{};
        t.check(s.submit_transition(0, ref, exec::OrderState::sent, sent) && sent.ok, "sharded: internal sent");
        oms::Completion ack{};
        t.check(s.submit_report(0, ExecReport{ref, 1, ReportKind::ack, 0, 0}, ack) &&
                    ack.report_outcome == ReportOutcome::applied,
                "sharded: ack report applied through the queue");
        oms::Completion dup{};
        t.check(s.submit_report(0, ExecReport{ref, 1, ReportKind::ack, 0, 0}, dup) &&
                    dup.report_outcome == ReportOutcome::duplicate,
                "sharded: duplicate report correctly rejected through the queue");
        oms::Completion fill{};
        t.check(s.submit_report(0, ExecReport{ref, 2, ReportKind::fill, 10, 0}, fill) &&
                    fill.report_outcome == ReportOutcome::applied,
                "sharded: fill report applied through the queue");
    }

    return t.result();
}
