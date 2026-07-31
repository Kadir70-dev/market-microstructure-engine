#include <chrono>
#include <cstdint>
#include <iostream>

#include "book/order_book.hpp"
#include "core/clock.hpp"
#include "features/feature_engine.hpp"
#include "telemetry/histogram.hpp"

// Phase 3 benchmark. Part 18 targets:
//   book update  p99 < 2 us
//   feature vector p99 < 5 us
//   throughput   >= 500k updates/s
//
// Latency is measured per operation into an HDR histogram; throughput is
// measured over the whole run so it includes the measurement overhead rather
// than flattering the result by excluding it.

namespace {
constexpr std::uint64_t iterations = 2'000'000;
}

int main() {
    book::OrderBook order_book(1);
    features::FeatureEngine engine(32);
    telemetry::Histogram<> book_ns;
    telemetry::Histogram<> feature_ns;

    // Warm the book so the L2 path exercises a populated ladder rather than a
    // trivially empty one.
    for (std::int64_t i = 0; i < 32; ++i) {
        order_book.apply_delta(book::Side::bid, 1000 - i, 10, book::DeltaAction::add, 0);
        order_book.apply_delta(book::Side::ask, 1100 + i, 10, book::DeltaAction::add, 0);
    }

    const auto wall_start = core::monotonic_now_ns();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        const auto price = 1000 - static_cast<std::int64_t>(i % 32);

        const auto b0 = core::monotonic_now_ns();
        order_book.apply_delta(book::Side::bid, price, 10 + static_cast<std::int64_t>(i % 5),
                               book::DeltaAction::modify, b0);
        const auto b1 = core::monotonic_now_ns();
        book_ns.record(b1 - b0);

        const auto f0 = core::monotonic_now_ns();
        const auto vector = engine.compute(order_book, book::BookSource::dom_aggregated);
        const auto f1 = core::monotonic_now_ns();
        feature_ns.record(f1 - f0);
        if (vector.warm_mask == 0xFFFFFFFFU) std::cout << "";  // defeat elision
    }
    const auto wall_ns = core::monotonic_now_ns() - wall_start;

    const double seconds = static_cast<double>(wall_ns) / 1e9;
    const double throughput = static_cast<double>(iterations) / seconds;

    std::cout << "PHASE3_BENCH iterations=" << iterations
              << " wall_seconds=" << seconds
              << " updates_per_second=" << throughput
              << " book_p50_ns=" << book_ns.percentile(0.50)
              << " book_p99_ns=" << book_ns.percentile(0.99)
              << " feature_p50_ns=" << feature_ns.percentile(0.50)
              << " feature_p99_ns=" << feature_ns.percentile(0.99) << '\n';

    const bool book_ok = book_ns.percentile(0.99) < 2'000;
    const bool feature_ok = feature_ns.percentile(0.99) < 5'000;
    const bool throughput_ok = throughput >= 500'000.0;
    std::cout << "PHASE3_TARGETS book_p99_under_2us=" << (book_ok ? 1 : 0)
              << " feature_p99_under_5us=" << (feature_ok ? 1 : 0)
              << " throughput_over_500k=" << (throughput_ok ? 1 : 0) << '\n';
    return (book_ok && feature_ok && throughput_ok) ? 0 : 1;
}
