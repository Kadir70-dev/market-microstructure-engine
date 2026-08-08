#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <vector>

#include "oms/multi_venue.hpp"
#include "oms/strategy_scheduler.hpp"
#include "telemetry/histogram.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

// Phase H benchmark: 2/4/8 equal-weight strategies x 2 venues, at 1K/100K/1M
// aggregate requests. Venues are configured with an unlimited rate limiter
// (Phase G's own overhead is already benchmarked separately in
// rate_limiter_bench) so these numbers isolate arbitration's own cost:
// arbitration p50/p95/p99/p99.9, admitted-per-strategy, fairness deviation
// from the ideal equal split, queue depth/high-water, queue wait latency
// (bulk-enqueue then drain, timing each request's enqueue-to-dispatch gap),
// throughput, RSS, CPU. Correctness (weighted fairness, no starvation,
// determinism) is proven separately by tests/cpp/phase7_strategy_scheduler.cpp;
// this binary exits nonzero only if a required step unexpectedly fails.

namespace {

constexpr oms::VenueId VENUE_A = 1;
constexpr oms::VenueId VENUE_B = 2;

struct MemUsage final { std::uint64_t rss_bytes{0}, peak_rss_bytes{0}; };
struct CpuUsage final { double cpu_seconds{0.0}; };

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

[[nodiscard]] CpuUsage cpu_usage() noexcept {
    CpuUsage usage{};
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const auto to_seconds = [](const FILETIME& ft) {
            ULARGE_INTEGER v{}; v.LowPart = ft.dwLowDateTime; v.HighPart = ft.dwHighDateTime;
            return static_cast<double>(v.QuadPart) / 1e7;
        };
        usage.cpu_seconds = to_seconds(kernel) + to_seconds(user);
    }
#endif
    return usage;
}

[[nodiscard]] std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void print_row(const char* op, const telemetry::Histogram<>& h) {
    std::cout << "    " << op << " n=" << h.count() << " p50=" << h.percentile(0.50)
              << "ns p95=" << h.percentile(0.95) << "ns p99=" << h.percentile(0.99)
              << "ns p999=" << h.percentile(0.999) << "ns max=" << h.max() << "ns\n";
}

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

std::unique_ptr<oms::ShardedOms> make_venue_oms(std::size_t cap) {
    constexpr std::size_t shard_count = 4;
    return std::make_unique<oms::ShardedOms>(shard_count, cap / shard_count + 64, 1, cap + 64);
}

