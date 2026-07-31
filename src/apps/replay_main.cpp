#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "core/clock.hpp"
#include "replay/replay_engine.hpp"

// Phase 4 — replay driver and determinism gate.
//
//   replay_main <wal-dir> [runs]
//
// Runs the replay `runs` times and asserts every run produced an identical
// digest. Part 18 Phase 4: "determinism passes 100 consecutive runs. Nothing
// proceeds until it does." Exit is non-zero on any mismatch, any corrupt WAL,
// or any failed target, so this is usable directly as a gate in CI.

namespace {
const char* error_name(feed::ReplayError error) noexcept {
    switch (error) {
        case feed::ReplayError::none: return "none";
        case feed::ReplayError::not_open: return "not_open";
        case feed::ReplayError::catalog_scan_failed: return "catalog_scan_failed";
        case feed::ReplayError::catalog_empty: return "catalog_empty";
        case feed::ReplayError::catalog_not_contiguous: return "catalog_not_contiguous";
        case feed::ReplayError::segment_open_failed: return "segment_open_failed";
        case feed::ReplayError::corrupt_frame: return "corrupt_frame";
        case feed::ReplayError::order_regression: return "order_regression";
    }
    return "unknown";
}
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: replay_main <wal-dir> [runs]\n";
        return 2;
    }
    const std::filesystem::path directory(argv[1]);
    const std::uint64_t runs = (argc == 3) ? std::strtoull(argv[2], nullptr, 10) : 100;
    if (runs == 0) { std::cerr << "runs must be >= 1\n"; return 2; }

    replay::ReplayEngine engine;

    const auto first_start = core::monotonic_now_ns();
    const auto first = engine.run(directory);
    const auto first_wall_ns = core::monotonic_now_ns() - first_start;

    if (!first.ok) {
        std::cerr << "REPLAY_FAIL error=" << error_name(first.error) << '\n';
        return 1;
    }

    std::cout << "REPLAY_RUN events=" << first.events
              << " quotes=" << first.quotes
              << " heartbeats=" << first.heartbeats
              << " other=" << first.other_events
              << " feature_vectors=" << first.feature_vectors
              << " rejected=" << first.rejected_updates
              << " crossed_halts=" << first.crossed_halts
              << " segments=" << first.segments
              << " span_ns=" << first.span_ns()
              << " digest=" << first.digest << '\n';

    // Determinism gate.
    std::uint64_t mismatches = 0;
    const auto gate_start = core::monotonic_now_ns();
    for (std::uint64_t i = 1; i < runs; ++i) {
        const auto next = engine.run(directory);
        if (!next.ok) {
            std::cerr << "REPLAY_FAIL run=" << i << " error=" << error_name(next.error) << '\n';
            return 1;
        }
        if (next.digest != first.digest || next.events != first.events) {
            ++mismatches;
            std::cerr << "DETERMINISM_MISMATCH run=" << i
                      << " expected_digest=" << first.digest
                      << " actual_digest=" << next.digest
                      << " expected_events=" << first.events
                      << " actual_events=" << next.events << '\n';
        }
    }
    const auto gate_wall_ns = core::monotonic_now_ns() - gate_start;

    std::cout << "REPLAY_DETERMINISM runs=" << runs
              << " mismatches=" << mismatches
              << " identical=" << (mismatches == 0 ? 1 : 0) << '\n';

    // Benchmark. Throughput is taken from the first run so it measures a cold
    // replay rather than a cache-warmed repeat.
    const double seconds = static_cast<double>(first_wall_ns) / 1e9;
    const double events_per_second = seconds > 0.0 ? static_cast<double>(first.events) / seconds : 0.0;
    const double realtime_multiple =
        (first_wall_ns > 0 && first.span_ns() > 0)
            ? static_cast<double>(first.span_ns()) / static_cast<double>(first_wall_ns)
            : 0.0;

    std::cout << "REPLAY_BENCH wall_seconds=" << seconds
              << " events_per_second=" << events_per_second
              << " realtime_multiple=" << realtime_multiple
              << " gate_seconds=" << (static_cast<double>(gate_wall_ns) / 1e9) << '\n';

    const bool throughput_ok = events_per_second >= 100'000.0;
    const bool realtime_ok = realtime_multiple >= 100.0;
    std::cout << "PHASE4_TARGETS determinism=" << (mismatches == 0 ? 1 : 0)
              << " events_per_second_over_100k=" << (throughput_ok ? 1 : 0)
              << " realtime_multiple_over_100x=" << (realtime_ok ? 1 : 0) << '\n';

    return (mismatches == 0 && throughput_ok && realtime_ok) ? 0 : 1;
}
