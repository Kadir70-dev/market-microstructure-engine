#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <vector>

#include "oms/multi_venue.hpp"
#include "telemetry/histogram.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

// Phase G benchmark: 2 venues at 1K/100K/1M aggregate orders. Two distinct
// measurements per scale, since they need opposite limiter configurations to
// mean anything:
//   (a) routing latency before/after the limiter -- both passes use a
//       generous (~unlimited) limiter config, so the comparison isolates the
//       overhead route_create() adds over raw ShardedOms::submit_create()
//       (session + connection-state + token-bucket-consume), not rate-limit-
//       induced deferral, which would otherwise dominate and hide it.
//   (b) admitted/deferred/rejected/sec, queue depth/high-water, queue wait
//       latency -- needs a limiter tight enough to actually saturate, so
//       this uses a separate, deliberately small sub-workload (capped, not
//       the full N) and a burst-then-drain shape: flood requests faster
//       than the bucket refills (producing real deferrals up to queue
//       capacity, then real queue-full rejections), then drain with
//       advancing synthetic time and measure actual wait per drained entry.
// Correctness is proven separately by tests/cpp/phase7_rate_limiting.cpp;
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

std::unique_ptr<oms::ShardedOms> make_venue_oms(std::size_t per_venue) {
    constexpr std::size_t shard_count = 4;
    return std::make_unique<oms::ShardedOms>(shard_count, per_venue / shard_count + 64, 1, per_venue + 64);
}