bool run_combo(std::size_t num_strategies, std::size_t aggregate_n) {
    const std::size_t per_strategy = aggregate_n / num_strategies;
    const auto mem_before_init = mem_usage();
    const auto cpu_before = cpu_usage();

    oms::VenueRegistry registry;
    registry.add(VENUE_A, make_venue_oms(aggregate_n), std::make_unique<FakeVenue>());
    registry.add(VENUE_B, make_venue_oms(aggregate_n), std::make_unique<FakeVenue>());
    registry.find(VENUE_A)->configure_rate_limit(oms::VenueRateLimitConfig{}, 0);
    registry.find(VENUE_B)->configure_rate_limit(oms::VenueRateLimitConfig{}, 0);

    oms::StrategyScheduler sched(num_strategies, per_strategy + 16);
    for (oms::StrategyId s = 1; s <= num_strategies; ++s) {
        if (!sched.register_strategy(s, {1})) { std::cerr << "FAIL register strategy " << s << "\n"; return false; }
    }

    const auto mem_after_init = mem_usage();

    // Bulk-enqueue phase: builds up real per-strategy queue depth/high-water,
    // and records each request's enqueue instant for queue-wait measurement.
    std::unordered_map<std::uint64_t, std::uint64_t> enqueue_time;
    enqueue_time.reserve(aggregate_n);
    for (oms::StrategyId s = 1; s <= num_strategies; ++s) {
        for (std::size_t i = 0; i < per_strategy; ++i) {
            const auto venue = (i % 2 == 0) ? VENUE_A : VENUE_B;
            const std::uint64_t logical_id = s * 10'000'000ULL + i + 1;
            oms::PendingRequest req{};
            req.kind = oms::RateLimitKind::order;
            req.ref = exec::BrokerOrderRef{venue, logical_id};
            req.symbol = static_cast<std::uint32_t>(i % exec::max_symbols);
            req.volume = 1;
            if (!sched.enqueue(s, venue, exec::Side::buy, req)) {
                std::cerr << "FAIL enqueue strategy=" << s << " i=" << i << "\n";
                return false;
            }
            enqueue_time[logical_id] = now_ns();
        }
    }

    std::size_t high_water = 0;
    for (oms::StrategyId s = 1; s <= num_strategies; ++s) high_water = std::max(high_water, sched.queue_high_water(s));

    // Drain phase: dispatch every enqueued request, timing arbitration
    // (next()) and end-to-end routing separately, plus per-request wait.
    telemetry::Histogram<> arbitration_hist, wait_hist;
    std::uint64_t admitted = 0, rejected = 0;
    const auto drain_start = now_ns();
    for (std::size_t i = 0; i < aggregate_n; ++i) {
        oms::ArbitratedRequest req{};
        const auto arb_start = now_ns();
        const bool had = sched.next(req);
        arbitration_hist.record(now_ns() - arb_start);
        if (!had) break;

        const auto it = enqueue_time.find(req.request.ref.logical_order_id);
        if (it != enqueue_time.end()) wait_hist.record(now_ns() - it->second);

        auto* venue = registry.find(req.venue);
        oms::Completion c{};
        const auto decision = venue->route_create(0, req.request.ref, req.request.symbol, req.request.volume,
                                                   0, c);
        if (decision == oms::Decision::admitted) ++admitted; else ++rejected;
    }
    const auto drain_ns = now_ns() - drain_start;
    const double throughput = drain_ns ? static_cast<double>(aggregate_n) * 1e9 / static_cast<double>(drain_ns) : 0.0;

    const auto mem_after = mem_usage();
    const auto cpu_after = cpu_usage();

    // Fairness deviation: max absolute deviation from the ideal equal split
    // (per_strategy each, for equal-weight strategies), as a fraction.
    std::uint64_t max_admitted = 0, min_admitted = UINT64_MAX;
    for (oms::StrategyId s = 1; s <= num_strategies; ++s) {
        const auto c = sched.admitted_count(s);
        max_admitted = std::max(max_admitted, c);
        min_admitted = std::min(min_admitted, c);
    }
    const double fairness_deviation =
        per_strategy > 0 ? static_cast<double>(max_admitted - min_admitted) / static_cast<double>(per_strategy) : 0.0;

    std::cout << "strategies=" << num_strategies << " aggregate=" << aggregate_n << " (per_strategy=" << per_strategy
              << ", 2 venues)\n";
    print_row("arbitration(next)", arbitration_hist);
    print_row("queue_wait", wait_hist);
    std::cout << "  admitted=" << admitted << " rejected=" << rejected << " throughput=" << throughput
              << " req/sec (" << (static_cast<double>(drain_ns) / 1e6) << " ms)\n";
    std::cout << "  fairness_deviation=" << fairness_deviation
              << " (0 = perfect equal split; min_admitted=" << min_admitted << " max_admitted=" << max_admitted
              << " ideal=" << per_strategy << ")\n";
    std::cout << "  queue_high_water=" << high_water << "\n";
    std::cout << "  RSS_MiB before_init=" << static_cast<double>(mem_before_init.rss_bytes) / 1048576.0
              << " after_init=" << static_cast<double>(mem_after_init.rss_bytes) / 1048576.0
              << " after_workload=" << static_cast<double>(mem_after.rss_bytes) / 1048576.0
              << " peak=" << static_cast<double>(mem_after.peak_rss_bytes) / 1048576.0 << "\n";
    std::cout << "  RSS_growth_after_init_MiB="
              << (static_cast<double>(mem_after_init.rss_bytes) - static_cast<double>(mem_before_init.rss_bytes)) /
                     1048576.0
              << "  (StrategyScheduler preallocates every slot's queue at construction; enqueue()/next() never "
                 "allocate)\n";
    std::cout << "  cpu_seconds=" << (cpu_after.cpu_seconds - cpu_before.cpu_seconds) << "\n";

    if (fairness_deviation > 0.05) {
        std::cerr << "FAIL fairness deviation too large: " << fairness_deviation << "\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    std::cout << "Strategy scheduler benchmark (Phase H). 2 venues.\n\n";
    bool all_ok = true;
    for (const std::size_t strategies : {std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        for (const std::size_t n : {std::size_t{1'000}, std::size_t{100'000}, std::size_t{1'000'000}}) {
            if (!run_combo(strategies, n)) all_ok = false;
            std::cout << "\n";
        }
    }
    return all_ok ? 0 : 1;
}
