#include <atomic>
#include <thread>
#include <vector>

#include "oms/multi_venue.hpp"
#include "phase6_test.hpp"

// Phase F -- deterministic multi-exchange architecture. Covers all 11 named
// scenarios from the phase requirements. Does not re-prove Phase B/C/D/E
// correctness (OMS capacity/concurrency, report sequencing, reconciliation
// mechanics) -- those are exercised unmodified, per venue, by the existing
// phase6/phase7 suites; this file proves the *new* thing multi-venue adds:
// identity isolation, per-venue state independence, routing, cross-venue
// exposure, and a global kill switch that still reaches every venue.

namespace {

constexpr oms::VenueId VENUE_A = 1001;
constexpr oms::VenueId VENUE_B = 1002;

// Minimal, fully test-controlled VenueAdapter: pushes exactly the
// open-orders/reports a test scenario wants, rather than deriving them from a
// live PaperBroker (whose NOT_CALIBRATED latency model would add unrelated
// timing noise to tests that are about venue *identity/isolation*, not fill
// mechanics -- fill mechanics are Phase D/E's job, already covered).
class FakeVenue final : public oms::VenueAdapter {
public:
    std::vector<oms::OrderSnapshot> open_orders;
    std::vector<oms::ExecReport> reports;

    [[nodiscard]] std::size_t fetch_open_orders(oms::OrderSnapshot* out,
                                                std::size_t capacity) const noexcept override {
        const auto n = std::min(capacity, open_orders.size());
        for (std::size_t i = 0; i < n; ++i) out[i] = open_orders[i];
        return n;
    }
    [[nodiscard]] std::size_t fetch_positions(oms::PositionSnapshot*, std::size_t) const noexcept override {
        return 0;
    }
    [[nodiscard]] std::size_t fetch_recent_reports(oms::ExecReport* out,
                                                    std::size_t capacity) const noexcept override {
        const auto n = std::min(capacity, reports.size());
        for (std::size_t i = 0; i < n; ++i) out[i] = reports[i];
        return n;
    }
    [[nodiscard]] exec::AccountState fetch_account() const noexcept override { return exec::AccountState{}; }
};

std::unique_ptr<oms::ShardedOms> make_venue_oms(std::size_t shards = 2, std::size_t cap_per_shard = 256,
                                                std::size_t max_producers = 4, std::size_t log_cap = 4096) {
    return std::make_unique<oms::ShardedOms>(shards, cap_per_shard, max_producers, log_cap);
}

// Registers venues A and B, each with its own ShardedOms + FakeVenue, and
// returns raw (non-owning) pointers to the FakeVenues so a test can script
// their responses after registration.
struct TwoVenues final {
    oms::VenueRegistry registry;
    FakeVenue* venue_a{nullptr};
    FakeVenue* venue_b{nullptr};
};

TwoVenues make_two_venues() {
    TwoVenues tv;
    auto adapter_a = std::make_unique<FakeVenue>();
    auto adapter_b = std::make_unique<FakeVenue>();
    tv.venue_a = adapter_a.get();
    tv.venue_b = adapter_b.get();
    tv.registry.add(VENUE_A, make_venue_oms(), std::move(adapter_a));
    tv.registry.add(VENUE_B, make_venue_oms(), std::move(adapter_b));
    return tv;
}

exec::BrokerOrderRef ref_on(oms::VenueId venue, std::uint64_t logical_id) noexcept {
    return exec::BrokerOrderRef{venue, logical_id};
}

}  // namespace

