#include <cstdint>
#include <iostream>

#include "book/order_book.hpp"
#include "core/clock.hpp"
#include "features/feature_engine.hpp"
#include "strategy/baseline_scalper.hpp"
#include "telemetry/histogram.hpp"

// Phase 7 benchmark. Part 18 target: decision p99 < 5 us.
//
// Measures the strategy decision alone — signal computation plus every filter
// plus the FSM step. Book and feature updates are timed separately so the
// decision figure is not inflated by work that Phase 3 already benchmarks.

int main() {
    strategy::StrategyConfig config{};
    config.volume = 1;
    config.entry_queue_imbalance = 0.30;
    config.require_ofi_agreement = true;
    config.max_spread_ticks = 50;
    config.take_profit_ticks = 10;
    config.stop_loss_ticks = 10;
    config.max_hold_ns = 5'000'000;
    config.cooldown_ns = 1'000'000;

    strategy::BaselineScalper scalper(config);
    book::OrderBook order_book(0);
    features::FeatureEngine features(32);
    telemetry::Histogram<> decision_ns;

    constexpr std::uint64_t iterations = 1'000'000;
    std::uint64_t ts = 1'000'000;
    std::uint64_t actionable = 0;

    const auto start = core::monotonic_now_ns();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        // Walk the book so imbalance, OFI and momentum all move.
        const std::int64_t drift = static_cast<std::int64_t>(i % 64);
        const std::int64_t bid_size = 10 + static_cast<std::int64_t>(i % 90);
        const std::int64_t ask_size = 10 + static_cast<std::int64_t>((i * 7) % 90);
        (void)order_book.apply_quote(1'000 + drift, 1'010 + drift, bid_size, ask_size, ts);
        const auto vector = features.compute(order_book, book::BookSource::dom_aggregated);

        const auto d0 = core::monotonic_now_ns();
        const auto intent = scalper.on_market(0, order_book, vector,
                                              book::BookSource::dom_aggregated, ts);
        const auto d1 = core::monotonic_now_ns();
        decision_ns.record(d1 - d0);

        if (intent.actionable()) {
            ++actionable;
            // Close the loop so the FSM keeps cycling rather than parking in
            // entry_pending for the rest of the run.
            if (intent.kind == strategy::IntentKind::enter)
                scalper.on_entry_filled(intent.side, intent.volume, 1'010 + drift, ts);
            else
                scalper.on_exit_filled(ts);
        }
        ts += 1'000;
    }
    const auto wall_ns = core::monotonic_now_ns() - start;

    const double seconds = static_cast<double>(wall_ns) / 1e9;
    const double decisions_per_second =
        seconds > 0.0 ? static_cast<double>(iterations) / seconds : 0.0;

    std::cout << "PHASE7_BENCH iterations=" << iterations
              << " actionable=" << actionable
              << " entries=" << scalper.entries_emitted()
              << " exits=" << scalper.exits_emitted()
              << " vetoes=" << scalper.vetoes()
              << " wall_seconds=" << seconds
              << " decisions_per_second=" << decisions_per_second
              << " decision_p50_ns=" << decision_ns.percentile(0.50)
              << " decision_p99_ns=" << decision_ns.percentile(0.99)
              << " decision_max_ns=" << decision_ns.percentile(1.0) << '\n';

    const bool p99_ok = decision_ns.percentile(0.99) < 5'000;
    std::cout << "PHASE7_TARGETS decision_p99_under_5us=" << (p99_ok ? 1 : 0) << '\n';
    return p99_ok ? 0 : 1;
}
