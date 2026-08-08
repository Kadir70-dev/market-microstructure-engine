#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "oms/multi_venue.hpp"
#include "oms/strategy_scheduler.hpp"
#include "phase6_test.hpp"

// Phase H -- deterministic multi-strategy scheduling and fair arbitration,
// layered on Phase F (multi-venue) and Phase G (rate limiting/session).
// Does not re-prove Phase F/G's own scenarios (those are unchanged and
// covered by tests/cpp/phase7_multi_venue.cpp and phase7_rate_limiting.cpp);
// this file proves the new arbitration layer: fairness, no-starvation,
// determinism, and that it never bypasses anything downstream.

namespace {

constexpr oms::VenueId VENUE_A = 1;
constexpr oms::VenueId VENUE_B = 2;
constexpr std::uint64_t T0 = 1'000'000'000ULL;

class FakeVenue final : public oms::VenueAdapter {
public:
    [[nodiscard]] std::size_t fetch_open_orders(oms::OrderSnapshot*, std::size_t) const noexcept override {
        return 0;
    }
    [[nodiscard]] std::size_t fetch_positions(oms::PositionSnapshot*, std::size_t) const noexcept override {
        return 0;
    }
    [[nodiscard]] std::size_t fetch_recent_reports(oms::ExecReport*, std::size_t) const noexcept override {
        return 0;
    }
    [[nodiscard]] exec::AccountState fetch_account() const noexcept override { return exec::AccountState{}; }
};

std::unique_ptr<oms::ShardedOms> make_venue_oms(std::size_t cap = 4096) {
    return std::make_unique<oms::ShardedOms>(2, cap, 4, cap + 64);
}

exec::BrokerOrderRef ref_on(oms::VenueId venue, std::uint64_t logical_id) noexcept {
    return exec::BrokerOrderRef{venue, logical_id};
}

oms::PendingRequest order_req(exec::BrokerOrderRef ref, std::uint32_t symbol, std::int64_t volume) noexcept {
    oms::PendingRequest r{};
    r.kind = oms::RateLimitKind::order;
    r.ref = ref;
    r.symbol = symbol;
    r.volume = volume;
    return r;
}

// Pops one arbitrated request and routes it through the real, unmodified
// Phase F/G path (VenueConnection::route_create), returning the routing
// Decision -- this is the actual end-to-end integration under test, not a
// mock of it.
oms::Decision dispatch_one(oms::StrategyScheduler& sched, oms::VenueRegistry& registry, std::size_t producer_id,
                           std::uint64_t now_ns) {
    oms::ArbitratedRequest req{};
    if (!sched.next(req)) return oms::Decision::rejected;  // nothing queued -- treated as a no-op by callers below
    auto* venue = registry.find(req.venue);
    if (venue == nullptr) return oms::Decision::rejected;
    oms::Completion c{};
    return venue->route_create(producer_id, req.request.ref, req.request.symbol, req.request.volume, now_ns, c);
}

}  // namespace