int main() {
    Phase6Test t;

    // ---- 1) same order ref on venue A + venue B must never collide --------
    {
        auto tv = make_two_venues();
        constexpr std::uint64_t logical_id = 42;

        oms::Completion ca{}, cb{};
        const bool ok_a = tv.registry.submit_create(VENUE_A, 0, ref_on(VENUE_A, logical_id), 7, 10, ca);
        const bool ok_b = tv.registry.submit_create(VENUE_B, 0, ref_on(VENUE_B, logical_id), 7, 20, cb);
        t.check(ok_a && ca.ok, "collision: create on venue A succeeds");
        t.check(ok_b && cb.ok, "collision: create on venue B succeeds");
        t.check(!(ref_on(VENUE_A, logical_id) == ref_on(VENUE_B, logical_id)),
               "collision: same logical_order_id, different venue -> refs compare unequal");

        auto* va = tv.registry.find(VENUE_A);
        auto* vb = tv.registry.find(VENUE_B);
        oms::Completion fa{}, fb{};
        t.check(va->oms().submit_find(0, ref_on(VENUE_A, logical_id), fa) && fa.ok && fa.order.requested_volume == 10,
               "collision: venue A's own order (volume 10) unaffected by venue B");
        t.check(vb->oms().submit_find(0, ref_on(VENUE_B, logical_id), fb) && fb.ok && fb.order.requested_volume == 20,
               "collision: venue B's own order (volume 20) unaffected by venue A");
        // Cross-venue lookup: venue A's Oms must never resolve venue B's ref,
        // proving isolation is structural (separate ShardedOms), not merely
        // that BrokerOrderRef happens to compare unequal.
        oms::Completion cross{};
        t.check(!va->oms().submit_find(0, ref_on(VENUE_B, logical_id), cross) || !cross.ok,
               "collision: venue A cannot find venue B's order under any ref");
    }

    // ---- 2) simultaneous orders on 2 venues + 3) fills from both ----------
    {
        auto tv = make_two_venues();
        std::thread ta([&] {
            oms::Completion c{};
            (void)tv.registry.submit_create(VENUE_A, 0, ref_on(VENUE_A, 1), 3, 100, c);
        });
        std::thread tb([&] {
            oms::Completion c{};
            (void)tv.registry.submit_create(VENUE_B, 0, ref_on(VENUE_B, 1), 3, 100, c);
        });
        ta.join();
        tb.join();

        auto* va = tv.registry.find(VENUE_A);
        auto* vb = tv.registry.find(VENUE_B);
        oms::Completion fa{}, fb{};
        t.check(va->oms().submit_find(0, ref_on(VENUE_A, 1), fa) && fa.ok, "simultaneous: venue A order landed");
        t.check(vb->oms().submit_find(0, ref_on(VENUE_B, 1), fb) && fb.ok, "simultaneous: venue B order landed");

        // create() leaves an order in pending_send; an ack report legally
        // transitions sent -> acknowledged, so the OMS-side "I sent it"
        // pending_send -> sent step has to happen first (this is the local
        // send lifecycle, distinct from the venue's own ack).
        oms::Completion sent_a{}, sent_b{};
        (void)va->oms().submit_transition(0, ref_on(VENUE_A, 1), exec::OrderState::sent, sent_a);
        (void)vb->oms().submit_transition(0, ref_on(VENUE_B, 1), exec::OrderState::sent, sent_b);

        // Fills on both venues, via each venue's own embedded ReportSequencer.
        oms::Completion ack_a{}, fill_a{}, ack_b{}, fill_b{};
        oms::ExecReport rep_ack_a{ref_on(VENUE_A, 1), 1, oms::ReportKind::ack, 0, 0};
        oms::ExecReport rep_fill_a{ref_on(VENUE_A, 1), 2, oms::ReportKind::fill, 40, 0};
        oms::ExecReport rep_ack_b{ref_on(VENUE_B, 1), 1, oms::ReportKind::ack, 0, 0};
        oms::ExecReport rep_fill_b{ref_on(VENUE_B, 1), 2, oms::ReportKind::fill, 60, 0};
        (void)va->oms().submit_report(0, rep_ack_a, ack_a);
        (void)va->oms().submit_report(0, rep_fill_a, fill_a);
        (void)vb->oms().submit_report(0, rep_ack_b, ack_b);
        (void)vb->oms().submit_report(0, rep_fill_b, fill_b);
        t.check(fill_a.report_outcome == oms::ReportOutcome::applied && fill_a.order.filled_volume == 40,
               "fills: venue A fill applied with correct volume");
        t.check(fill_b.report_outcome == oms::ReportOutcome::applied && fill_b.order.filled_volume == 60,
               "fills: venue B fill applied with correct volume, independent of venue A's");
    }

    // ---- 4) duplicate / out-of-order reports independently per venue ------
    {
        auto tv = make_two_venues();
        auto* va = tv.registry.find(VENUE_A);
        auto* vb = tv.registry.find(VENUE_B);
        oms::Completion c{};
        (void)tv.registry.submit_create(VENUE_A, 0, ref_on(VENUE_A, 5), 1, 100, c);
        (void)tv.registry.submit_create(VENUE_B, 0, ref_on(VENUE_B, 5), 1, 100, c);
        oms::Completion sent_a{}, sent_b{};
        (void)va->oms().submit_transition(0, ref_on(VENUE_A, 5), exec::OrderState::sent, sent_a);
        (void)vb->oms().submit_transition(0, ref_on(VENUE_B, 5), exec::OrderState::sent, sent_b);

        // Venue A: out-of-order (seq 2 before seq 1) -> held_for_gap, then
        // seq 1 arrives -> both apply via the gap drain.
        oms::Completion out_of_order{}, then_ack{};
        oms::ExecReport a_seq2{ref_on(VENUE_A, 5), 2, oms::ReportKind::fill, 10, 0};
        oms::ExecReport a_seq1{ref_on(VENUE_A, 5), 1, oms::ReportKind::ack, 0, 0};
        (void)va->oms().submit_report(0, a_seq2, out_of_order);
        (void)va->oms().submit_report(0, a_seq1, then_ack);
        t.check(out_of_order.report_outcome == oms::ReportOutcome::held_for_gap,
               "OOO: venue A seq-2-before-seq-1 held for gap");
        t.check(then_ack.report_outcome == oms::ReportOutcome::applied, "OOO: venue A seq-1 applied, drains seq-2");

        // Venue B: duplicate ack, independent of whatever venue A just did.
        oms::Completion b_ack1{}, b_ack1_dup{};
        oms::ExecReport b_seq1{ref_on(VENUE_B, 5), 1, oms::ReportKind::ack, 0, 0};
        (void)vb->oms().submit_report(0, b_seq1, b_ack1);
        (void)vb->oms().submit_report(0, b_seq1, b_ack1_dup);
        t.check(b_ack1.report_outcome == oms::ReportOutcome::applied, "dup: venue B first ack applied");
        t.check(b_ack1_dup.report_outcome == oms::ReportOutcome::duplicate,
               "dup: venue B retransmit correctly rejected as duplicate, unaffected by venue A's gap handling");
    }

    // ---- 5) venue A disconnect while venue B remains healthy --------------
    {
        auto tv = make_two_venues();
        auto* va = tv.registry.find(VENUE_A);
        auto* vb = tv.registry.find(VENUE_B);
        t.check(va->state() == oms::ConnectionState::connected && vb->state() == oms::ConnectionState::connected,
               "isolation: both venues start CONNECTED");

        va->disconnect();
        t.check(va->state() == oms::ConnectionState::disconnected, "isolation: venue A is DISCONNECTED");
        t.check(vb->state() == oms::ConnectionState::connected,
               "isolation: venue B stays CONNECTED, untouched by venue A's disconnect");

        // Venue B keeps operating normally while A is down.
        oms::Completion c{};
        t.check(tv.registry.submit_create(VENUE_B, 0, ref_on(VENUE_B, 9), 1, 10, c) && c.ok,
               "isolation: venue B still accepts new orders while venue A is disconnected");

        // Venue A recovers on its own; matching (clean) venue state -> READY.
        std::array<oms::ExecReport, 8> report_scratch{};
        std::array<exec::Order, 256> snap_scratch{};
        std::array<oms::OrderSnapshot, 256> local_scratch{}, venue_scratch{};
        std::array<oms::OrderReconcileResult, 512> result_scratch{};
        t.check(va->begin_recovery(), "isolation: venue A begin_recovery -> RECONCILING");
        t.check(va->state() == oms::ConnectionState::reconciling, "isolation: venue A is RECONCILING");
        const auto outcome = va->run_reconciliation(0, report_scratch.data(), report_scratch.size(),
                                                     snap_scratch.data(), snap_scratch.size(), local_scratch.data(),
                                                     local_scratch.size(), venue_scratch.data(), venue_scratch.size(),
                                                     result_scratch.data(), result_scratch.size());
        t.check(outcome.clean() && va->state() == oms::ConnectionState::ready,
               "isolation: venue A reaches READY (no local orders, no venue orders -> clean)");
        t.check(vb->state() == oms::ConnectionState::connected,
               "isolation: venue B state untouched by venue A's whole recovery cycle");
    }

    // ---- 6) simultaneous venue reconnect -----------------------------------
    {
        auto tv = make_two_venues();
        auto* va = tv.registry.find(VENUE_A);
        auto* vb = tv.registry.find(VENUE_B);
        oms::Completion c{};
        (void)tv.registry.submit_create(VENUE_A, 0, ref_on(VENUE_A, 1), 1, 10, c);
        (void)tv.registry.submit_create(VENUE_B, 0, ref_on(VENUE_B, 1), 1, 20, c);
        // Both venues report the same order they hold locally -- so a clean
        // reconciliation is reachable for both, independently. create()
        // leaves an order in pending_send (no ack applied in this block), so
        // that is the state the venue must agree with for a clean match.
        tv.venue_a->open_orders = {oms::OrderSnapshot{ref_on(VENUE_A, 1), exec::OrderState::pending_send, 10, 0}};
        tv.venue_b->open_orders = {oms::OrderSnapshot{ref_on(VENUE_B, 1), exec::OrderState::pending_send, 20, 0}};

        va->disconnect();
        vb->disconnect();

        std::atomic<bool> ok_a{false}, ok_b{false};
        auto reconnect = [](oms::VenueConnection* v, std::atomic<bool>& ok_flag) {
            std::array<oms::ExecReport, 8> reports{};
            std::array<exec::Order, 256> snap{};
            std::array<oms::OrderSnapshot, 256> local{}, venue{};
            std::array<oms::OrderReconcileResult, 512> result{};
            (void)v->begin_recovery();
            const auto outcome = v->run_reconciliation(0, reports.data(), reports.size(), snap.data(), snap.size(),
                                                        local.data(), local.size(), venue.data(), venue.size(),
                                                        result.data(), result.size());
            ok_flag.store(outcome.clean() && v->state() == oms::ConnectionState::ready);
        };
        std::thread ta([&] { reconnect(va, ok_a); });
        std::thread tb([&] { reconnect(vb, ok_b); });
        ta.join();
        tb.join();
        t.check(ok_a.load(), "simultaneous reconnect: venue A reaches READY");
        t.check(ok_b.load(), "simultaneous reconnect: venue B reaches READY, independent of venue A's own reconnect");
    }

    // ---- 7) venue-only / local-only reconciliation, independently ---------
    {
        auto tv = make_two_venues();
        auto* va = tv.registry.find(VENUE_A);
        auto* vb = tv.registry.find(VENUE_B);
        oms::Completion c{};
        // Venue A: a local order the venue never reports back -> local_only.
        (void)tv.registry.submit_create(VENUE_A, 0, ref_on(VENUE_A, 1), 1, 10, c);
        tv.venue_a->open_orders.clear();  // venue A's venue reports nothing

        // Venue B: the venue reports an order local Oms never created -> venue_only.
        tv.venue_b->open_orders = {oms::OrderSnapshot{ref_on(VENUE_B, 99), exec::OrderState::new_order, 5, 0}};

        va->disconnect();
        vb->disconnect();
        (void)va->begin_recovery();
        (void)vb->begin_recovery();

        std::array<oms::ExecReport, 8> reports{};
        std::array<exec::Order, 256> snap{};
        std::array<oms::OrderSnapshot, 256> local{}, venue{};
        std::array<oms::OrderReconcileResult, 512> result{};
        const auto outcome_a = va->run_reconciliation(0, reports.data(), reports.size(), snap.data(), snap.size(),
                                                       local.data(), local.size(), venue.data(), venue.size(),
                                                       result.data(), result.size());
        const auto outcome_b = vb->run_reconciliation(0, reports.data(), reports.size(), snap.data(), snap.size(),
                                                       local.data(), local.size(), venue.data(), venue.size(),
                                                       result.data(), result.size());
        t.check(outcome_a.local_only == 1 && outcome_a.venue_only == 0,
               "per-venue reconcile: venue A's local_only break detected, not misreported as venue_only");
        t.check(outcome_b.venue_only == 1 && outcome_b.local_only == 0,
               "per-venue reconcile: venue B's venue_only break detected independently of venue A's break");
        t.check(va->state() == oms::ConnectionState::reconciling && vb->state() == oms::ConnectionState::reconciling,
               "per-venue reconcile: both venues correctly stay RECONCILING (neither break auto-resolves)");
    }

    // ---- 8) cross-venue aggregate exposure ---------------------------------
    {
        auto tv = make_two_venues();
        oms::Completion c{};
        // Venue A: buy 100 symbol 2, filled 30 -> open 70.
        (void)tv.registry.submit_create(VENUE_A, 0, ref_on(VENUE_A, 1), 2, 100, c);
        {
            oms::Completion sent{}, ack{}, fill{};
            (void)tv.registry.find(VENUE_A)->oms().submit_transition(0, ref_on(VENUE_A, 1), exec::OrderState::sent,
                                                                      sent);
            oms::ExecReport a_ack{ref_on(VENUE_A, 1), 1, oms::ReportKind::ack, 0, 0};
            oms::ExecReport a_fill{ref_on(VENUE_A, 1), 2, oms::ReportKind::fill, 30, 0};
            (void)tv.registry.find(VENUE_A)->oms().submit_report(0, a_ack, ack);
            (void)tv.registry.find(VENUE_A)->oms().submit_report(0, a_fill, fill);
        }
        // Venue B: buy 50 symbol 2, unfilled -> open 50. Aggregate symbol 2: 120.
        (void)tv.registry.submit_create(VENUE_B, 0, ref_on(VENUE_B, 2), 2, 50, c);

        std::array<exec::Order, 256> scratch{};
        const auto report = oms::aggregate_exposure(tv.registry, 0, scratch.data(), scratch.size());
        t.check(report.by_symbol[2].open_volume == 120,
               "exposure: symbol 2 aggregates 70 (venue A, post-fill) + 50 (venue B) = 120");
        t.check(report.aggregate_open_volume == 120, "exposure: process-wide aggregate matches per-symbol sum");
    }

    // ---- 10) deterministic multi-venue replay ------------------------------
    // (numbered to match the phase's own scenario order; run before the
    // 100-cycle and kill-switch tests below since both of those leave lasting
    // state -- the kill switch especially, see its own comment.)
    {
        for (int trial = 0; trial < 3; ++trial) {
            auto tv = make_two_venues();
            constexpr std::size_t per_venue = 500;
            auto flood = [&](oms::VenueId venue) {
                for (std::size_t i = 0; i < per_venue; ++i) {
                    oms::Completion c{};
                    (void)tv.registry.submit_create(venue, 0, ref_on(venue, i + 1), 0, 1, c);
                }
            };
            std::thread ta(flood, VENUE_A);
            std::thread tb(flood, VENUE_B);
            ta.join();
            tb.join();

            const auto d1 = oms::multi_venue_digest(tv.registry, per_venue + 16);
            const auto d2 = oms::multi_venue_digest(tv.registry, per_venue + 16);
            t.check(d1 == d2, "digest: replaying the same captured multi-venue state twice is reproducible");
        }
    }

    // ---- 11) 100 reconnect cycles with 2 venues ----------------------------
    {
        auto tv = make_two_venues();
        bool all_ready = true;
        for (int cycle = 0; cycle < 100; ++cycle) {
            auto* va = tv.registry.find(VENUE_A);
            auto* vb = tv.registry.find(VENUE_B);
            va->disconnect();
            vb->disconnect();
            (void)va->begin_recovery();
            (void)vb->begin_recovery();
            std::array<oms::ExecReport, 4> reports{};
            std::array<exec::Order, 16> snap{};
            std::array<oms::OrderSnapshot, 16> local{}, venue{};
            std::array<oms::OrderReconcileResult, 32> result{};
            const auto oa = va->run_reconciliation(0, reports.data(), reports.size(), snap.data(), snap.size(),
                                                    local.data(), local.size(), venue.data(), venue.size(),
                                                    result.data(), result.size());
            const auto ob = vb->run_reconciliation(0, reports.data(), reports.size(), snap.data(), snap.size(),
                                                    local.data(), local.size(), venue.data(), venue.size(),
                                                    result.data(), result.size());
            if (!oa.clean() || va->state() != oms::ConnectionState::ready ||
                !ob.clean() || vb->state() != oms::ConnectionState::ready) {
                all_ready = false;
                break;
            }
        }
        t.check(all_ready, "100 reconnect cycles across 2 venues: every cycle reaches READY on both");
    }

    // ---- 9) global kill switch ----------------------------------------------
    // MUST run last: RiskEngine::halt() has no reset (by design -- a real
    // kill switch is not self-clearing), and it is shared process-wide across
    // every ShardedOms (see sharded_oms.hpp's standing_engine()). Any test
    // block after this one that needs a create() to succeed would fail.
    {
        auto tv = make_two_venues();
        oms::Completion before{};
        t.check(tv.registry.submit_create(VENUE_A, 0, ref_on(VENUE_A, 1), 0, 1, before) && before.ok,
               "kill switch: order creation works before halt");

        t.check(!oms::ShardedOms::globally_halted(), "kill switch: not halted initially");
        oms::ShardedOms::halt_globally(risk::HaltReason::manual_kill);
        t.check(oms::ShardedOms::globally_halted(), "kill switch: halted() reflects the trip");

        oms::Completion after_a{}, after_b{};
        const bool submitted_a = tv.registry.submit_create(VENUE_A, 0, ref_on(VENUE_A, 2), 0, 1, after_a);
        const bool submitted_b = tv.registry.submit_create(VENUE_B, 0, ref_on(VENUE_B, 2), 0, 1, after_b);
        t.check(submitted_a && !after_a.ok, "kill switch: venue A create refused after halt (queue accepted, apply rejected)");
        t.check(submitted_b && !after_b.ok,
               "kill switch: venue B create refused after halt too -- authoritative across every venue, not just A");
    }

    return t.result();
}