bool run_scale(std::size_t aggregate_n) {
    const std::size_t per_venue = aggregate_n / 2;

    // ---- (a) routing latency before/after the limiter, generous config -----
    telemetry::Histogram<> before_hist, after_hist;
    {
        auto oms_before = make_venue_oms(per_venue);
        for (std::size_t i = 0; i < per_venue; ++i) {
            oms::Completion c{};
            const auto start = now_ns();
            (void)oms_before->submit_create(0, exec::BrokerOrderRef{VENUE_A, i + 1},
                                            static_cast<std::uint32_t>(i % exec::max_symbols), 10, c);
            before_hist.record(now_ns() - start);
        }
    }
    {
        oms::VenueConnection va(VENUE_A, make_venue_oms(per_venue), std::make_unique<FakeVenue>());
        va.configure_rate_limit(oms::VenueRateLimitConfig{}, 0);  // ~unlimited: isolates route_create()'s own overhead
        for (std::size_t i = 0; i < per_venue; ++i) {
            oms::Completion c{};
            const auto start = now_ns();
            (void)va.route_create(0, exec::BrokerOrderRef{VENUE_A, i + 1},
                                  static_cast<std::uint32_t>(i % exec::max_symbols), 10, 0, c);
            after_hist.record(now_ns() - start);
        }
    }

    // ---- (b) admission mix + queue depth/high-water + wait latency ----------
    const std::size_t queue_n = std::min<std::size_t>(per_venue, 20'000);
    const std::size_t flood_n = queue_n * 3;  // deliberately overruns the queue, to exercise queue-full too
    const auto mem_before_init = mem_usage();
    const auto cpu_before = cpu_usage();

    oms::VenueRegistry registry;
    registry.add(VENUE_A, make_venue_oms(flood_n), std::make_unique<FakeVenue>());
    registry.add(VENUE_B, make_venue_oms(flood_n), std::make_unique<FakeVenue>());
    oms::VenueRateLimitConfig tight{};
    tight.orders_per_sec = 10'000.0;
    tight.burst_capacity = 200.0;
    tight.policy = oms::QueuePolicy::defer;
    tight.pending_queue_capacity = queue_n;
    registry.find(VENUE_A)->configure_rate_limit(tight, 0);
    registry.find(VENUE_B)->configure_rate_limit(tight, 0);

    const auto mem_after_init = mem_usage();

    telemetry::Histogram<> decision_hist, wait_hist;
    std::uint64_t admitted = 0, deferred = 0, rejected = 0;
    std::size_t high_water = 0;

    auto flood_venue = [&](oms::VenueId venue) {
        auto* conn = registry.find(venue);
        std::unordered_map<std::uint64_t, std::uint64_t> defer_time;  // logical_order_id -> defer instant
        defer_time.reserve(flood_n);
        std::uint64_t clock = 0;
        for (std::size_t i = 0; i < flood_n; ++i) {
            oms::Completion c{};
            const auto decision_start = now_ns();
            const auto decision = conn->route_create(0, exec::BrokerOrderRef{venue, i + 1}, 0, 1, clock, c);
            decision_hist.record(now_ns() - decision_start);
            clock += 100;  // synthetic: 100ns between submissions, far faster than the bucket refills
            switch (decision) {
                case oms::Decision::admitted: ++admitted; break;
                case oms::Decision::deferred: ++deferred; defer_time[i + 1] = clock; break;
                case oms::Decision::rejected: ++rejected; break;
            }
        }
        // Drain phase: stop submitting, advance synthetic time in coarse
        // steps, and drain whatever the refill rate now allows, timing each
        // drained entry's actual queue wait.
        for (int step = 0; step < 20'000 && conn->rate_limiter().pending_depth() > 0; ++step) {
            clock += 1'000'000;  // 1ms/step
            oms::PendingRequest out{};
            while (conn->rate_limiter().try_drain_one(clock, out)) {
                oms::Completion c{};
                (void)conn->oms().submit_create(0, out.ref, out.symbol, out.volume, c);
                const auto it = defer_time.find(out.ref.logical_order_id);
                if (it != defer_time.end()) wait_hist.record(clock - it->second);
            }
        }
    };

    std::thread ta(flood_venue, VENUE_A);
    std::thread tb(flood_venue, VENUE_B);
    ta.join();
    tb.join();
    high_water = std::max(registry.find(VENUE_A)->rate_limiter().pending_high_water(),
                          registry.find(VENUE_B)->rate_limiter().pending_high_water());

    const auto mem_after = mem_usage();
    const auto cpu_after = cpu_usage();
    const double total_decisions = static_cast<double>(admitted + deferred + rejected);
    const double window_s = static_cast<double>(flood_n * 2) * 100e-9;  // synthetic clock, both venues
    const double admitted_per_sec = window_s > 0 ? static_cast<double>(admitted) / window_s : 0.0;
    const double deferred_per_sec = window_s > 0 ? static_cast<double>(deferred) / window_s : 0.0;
    const double rejected_per_sec = window_s > 0 ? static_cast<double>(rejected) / window_s : 0.0;

    std::cout << "aggregate_orders=" << aggregate_n << " (per_venue=" << per_venue << ", queue_exercise_n="
              << queue_n << " x2 venues)\n";
    std::cout << "  routing_before_limiter:\n"; print_row("create(raw)", before_hist);
    std::cout << "  routing_after_limiter:\n"; print_row("route_create", after_hist);
    print_row("limiter_decision", decision_hist);
    print_row("queue_wait", wait_hist);
    std::cout << "  admitted=" << admitted << " deferred=" << deferred << " rejected=" << rejected
              << " (of " << total_decisions << " decisions, both venues)\n";
    std::cout << "  admitted_per_sec=" << admitted_per_sec << " deferred_per_sec=" << deferred_per_sec
              << " rejected_per_sec=" << rejected_per_sec << " (synthetic-clock window, illustrative)\n";
    std::cout << "  queue_high_water=" << high_water << " (capacity=" << queue_n << ")\n";
    std::cout << "  RSS_MiB before_init=" << static_cast<double>(mem_before_init.rss_bytes) / 1048576.0
              << " after_init=" << static_cast<double>(mem_after_init.rss_bytes) / 1048576.0
              << " after_workload=" << static_cast<double>(mem_after.rss_bytes) / 1048576.0
              << " peak=" << static_cast<double>(mem_after.peak_rss_bytes) / 1048576.0 << "\n";
    std::cout << "  RSS_growth_after_init_MiB="
              << (static_cast<double>(mem_after.rss_bytes) - static_cast<double>(mem_after_init.rss_bytes)) / 1048576.0
              << "  (VenueRateLimiter preallocates its pending queue at configure_rate_limit() time; "
                 "admit()/try_drain_one() never allocate -- growth here is this benchmark's own "
                 "defer_time map/histograms, not the limiter)\n";
    std::cout << "  cpu_seconds=" << (cpu_after.cpu_seconds - cpu_before.cpu_seconds) << "\n";

    return true;
}

}  // namespace

int main() {
    std::cout << "Rate limiter benchmark (Phase G). 2 venues.\n\n";
    bool all_ok = true;
    for (const std::size_t n : {std::size_t{1'000}, std::size_t{100'000}, std::size_t{1'000'000}}) {
        if (!run_scale(n)) all_ok = false;
        std::cout << "\n";
    }
    return all_ok ? 0 : 1;
}