int main() {
    Phase6Test t;

    // ---- 1) 2 strategies, same venue, equal weight -------------------------
    {
        oms::VenueRegistry registry;
        registry.add(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
        registry.find(VENUE_A)->configure_rate_limit(oms::VenueRateLimitConfig{}, T0);  // unlimited: isolate arbitration

        oms::StrategyScheduler sched(4, 64);
        t.check(sched.register_strategy(1, {1}), "2-strat: register strategy 1");
        t.check(sched.register_strategy(2, {1}), "2-strat: register strategy 2");
        for (std::uint64_t i = 0; i < 10; ++i) {
            t.check(sched.enqueue(1, VENUE_A, exec::Side::buy, order_req(ref_on(VENUE_A, 100 + i), 0, 1)),
                   "2-strat: strategy 1 enqueue");
            t.check(sched.enqueue(2, VENUE_A, exec::Side::buy, order_req(ref_on(VENUE_A, 200 + i), 0, 1)),
                   "2-strat: strategy 2 enqueue");
        }
        for (int i = 0; i < 20; ++i) t.check(dispatch_one(sched, registry, 0, T0) == oms::Decision::admitted,
                                             "2-strat: every dispatch admits (unlimited rate)");
        t.check(sched.admitted_count(1) == 10 && sched.admitted_count(2) == 10,
               "2-strat: equal weight -> exactly equal service (10 each)");
    }

    // ---- 2) 4+ strategies, same venue ---------------------------------------
    {
        oms::VenueRegistry registry;
        registry.add(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
        registry.find(VENUE_A)->configure_rate_limit(oms::VenueRateLimitConfig{}, T0);
        oms::StrategyScheduler sched(5, 64);
        for (oms::StrategyId s = 1; s <= 5; ++s) t.check(sched.register_strategy(s, {1}), "5-strat: register");
        for (oms::StrategyId s = 1; s <= 5; ++s)
            for (std::uint64_t i = 0; i < 8; ++i)
                t.check(sched.enqueue(s, VENUE_A, exec::Side::buy, order_req(ref_on(VENUE_A, s * 1000 + i), 0, 1)),
                       "5-strat: enqueue");
        for (int i = 0; i < 40; ++i) (void)dispatch_one(sched, registry, 0, T0);
        bool all_equal = true;
        for (oms::StrategyId s = 1; s <= 5; ++s) if (sched.admitted_count(s) != 8) all_equal = false;
        t.check(all_equal, "5-strat: 5 equal-weight strategies each get exactly 1/5 of throughput (8 each)");
    }

    // ---- 3) multiple strategies across 2 venues -----------------------------
    {
        oms::VenueRegistry registry;
        registry.add(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
        registry.add(VENUE_B, make_venue_oms(), std::make_unique<FakeVenue>());
        registry.find(VENUE_A)->configure_rate_limit(oms::VenueRateLimitConfig{}, T0);
        registry.find(VENUE_B)->configure_rate_limit(oms::VenueRateLimitConfig{}, T0);
        oms::StrategyScheduler sched(3, 64);
        t.check(sched.register_strategy(1, {1}) && sched.register_strategy(2, {1}) && sched.register_strategy(3, {1}),
               "cross-venue: register 3 strategies");
        // strategy 1 -> venue A only, strategy 2 -> venue B only, strategy 3 -> both
        for (std::uint64_t i = 0; i < 5; ++i) {
            (void)sched.enqueue(1, VENUE_A, exec::Side::buy, order_req(ref_on(VENUE_A, i + 1), 0, 1));
            (void)sched.enqueue(2, VENUE_B, exec::Side::buy, order_req(ref_on(VENUE_B, i + 1), 0, 1));
            (void)sched.enqueue(3, (i % 2 == 0) ? VENUE_A : VENUE_B, exec::Side::buy,
                                order_req(ref_on((i % 2 == 0) ? VENUE_A : VENUE_B, 900 + i), 1, 1));
        }
        int admitted = 0;
        for (int i = 0; i < 15; ++i) if (dispatch_one(sched, registry, 0, T0) == oms::Decision::admitted) ++admitted;
        t.check(admitted == 15, "cross-venue: all 15 requests admit -- one strategy's cross-venue "
                                "requests interleave with venue-exclusive strategies without conflict");
        oms::Completion probe{};
        t.check(registry.find(VENUE_A)->oms().submit_find(0, ref_on(VENUE_A, 1), probe) && probe.ok,
               "cross-venue: strategy 1's order actually landed on venue A");
        t.check(registry.find(VENUE_B)->oms().submit_find(0, ref_on(VENUE_B, 1), probe) && probe.ok,
               "cross-venue: strategy 2's order actually landed on venue B");
    }

    // ---- 4) unequal weights ---------------------------------------------------
    {
        oms::VenueRegistry registry;
        registry.add(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
        registry.find(VENUE_A)->configure_rate_limit(oms::VenueRateLimitConfig{}, T0);
        oms::StrategyScheduler sched(2, 128);
        t.check(sched.register_strategy(1, {3}), "weights: strategy 1 quantum=3");
        t.check(sched.register_strategy(2, {1}), "weights: strategy 2 quantum=1");
        for (std::uint64_t i = 0; i < 30; ++i) {
            (void)sched.enqueue(1, VENUE_A, exec::Side::buy, order_req(ref_on(VENUE_A, 1000 + i), 0, 1));
            (void)sched.enqueue(2, VENUE_A, exec::Side::buy, order_req(ref_on(VENUE_A, 2000 + i), 0, 1));
        }
        for (int i = 0; i < 40; ++i) (void)dispatch_one(sched, registry, 0, T0);
        t.check(sched.admitted_count(1) == 30 && sched.admitted_count(2) == 10,
               "weights: 3:1 quantum ratio -> 3:1 service ratio (30 vs 10) over 40 dispatches");
    }

    // ---- 5) starvation bound ----------------------------------------------------
    {
        oms::VenueRegistry registry;
        registry.add(VENUE_A, make_venue_oms(20'000), std::make_unique<FakeVenue>());
        registry.find(VENUE_A)->configure_rate_limit(oms::VenueRateLimitConfig{}, T0);
        oms::StrategyScheduler sched(3, 20'000);
        // Two "heavy" strategies with large quanta constantly refilled, one
        // "light" strategy with quantum=1 enqueuing occasionally -- the
        // theoretical worst-case wait for the light strategy is bounded by
        // the *other* active strategies' quanta (10+10=20 requests), never
        // unbounded regardless of how much the heavy strategies flood.
        t.check(sched.register_strategy(1, {10}) && sched.register_strategy(2, {10}) && sched.register_strategy(3, {1}),
               "starvation: register 2 heavy (q=10) + 1 light (q=1)");
        std::uint64_t next_id = 1;
        for (int i = 0; i < 100; ++i) (void)sched.enqueue(1, VENUE_A, exec::Side::buy,
                                                          order_req(ref_on(VENUE_A, next_id++), 0, 1));
        for (int i = 0; i < 100; ++i) (void)sched.enqueue(2, VENUE_A, exec::Side::buy,
                                                          order_req(ref_on(VENUE_A, next_id++), 0, 1));
        (void)sched.enqueue(3, VENUE_A, exec::Side::buy, order_req(ref_on(VENUE_A, next_id++), 0, 1));

        std::size_t dispatches_until_strategy_3 = 0;
        bool strategy_3_served = false;
        for (std::size_t i = 0; i < 100 && !strategy_3_served; ++i) {
            oms::ArbitratedRequest req{};
            (void)sched.next(req);
            ++dispatches_until_strategy_3;
            if (req.strategy == 3) strategy_3_served = true;
        }
        t.check(strategy_3_served, "starvation: strategy 3 (light, single item) does get served");
        t.check(dispatches_until_strategy_3 <= 21,
               "starvation: served within the provable bound (sum of other active quanta = 20, +1 for itself)");
    }

    // ---- 6) one strategy flooding queue -----------------------------------------
    {
        oms::VenueRegistry registry;
        registry.add(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
        registry.find(VENUE_A)->configure_rate_limit(oms::VenueRateLimitConfig{}, T0);
        oms::StrategyScheduler sched(2, 32);
        t.check(sched.register_strategy(1, {1}) && sched.register_strategy(2, {1}), "flood: register 2 equal");
        std::size_t flooded = 0;
        for (int i = 0; i < 1000; ++i)
            if (sched.enqueue(1, VENUE_A, exec::Side::buy, order_req(ref_on(VENUE_A, 5000 + i), 0, 1))) ++flooded;
        t.check(flooded == 32, "flood: strategy 1's own queue caps at its 32-entry bound, no unbounded growth");
        (void)sched.enqueue(2, VENUE_A, exec::Side::buy, order_req(ref_on(VENUE_A, 1), 0, 1));
        oms::ArbitratedRequest first{};
        (void)sched.next(first);
        t.check(first.strategy == 1, "flood: strategy 1 still gets served first (round-robin order preserved)");
        oms::ArbitratedRequest second{};
        (void)sched.next(second);
        t.check(second.strategy == 2,
               "flood: strategy 2's single request is NOT starved by strategy 1's full queue -- served 2nd, not last");
    }

    // ---- 7) queue full / fail-closed ---------------------------------------------
    {
        oms::StrategyScheduler sched(1, 4);
        t.check(sched.register_strategy(1, {1}), "queue-full: register");
        int ok_count = 0;
        for (int i = 0; i < 6; ++i)
            if (sched.enqueue(1, VENUE_A, exec::Side::buy, order_req(ref_on(VENUE_A, i + 1), 0, 1))) ++ok_count;
        t.check(ok_count == 4, "queue-full: only the first 4 (capacity) enqueue successfully");
        t.check(sched.queue_full_rejections(1) == 2, "queue-full: the other 2 are observably rejected, not dropped silently");
        t.check(sched.queue_depth(1) == 4 && sched.queue_high_water(1) == 4,
               "queue-full: depth/high-water both report exactly capacity, never more");
    }

    // ---- 8) self-trade prevention interaction -------------------------------------
    {
        oms::SelfTradeTracker guard(/*allow_cross_strategy_opposition=*/false);
        t.check(guard.check_and_apply(VENUE_A, 0, /*strategy=*/1, exec::Side::buy, 100),
               "self-trade: strategy 1 opens a long position, no prior owner -> OK");
        t.check(!guard.check_and_apply(VENUE_A, 0, /*strategy=*/2, exec::Side::sell, 50),
               "self-trade: strategy 2 selling against strategy 1's long is blocked (hedging, opposition disallowed)");
        t.check(guard.check_and_apply(VENUE_A, 0, /*strategy=*/1, exec::Side::buy, 20),
               "self-trade: strategy 1 adding to its own long (same strategy) is never blocked");
        t.check(guard.check_and_apply(VENUE_A, 1, /*strategy=*/2, exec::Side::sell, 50),
               "self-trade: a different symbol has no cross-contamination -- strategy 2 sells freely there");

        oms::SelfTradeTracker permissive(/*allow_cross_strategy_opposition=*/true);
        t.check(permissive.check_and_apply(VENUE_A, 0, 1, exec::Side::buy, 100), "self-trade: permissive setup, strategy 1 long");
        t.check(permissive.check_and_apply(VENUE_A, 0, 2, exec::Side::sell, 50),
               "self-trade: with allow_cross_strategy_opposition=true, the same opposing trade is allowed");
    }

    // ---- 9) venue rate-limit interaction -------------------------------------------
    {
        oms::VenueRegistry registry;
        registry.add(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
        oms::VenueRateLimitConfig tight{}; tight.orders_per_sec = 1.0; tight.burst_capacity = 1.0;
        registry.find(VENUE_A)->configure_rate_limit(tight, T0);
        oms::StrategyScheduler sched(2, 16);
        t.check(sched.register_strategy(1, {1}) && sched.register_strategy(2, {1}), "rate-interact: register");
        for (int i = 0; i < 3; ++i) {
            (void)sched.enqueue(1, VENUE_A, exec::Side::buy, order_req(ref_on(VENUE_A, 10 + i), 0, 1));
            (void)sched.enqueue(2, VENUE_A, exec::Side::buy, order_req(ref_on(VENUE_A, 20 + i), 0, 1));
        }
        const auto d1 = dispatch_one(sched, registry, 0, T0);
        const auto d2 = dispatch_one(sched, registry, 0, T0);
        t.check(d1 == oms::Decision::admitted, "rate-interact: arbitration's first pick still consumes the venue's only burst token");
        t.check(d2 == oms::Decision::rejected,
               "rate-interact: the very next arbitrated pick is rejected by the (unchanged) Phase G rate limiter -- "
               "arbitration decided *order*, the limiter still decided *admission*");
    }

    // ---- 10) session close interaction ---------------------------------------------
    {
        oms::VenueRegistry registry;
        registry.add(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
        registry.find(VENUE_A)->configure_rate_limit(oms::VenueRateLimitConfig{}, T0);
        oms::SessionConstraints closed{}; closed.session = oms::SessionState::closed;
        registry.find(VENUE_A)->set_session(closed);
        oms::StrategyScheduler sched(1, 4);
        t.check(sched.register_strategy(1, {1}), "session-interact: register");
        (void)sched.enqueue(1, VENUE_A, exec::Side::buy, order_req(ref_on(VENUE_A, 1), 0, 1));
        t.check(dispatch_one(sched, registry, 0, T0) == oms::Decision::rejected,
               "session-interact: a closed session still rejects the arbitrated request -- unchanged Phase G gate");
    }

    // ---- 11) deterministic repeated-run digest --------------------------------------
    {
        auto run_once = [&]() -> std::uint64_t {
            oms::VenueRegistry registry;
            registry.add(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
            registry.add(VENUE_B, make_venue_oms(), std::make_unique<FakeVenue>());
            registry.find(VENUE_A)->configure_rate_limit(oms::VenueRateLimitConfig{}, T0);
            registry.find(VENUE_B)->configure_rate_limit(oms::VenueRateLimitConfig{}, T0);
            oms::StrategyScheduler sched(4, 512);
            for (oms::StrategyId s = 1; s <= 4; ++s) (void)sched.register_strategy(s, {s});  // weights 1,2,3,4
            for (oms::StrategyId s = 1; s <= 4; ++s)
                for (std::uint64_t i = 0; i < 100; ++i)
                    (void)sched.enqueue(s, (i % 2 == 0) ? VENUE_A : VENUE_B, exec::Side::buy,
                                        order_req(ref_on((i % 2 == 0) ? VENUE_A : VENUE_B, s * 10000 + i), 0, 1));
            std::uint64_t digest = 1469598103934665603ULL;
            for (int i = 0; i < 400; ++i) {
                oms::ArbitratedRequest req{};
                const bool had = sched.next(req);
                digest ^= (had ? (static_cast<std::uint64_t>(req.strategy) * 1000 + req.venue + 1) : 0);
                digest *= 1099511628211ULL;
            }
            return digest;
        };
        const auto d1 = run_once();
        const auto d2 = run_once();
        const auto d3 = run_once();
        t.check(d1 == d2 && d2 == d3,
               "digest: same enqueue sequence + same config -> identical arbitration order, every run");
    }

    // ---- 12) global kill-switch interaction ------------------------------------------
    // MUST run last: matches Phase F/G's own established discipline --
    // risk::RiskEngine::halt() has no reset and is shared process-wide.
    {
        oms::VenueRegistry registry;
        registry.add(VENUE_A, make_venue_oms(), std::make_unique<FakeVenue>());
        registry.find(VENUE_A)->configure_rate_limit(oms::VenueRateLimitConfig{}, T0);
        oms::StrategyScheduler sched(1, 4);
        t.check(sched.register_strategy(1, {1}), "kill-switch: register");
        (void)sched.enqueue(1, VENUE_A, exec::Side::buy, order_req(ref_on(VENUE_A, 1), 0, 1));
        t.check(dispatch_one(sched, registry, 0, T0) == oms::Decision::admitted, "kill-switch: works before halt");

        oms::ShardedOms::halt_globally(risk::HaltReason::manual_kill);
        (void)sched.enqueue(1, VENUE_A, exec::Side::buy, order_req(ref_on(VENUE_A, 2), 0, 1));
        t.check(dispatch_one(sched, registry, 0, T0) == oms::Decision::rejected,
               "kill-switch: arbitrated request still fails closed after halt -- arbitration cannot route around it");
    }

    return t.result();
}
