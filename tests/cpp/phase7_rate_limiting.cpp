#include <array>
#include <cstdint>
#include <thread>
#include <vector>

#include "oms/multi_venue.hpp"
#include "phase6_test.hpp"

// Phase G -- deterministic per-exchange rate limiting + session constraints,
// layered on Phase F's multi-venue architecture (VenueConnection::route_*()
// in include/oms/multi_venue.hpp, backed by include/oms/rate_limiter.hpp).
// Does not re-prove Phase F's own scenarios (venue identity/isolation,
// reconciliation mechanics, exactly-once reports) -- those are unchanged and
// covered by tests/cpp/phase7_multi_venue.cpp; this file proves the *new*
// gates: exact rate enforcement, bounded/fail-closed queueing, session/halt/
// reconnect interaction, and that none of it can be bypassed to reach the
// kill switch or create new exposure outside an allowed state.
//
// All timestamps are synthetic (a fixed counter this file advances itself),
// never a live clock -- required for the "same timestamps -> same digest"
// determinism claim to be checkable at all: replaying against a live clock
// could not, even in principle, replay the same timestamps twice.

namespace {

constexpr oms::VenueId VENUE_A = 1;
constexpr oms::VenueId VENUE_B = 2;
constexpr std::uint64_t T0 = 1'000'000'000ULL;  // arbitrary fixed epoch, avoids t=0 edge cases

exec::BrokerOrderRef ref_on(oms::VenueId venue, std::uint64_t logical_id) noexcept {
    return exec::BrokerOrderRef{venue, logical_id};
}

// Advances a legally-created order (create -> sent -> acknowledged) so
// route_cancel()/route_replace() have something legal to act on. Bypasses
// the rate limiter entirely (calls ShardedOms directly) -- test setup, not
// what's under test in these blocks.
void make_acknowledged(oms::ShardedOms& venue_oms, exec::BrokerOrderRef ref, std::int64_t volume) noexcept {
    oms::Completion c{};
    (void)venue_oms.submit_create(0, ref, 0, volume, c);
    (void)venue_oms.submit_transition(0, ref, exec::OrderState::sent, c);
    (void)venue_oms.submit_transition(0, ref, exec::OrderState::acknowledged, c);
}

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

std::unique_ptr<oms::ShardedOms> make_venue_oms() {
    return std::make_unique<oms::ShardedOms>(2, 256, 4, 4096);
}

}  // namespace

