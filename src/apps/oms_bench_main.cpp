#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

#include "oms/oms.hpp"
#include "telemetry/histogram.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

// Phase B benchmark: create/find/transition/cancel latency and memory at
// realistic occupancies (1, 1K, 100K, 500K, 1M live orders), plus evidence
// that nothing allocates after construction. Correctness is proven by
// tests/cpp/phase6_oms_capacity.cpp; this binary is purely a measurement
// harness and always exits 0 unless a structural invariant it depends on
// (every create/find succeeding at the sizes below) breaks.

namespace {

struct MemUsage final {
    std::uint64_t rss_bytes{0};
    std::uint64_t peak_rss_bytes{0};
};

[[nodiscard]] MemUsage mem_usage() noexcept {
    MemUsage usage{};
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        usage.rss_bytes = counters.WorkingSetSize;
        usage.peak_rss_bytes = counters.PeakWorkingSetSize;
    }
#endif
    return usage;
}

[[nodiscard]] std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

risk::Approval approval() {
    static risk::RiskEngine e(risk::Limits{});
    risk::Request q{};
    q.volume = 1; q.risk_minor = 1; q.free_margin = 1'000'000; q.warm_mask = 1; q.session_open = true;
    return e.check(q).token;
}

void print_row(const char* op, const telemetry::Histogram<>& h) {
    std::cout << "  " << op << " n=" << h.count() << " p50=" << h.percentile(0.50)
              << "ns p95=" << h.percentile(0.95) << "ns p99=" << h.percentile(0.99)
              << "ns p999=" << h.percentile(0.999) << "ns max=" << h.max() << "ns\n";
}

bool run_occupancy(std::size_t n) {
    const auto token = approval();
    const auto before = mem_usage();

    telemetry::Histogram<> create_hist, find_hist, transition_hist, cancel_hist;

    const auto ctor_start = now_ns();
    oms::Oms o(n);
    const auto ctor_ns = now_ns() - ctor_start;
    const auto after_ctor = mem_usage();

    // ---- create: fill to 100% occupancy ------------------------------------
    const auto create_start = now_ns();
    bool ok = true;
    for (std::size_t i = 1; i <= n; ++i) {
        const auto op_start = now_ns();
        auto* order = o.create(exec::BrokerOrderRef{1, static_cast<std::uint64_t>(i)},
                               static_cast<std::uint32_t>(i % 8), 1, token);
        create_hist.record(now_ns() - op_start);
        if (order == nullptr) { ok = false; break; }
    }
    const auto create_wall_ns = now_ns() - create_start;
    if (!ok || o.size() != n) {
        std::cerr << "FAIL occupancy=" << n << " create did not reach full occupancy\n";
        return false;
    }
    const auto after_fill = mem_usage();

    // ---- find: scattered access pattern, not sequential --------------------
    // (i * odd constant) mod n scatters access across the full range without
    // needing an O(n) shuffle buffer -- deterministic, reproducible, and its
    // own memory footprint is O(1).
    constexpr std::uint64_t stride = 2654435761ULL;  // Knuth multiplicative hash constant
    const std::size_t find_samples = std::min<std::size_t>(n, 200'000);
    for (std::size_t k = 0; k < find_samples; ++k) {
        const auto id = ((static_cast<std::uint64_t>(k) * stride) % n) + 1;
        const auto op_start = now_ns();
        const auto* order = o.find(exec::BrokerOrderRef{1, id});
        find_hist.record(now_ns() - op_start);
        if (order == nullptr) { ok = false; break; }
    }
    if (!ok) { std::cerr << "FAIL occupancy=" << n << " find() missed a live order\n"; return false; }

    // ---- transition: legal non-terminal transition on a scattered sample ---
    const std::size_t transition_samples = std::min<std::size_t>(n, 200'000);
    for (std::size_t k = 0; k < transition_samples; ++k) {
        const auto id = ((static_cast<std::uint64_t>(k) * stride) % n) + 1;
        const auto op_start = now_ns();
        const auto moved = o.transition(exec::BrokerOrderRef{1, id}, exec::OrderState::sent);
        transition_hist.record(now_ns() - op_start);
        if (!moved) { ok = false; break; }
    }
    if (!ok) { std::cerr << "FAIL occupancy=" << n << " transition() failed on a live order\n"; return false; }

    // ---- cancel (terminal transition -> reclaim) on a scattered sample -----
    // Every order transitioned above is now `sent`; cancel-equivalent here is
    // sent -> rejected, a legal terminal transition that exercises reclaim().
    const std::size_t cancel_samples = std::min<std::size_t>(transition_samples, 100'000);
    std::size_t reclaimed = 0;
    for (std::size_t k = 0; k < cancel_samples; ++k) {
        const auto id = ((static_cast<std::uint64_t>(k) * stride) % n) + 1;
        const auto op_start = now_ns();
        const auto moved = o.transition(exec::BrokerOrderRef{1, id}, exec::OrderState::rejected);
        cancel_hist.record(now_ns() - op_start);
        if (moved) ++reclaimed;
    }
    const auto after_ops = mem_usage();

    std::cout << "occupancy=" << n << "\n";
    std::cout << "  ctor_ns=" << ctor_ns << " create_wall_ns=" << create_wall_ns
              << " create_throughput_ops_per_s="
              << (create_wall_ns ? (static_cast<double>(n) * 1e9 / static_cast<double>(create_wall_ns)) : 0.0)
              << "\n";
    print_row("create    ", create_hist);
    print_row("find      ", find_hist);
    print_row("transition", transition_hist);
    print_row("cancel    ", cancel_hist);
    std::cout << "  reclaimed=" << reclaimed << "/" << cancel_samples
              << " size_after_cancel=" << o.size() << "\n";
    std::cout << "  RSS_MiB before=" << static_cast<double>(before.rss_bytes) / 1048576.0
              << " after_ctor=" << static_cast<double>(after_ctor.rss_bytes) / 1048576.0
              << " after_fill=" << static_cast<double>(after_fill.rss_bytes) / 1048576.0
              << " after_ops=" << static_cast<double>(after_ops.rss_bytes) / 1048576.0
              << " peak=" << static_cast<double>(after_ops.peak_rss_bytes) / 1048576.0 << "\n";
    std::cout << "  RSS_growth_after_fill_MiB="
              << static_cast<double>(after_ops.rss_bytes >= after_fill.rss_bytes
                                          ? after_ops.rss_bytes - after_fill.rss_bytes
                                          : 0)
                     / 1048576.0
              << "  (0 or near-0 expected: find/transition/cancel perform no allocation)\n";
    return true;
}

}  // namespace

int main() {
    std::cout << "OMS capacity/storage benchmark (Phase B). Single allocation at construction "
                 "(orders_ + index_), zero allocation thereafter -- verified structurally in "
                 "oms.hpp (the only std::make_unique calls are in Oms's constructor and copy "
                 "assignment) and empirically below via flat RSS across find/transition/cancel.\n\n";

    bool all_ok = true;
    for (const std::size_t n : {std::size_t{1}, std::size_t{1'000}, std::size_t{100'000},
                                std::size_t{500'000}, std::size_t{1'000'000}}) {
        if (!run_occupancy(n)) all_ok = false;
        std::cout << "\n";
    }
    return all_ok ? 0 : 1;
}