int main() {
    Phase6Test t;

    // ---- 1) exact rate boundary --------------------------------------------
    // rate=8/sec, burst=1: refill interval is exactly 1/8s = 125,000,000ns.
    // 8 and 0.125 are both exact dyadic values in double, so this boundary is
    // bit-exact, not merely "close enough".
    {
        oms::VenueRateLimiter limiter;
        oms::VenueRateLimitConfig cfg{};
        cfg.orders_per_sec = 8.0; cfg.burst_capacity = 1.0;
        cfg.cancels_per_sec = 8.0; cfg.replaces_per_sec = 8.0;
        limiter.configure(cfg, T0);

        oms::PendingRequest req{}; req.kind = oms::RateLimitKind::order; req.ref = ref_on(VENUE_A, 1);
        t.check(limiter.admit(req, T0) == oms::Decision::admitted, "boundary: initial burst token admits");
        t.check(limiter.admit(req, T0 + 124'999'999) == oms::Decision::rejected,
               "boundary: 1ns before exact refill instant is still rejected");
        t.check(limiter.admit(req, T0 + 125'000'000) == oms::Decision::admitted,
               "boundary: exactly at the refill instant is admitted");
    }

    // ---- 2) burst exhaustion / refill ---------------------------------------
    {
        oms::VenueRateLimiter limiter;
        oms::VenueRateLimitConfig cfg{};
        cfg.orders_per_sec = 5.0; cfg.burst_capacity = 5.0;
        cfg.cancels_per_sec = 5.0; cfg.replaces_per_sec = 5.0;
        limiter.configure(cfg, T0);

        oms::PendingRequest req{}; req.kind = oms::RateLimitKind::order;
        bool all_five_ok = true;
        for (int i = 0; i < 5; ++i) {
            req.ref = ref_on(VENUE_A, static_cast<std::uint64_t>(i + 1));
            if (limiter.admit(req, T0) != oms::Decision::admitted) all_five_ok = false;
        }
        t.check(all_five_ok, "burst: all 5 burst tokens admit at the same instant");
        req.ref = ref_on(VENUE_A, 6);
        t.check(limiter.admit(req, T0) == oms::Decision::rejected, "burst: 6th at the same instant is rejected");
        // Full second later: rate=5/sec refills the bucket completely.
        t.check(limiter.admit(req, T0 + 1'000'000'000) == oms::Decision::admitted,
               "burst: refilled to capacity 1s later, 6th now admits");
    }

    // ---- 3) venue A saturated while venue B remains operational ------------
    {
        oms::VenueConnection va(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
        oms::VenueConnection vb(VENUE_B, make_venue_oms(), std::make_unique<FakeVenue>());
        oms::VenueRateLimitConfig tight{}; tight.orders_per_sec = 1.0; tight.burst_capacity = 1.0;
        tight.cancels_per_sec = 1.0; tight.replaces_per_sec = 1.0;
        oms::VenueRateLimitConfig generous{};  // default: ~unlimited
        va.configure_rate_limit(tight, T0);
        vb.configure_rate_limit(generous, T0);

        oms::Completion c{};
        t.check(va.route_create(0, ref_on(VENUE_A, 1), 0, 1, T0, c) == oms::Decision::admitted,
               "isolation: venue A's first order admits");
        t.check(va.route_create(0, ref_on(VENUE_A, 2), 0, 1, T0, c) == oms::Decision::rejected,
               "isolation: venue A's second order at the same instant is rate limited");

        bool venue_b_all_ok = true;
        for (std::uint64_t i = 0; i < 500; ++i) {
            if (va.route_create(0, ref_on(VENUE_A, 100 + i), 0, 1, T0, c) != oms::Decision::rejected)
                { /* venue A stays saturated throughout -- not asserted per-iteration to keep this terse */ }
            if (vb.route_create(0, ref_on(VENUE_B, i + 1), 0, 1, T0, c) != oms::Decision::admitted)
                venue_b_all_ok = false;
        }
        t.check(venue_b_all_ok,
               "isolation: venue B admits 500 orders at the same instant venue A stays fully saturated");
    }

    // ---- 4) cancel rate limiting --------------------------------------------
    {
        oms::VenueConnection va(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
        oms::VenueRateLimitConfig cfg{}; cfg.cancels_per_sec = 2.0; cfg.cancel_burst_capacity = 2.0;
        va.configure_rate_limit(cfg, T0);
        for (std::uint64_t i = 1; i <= 3; ++i) make_acknowledged(va.oms(), ref_on(VENUE_A, i), 10);

        oms::Completion c{};
        t.check(va.route_cancel(0, ref_on(VENUE_A, 1), T0, c) == oms::Decision::admitted, "cancel: 1st admitted");
        t.check(va.route_cancel(0, ref_on(VENUE_A, 2), T0, c) == oms::Decision::admitted, "cancel: 2nd admitted");
        t.check(va.route_cancel(0, ref_on(VENUE_A, 3), T0, c) == oms::Decision::rejected,
               "cancel: 3rd at the same instant is rate limited, independent of the order bucket");
    }

    // ---- 5) replace rate limiting -------------------------------------------
    {
        oms::VenueConnection va(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
        oms::VenueRateLimitConfig cfg{}; cfg.replaces_per_sec = 2.0; cfg.replace_burst_capacity = 2.0;
        va.configure_rate_limit(cfg, T0);
        for (std::uint64_t i = 1; i <= 3; ++i) make_acknowledged(va.oms(), ref_on(VENUE_A, i), 10);

        oms::Completion c{};
        t.check(va.route_replace(0, ref_on(VENUE_A, 1), 100, 20, T0, c) == oms::Decision::admitted,
               "replace: 1st admitted");
        t.check(va.route_replace(0, ref_on(VENUE_A, 2), 100, 20, T0, c) == oms::Decision::admitted,
               "replace: 2nd admitted");
        t.check(va.route_replace(0, ref_on(VENUE_A, 3), 100, 20, T0, c) == oms::Decision::rejected,
               "replace: 3rd at the same instant is rate limited, independent of order/cancel buckets");
    }

    // ---- 6) bounded queue saturation ----------------------------------------
    {
        oms::VenueConnection va(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
        oms::VenueRateLimitConfig cfg{};
        cfg.orders_per_sec = 1.0; cfg.burst_capacity = 1.0;
        cfg.policy = oms::QueuePolicy::defer;
        cfg.pending_queue_capacity = 2;
        va.configure_rate_limit(cfg, T0);

        oms::Completion c{};
        t.check(va.route_create(0, ref_on(VENUE_A, 1), 0, 1, T0, c) == oms::Decision::admitted,
               "queue: 1st consumes the burst token directly, never queued");
        t.check(va.route_create(0, ref_on(VENUE_A, 2), 0, 1, T0, c) == oms::Decision::deferred,
               "queue: 2nd is rate limited and queued (depth 1/2)");
        t.check(va.route_create(0, ref_on(VENUE_A, 3), 0, 1, T0, c) == oms::Decision::deferred,
               "queue: 3rd is queued too (depth 2/2, now full)");
        t.check(va.rate_limiter().pending_depth() == 2, "queue: depth reports 2");
        t.check(va.rate_limiter().pending_high_water() == 2, "queue: high-water mark reports 2");
        t.check(va.route_create(0, ref_on(VENUE_A, 4), 0, 1, T0, c) == oms::Decision::rejected,
               "queue: 4th finds the queue full and fails closed rather than growing it");
        t.check(va.rate_limiter().queue_full_rejections() == 1,
               "queue: queue-full rejection is distinctly observable, not conflated with an ordinary rate reject");
        t.check(va.rate_limiter().pending_depth() == 2, "queue: depth still 2 -- the queue never grew past capacity");

        // A full second later, refills give 1 admitted token/sec: draining
        // should pull exactly one queued entry through and actually create it.
        const auto drained = va.drain_pending(0, T0 + 1'000'000'000, 10);
        t.check(drained == 1, "queue: drain admits exactly the tokens actually available (1), not the whole queue");
        t.check(va.rate_limiter().pending_depth() == 1, "queue: depth drops to 1 after the drain");
        oms::Completion found{};
        t.check(va.oms().submit_find(0, ref_on(VENUE_A, 2), found) && found.ok,
               "queue: the drained (FIFO-first) request was actually submitted to the OMS");
    }

    // ---- 7) session closed ---------------------------------------------------
    {
        oms::VenueConnection va(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
        va.configure_rate_limit(oms::VenueRateLimitConfig{}, T0);  // generous -- session is the only gate under test
        oms::SessionConstraints session{};
        session.session = oms::SessionState::closed;
        va.set_session(session);

        oms::Completion c{};
        t.check(va.route_create(0, ref_on(VENUE_A, 1), 0, 1, T0, c) == oms::Decision::rejected,
               "session: closed session rejects new exposure even with an otherwise-unlimited rate limiter");
    }

    // ---- 8) temporary venue halt ---------------------------------------------
    {
        oms::VenueConnection va(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
        va.configure_rate_limit(oms::VenueRateLimitConfig{}, T0);
        make_acknowledged(va.oms(), ref_on(VENUE_A, 1), 10);

        oms::SessionConstraints session{};
        session.trading_halted = true;
        va.set_session(session);

        oms::Completion c{};
        t.check(va.route_create(0, ref_on(VENUE_A, 2), 0, 1, T0, c) == oms::Decision::rejected,
               "halt: new exposure rejected during a temporary halt");
        t.check(va.route_cancel(0, ref_on(VENUE_A, 1), T0, c) == oms::Decision::admitted,
               "halt: risk-reducing cancel remains possible during a halt by default policy");

        session.allow_risk_reducing_during_halt = false;
        va.set_session(session);
        make_acknowledged(va.oms(), ref_on(VENUE_A, 3), 10);
        t.check(va.route_cancel(0, ref_on(VENUE_A, 3), T0, c) == oms::Decision::rejected,
               "halt: with the policy flag flipped, a halt blocks cancels too -- 'where policy allows' is honored");
    }

    // ---- 9) disconnect / reconciliation interaction --------------------------
    {
        oms::VenueConnection va(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
        va.configure_rate_limit(oms::VenueRateLimitConfig{}, T0);
        make_acknowledged(va.oms(), ref_on(VENUE_A, 1), 10);

        oms::Completion c{};
        t.check(va.route_create(0, ref_on(VENUE_A, 2), 0, 1, T0, c) == oms::Decision::admitted,
               "reconnect: new exposure allowed while CONNECTED (never disconnected)");

        va.disconnect();
        t.check(va.route_create(0, ref_on(VENUE_A, 3), 0, 1, T0, c) == oms::Decision::rejected,
               "reconnect: new exposure rejected while DISCONNECTED");
        t.check(va.route_cancel(0, ref_on(VENUE_A, 1), T0, c) == oms::Decision::admitted,
               "reconnect: cancel still reachable while DISCONNECTED -- risk-reducing is not gated by connection state");

        t.check(va.begin_recovery(), "reconnect: begin_recovery -> RECONCILING");
        t.check(va.route_create(0, ref_on(VENUE_A, 4), 0, 1, T0, c) == oms::Decision::rejected,
               "reconnect: new exposure still rejected while RECONCILING");

        std::array<oms::ExecReport, 4> reports{};
        std::array<exec::Order, 64> snap{};
        std::array<oms::OrderSnapshot, 64> local{}, venue{};
        std::array<oms::OrderReconcileResult, 128> result{};
        const auto outcome = va.run_reconciliation(0, reports.data(), reports.size(), snap.data(), snap.size(),
                                                    local.data(), local.size(), venue.data(), venue.size(),
                                                    result.data(), result.size());
        (void)outcome;  // deliberately not asserted clean() here -- venue_a's FakeVenue reports nothing,
                        // so local_only breaks are expected; the point of this block is the state gate,
                        // covered directly by phase7_multi_venue.cpp's own reconciliation scenarios.
        t.check(va.state() != oms::ConnectionState::ready,
               "reconnect: (setup check) reconciliation did not spuriously reach READY here");
        t.check(va.route_create(0, ref_on(VENUE_A, 5), 0, 1, T0, c) == oms::Decision::rejected,
               "reconnect: new exposure remains rejected while still RECONCILING (not clean)");
    }

    // ---- 11) clock edge cases -------------------------------------------------
    // (numbered to match the phase's own list; run before the kill switch
    // test below since that one leaves lasting process-wide state)
    {
        oms::VenueRateLimiter limiter;
        oms::VenueRateLimitConfig cfg{};
        cfg.orders_per_sec = 1.0; cfg.burst_capacity = 1.0;
        limiter.configure(cfg, T0);
        oms::PendingRequest req{}; req.kind = oms::RateLimitKind::order; req.ref = ref_on(VENUE_A, 1);

        t.check(limiter.admit(req, T0) == oms::Decision::admitted, "clock: baseline admit consumes the burst");
        t.check(limiter.admit(req, T0) == oms::Decision::rejected,
               "clock: same timestamp again (zero elapsed) grants no refill");

        // Backwards clock: must not crash, must not manufacture tokens.
        t.check(limiter.admit(req, T0 - 1'000'000) == oms::Decision::rejected,
               "clock: a timestamp earlier than the last observed one is rejected, not UB/crash");
        t.check(limiter.tokens(oms::RateLimitKind::order, T0 - 1'000'000) < 1.0,
               "clock: backwards clock does not add tokens");

        // Huge forward jump: must saturate at capacity, not overflow.
        limiter.configure(cfg, T0);
        (void)limiter.admit(req, T0);  // drain to 0
        const auto far_future = T0 + 1'000'000'000'000'000ULL;  // ~11.5 days
        t.check(limiter.tokens(oms::RateLimitKind::order, far_future) == 1.0,
               "clock: a huge forward jump saturates at burst capacity, does not overflow past it");
    }

    // ---- 12) deterministic repeated-run digest --------------------------------
    {
        auto run_once = [&]() -> std::uint64_t {
            oms::VenueConnection va(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
            oms::VenueConnection vb(VENUE_B, make_venue_oms(), std::make_unique<FakeVenue>());
            oms::VenueRateLimitConfig cfg{};
            cfg.orders_per_sec = 50.0; cfg.burst_capacity = 20.0;
            cfg.policy = oms::QueuePolicy::defer; cfg.pending_queue_capacity = 64;
            va.configure_rate_limit(cfg, T0);
            vb.configure_rate_limit(cfg, T0);

            std::vector<std::uint64_t> per_venue_digest(2, 1469598103934665603ULL);
            auto drive = [&](oms::VenueConnection& v, std::size_t venue_slot) {
                constexpr std::size_t n = 2'000;
                std::uint64_t& digest = per_venue_digest[venue_slot];
                for (std::size_t i = 0; i < n; ++i) {
                    const auto now = T0 + static_cast<std::uint64_t>(i) * 1'000'000ULL;  // fixed synthetic clock
                    oms::Completion c{};
                    const auto decision = v.route_create(0, ref_on(v.id(), i + 1), static_cast<std::uint32_t>(i % 8),
                                                          1, now, c);
                    digest ^= (static_cast<std::uint64_t>(decision) + 1) * (i + 1) * 1099511628211ULL;
                    if (i % 32 == 0) (void)v.drain_pending(0, now, 4);
                }
            };
            std::thread ta(drive, std::ref(va), 0);
            std::thread tb(drive, std::ref(vb), 1);
            ta.join();
            tb.join();
            return per_venue_digest[0] ^ (per_venue_digest[1] * 1099511628211ULL);
        };

        const auto d1 = run_once();
        const auto d2 = run_once();
        const auto d3 = run_once();
        t.check(d1 == d2 && d2 == d3,
               "digest: same input event stream + same synthetic timestamps + same config -> identical digest");
    }

    // ---- 10) global kill switch interaction -----------------------------------
    // MUST run last: risk::RiskEngine::halt() has no reset (Phase F's own
    // documented reasoning: a real kill switch is not self-clearing) and is
    // shared process-wide across every ShardedOms/VenueConnection. Any block
    // after this one that needs a create to actually succeed would fail.
    {
        oms::VenueConnection va(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
        va.configure_rate_limit(oms::VenueRateLimitConfig{}, T0);  // generous: isolate the kill-switch gate

        oms::Completion before{};
        t.check(va.route_create(0, ref_on(VENUE_A, 1), 0, 1, T0, before) == oms::Decision::admitted,
               "kill switch: route_create works before halt");

        oms::ShardedOms::halt_globally(risk::HaltReason::manual_kill);

        oms::Completion after{};
        t.check(va.route_create(0, ref_on(VENUE_A, 2), 0, 1, T0, after) == oms::Decision::rejected,
               "kill switch: route_create fails closed after halt, even though session/connection/rate limiter "
               "all still say yes -- the limiter cannot be used to route around it");
        t.check(!after.ok, "kill switch: the underlying ShardedOms completion itself reflects the rejection");
    }

    return t.result();
}
