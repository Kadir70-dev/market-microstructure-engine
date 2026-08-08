#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include "oms/sharded_oms.hpp"
#include "telemetry/histogram.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

// Phase C benchmark: throughput/latency scaling by worker-thread count
// (1/2/4) at each load level (1K/100K/500K/1M live orders), plus queue
// depth, CPU utilization, RSS, and a direct comparison against the Phase A
// (single-order, single-thread) and Phase B (direct-call, single-thread,
// occupancy-scaled) baselines. Correctness is proven separately by
// tests/cpp/phase6_oms_concurrent.cpp; this binary always exits 0 unless a
// create/find/transition unexpectedly fails.

namespace {

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

bool run_combo(std::size_t threads, std::size_t load) {
    const auto shard_count = threads;
    const auto cap_per_shard = load / shard_count + 16;
    const auto log_cap_per_shard = load / shard_count + 16;

    const auto cpu_before = cpu_usage();
    const auto mem_before = mem_usage();

    const auto ctor_start = now_ns();
    oms::ShardedOms s(shard_count, cap_per_shard, threads, log_cap_per_shard);
    const auto ctor_ns = now_ns() - ctor_start;
    const auto mem_after_ctor = mem_usage();

    std::atomic<bool> ok{true};
    std::vector<telemetry::Histogram<>> create_hist(threads), find_hist(threads), transition_hist(threads);

    const auto per_thread = load / threads;

    // ---- create: full load, split evenly across producer threads ----------
    // A monitor thread samples every (producer, shard) ring's instantaneous
    // depth throughout the create phase -- this is what "queue depth under
    // load" and "contention" mean empirically: how full the rings actually
    // get while threads are genuinely racing to submit.
    std::atomic<bool> monitor_stop{false};
    std::atomic<std::size_t> max_depth_observed{0};
    std::atomic<std::uint64_t> depth_samples{0}, depth_sum{0};
    std::thread monitor([&] {
        while (!monitor_stop.load(std::memory_order_relaxed)) {
            for (std::size_t sh = 0; sh < shard_count; ++sh) {
                for (std::size_t p = 0; p < threads; ++p) {
                    const auto d = s.queue_depth(sh, p);
                    depth_samples.fetch_add(1, std::memory_order_relaxed);
                    depth_sum.fetch_add(d, std::memory_order_relaxed);
                    auto cur = max_depth_observed.load(std::memory_order_relaxed);
                    while (d > cur && !max_depth_observed.compare_exchange_weak(cur, d, std::memory_order_relaxed)) {}
                }
                // This loop had no backoff at all in an earlier version: on a
                // 4-core box, threads=4 already runs 4 producers + 4 shard
                // workers, so an additional hard-spinning monitor thread is a
                // 9th thread fighting for 4 cores. That produced a reproduced,
                // severe (tens-of-seconds) stall at threads=4/load=500000 --
                // not a livelock in ShardedOms itself (identical logic at
                // load=1,000,000 ran in 5s), but real, measured evidence that
                // an unthrottled monitor thread can starve the very workers
                // it is trying to observe. Sleeping briefly between sweeps
                // costs sampling resolution (still thousands of samples/sec)
                // in exchange for not perturbing what is being measured.
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    });

    const auto create_wall_start = now_ns();
    {
        std::vector<std::thread> workers;
        for (std::size_t w = 0; w < threads; ++w) {
            workers.emplace_back([&, w] {
                for (std::size_t i = 0; i < per_thread; ++i) {
                    const std::uint64_t id = w * per_thread + i + 1;
                    const auto op_start = now_ns();
                    oms::Completion c{};
                    const bool accepted = s.submit_create(w, exec::BrokerOrderRef{1, id}, 0, 1, c);
                    create_hist[w].record(now_ns() - op_start);
                    if (!accepted || !c.ok) ok.store(false, std::memory_order_relaxed);
                }
            });
        }
        for (auto& w : workers) w.join();
    }
    const auto create_wall_ns = now_ns() - create_wall_start;
    monitor_stop.store(true, std::memory_order_relaxed);
    monitor.join();
    const auto mem_after_fill = mem_usage();
    if (!ok.load()) { std::cerr << "FAIL threads=" << threads << " load=" << load << " create\n"; return false; }

    // ---- find: scattered, capped sample per thread -------------------------
    const std::size_t find_samples_per_thread = std::min<std::size_t>(per_thread, 50'000);
    {
        std::vector<std::thread> workers;
        for (std::size_t w = 0; w < threads; ++w) {
            workers.emplace_back([&, w] {
                constexpr std::uint64_t stride = 2654435761ULL;
                for (std::size_t k = 0; k < find_samples_per_thread; ++k) {
                    const auto id = w * per_thread + ((k * stride) % per_thread) + 1;
                    const auto op_start = now_ns();
                    exec::Order out{};
                    const bool found = s.find_copy(w, exec::BrokerOrderRef{1, id}, out);
                    find_hist[w].record(now_ns() - op_start);
                    if (!found) ok.store(false, std::memory_order_relaxed);
                }
            });
        }
        for (auto& w : workers) w.join();
    }
    if (!ok.load()) { std::cerr << "FAIL threads=" << threads << " load=" << load << " find\n"; return false; }

    // ---- transition: legal pending_send -> sent, scattered, capped ---------
    const std::size_t transition_samples_per_thread = std::min<std::size_t>(per_thread, 50'000);
    {
        std::vector<std::thread> workers;
        for (std::size_t w = 0; w < threads; ++w) {
            workers.emplace_back([&, w] {
                constexpr std::uint64_t stride = 2654435761ULL;
                for (std::size_t k = 0; k < transition_samples_per_thread; ++k) {
                    const auto id = w * per_thread + ((k * stride) % per_thread) + 1;
                    const auto op_start = now_ns();
                    oms::Completion c{};
                    const bool moved = s.submit_transition(w, exec::BrokerOrderRef{1, id},
                                                           exec::OrderState::sent, c);
                    transition_hist[w].record(now_ns() - op_start);
                    if (!moved || !c.ok) ok.store(false, std::memory_order_relaxed);
                }
            });
        }
        for (auto& w : workers) w.join();
    }
    const auto mem_after_ops = mem_usage();
    const auto cpu_after = cpu_usage();
    if (!ok.load()) { std::cerr << "FAIL threads=" << threads << " load=" << load << " transition\n"; return false; }

    // ---- merge per-thread histograms for a single reported distribution ---
    // Histogram has no merge API; re-deriving combined percentiles by summing
    // exposing internals, so instead report the worst-case (max) p99/p999
    // across threads, which is the more conservative and more relevant
    // number for a shared-resource system anyway (the slowest thread is what
    // an operator actually experiences).
    std::uint64_t create_p50_max = 0, create_p99_max = 0, create_p999_max = 0;
    std::uint64_t find_p50_max = 0, find_p99_max = 0, find_p999_max = 0;
    std::uint64_t trans_p50_max = 0, trans_p99_max = 0, trans_p999_max = 0;
    for (std::size_t w = 0; w < threads; ++w) {
        create_p50_max = std::max(create_p50_max, create_hist[w].percentile(0.50));
        create_p99_max = std::max(create_p99_max, create_hist[w].percentile(0.99));
        create_p999_max = std::max(create_p999_max, create_hist[w].percentile(0.999));
        find_p50_max = std::max(find_p50_max, find_hist[w].percentile(0.50));
        find_p99_max = std::max(find_p99_max, find_hist[w].percentile(0.99));
        find_p999_max = std::max(find_p999_max, find_hist[w].percentile(0.999));
        trans_p50_max = std::max(trans_p50_max, transition_hist[w].percentile(0.50));
        trans_p99_max = std::max(trans_p99_max, transition_hist[w].percentile(0.99));
        trans_p999_max = std::max(trans_p999_max, transition_hist[w].percentile(0.999));
    }

    const double create_throughput = create_wall_ns
        ? static_cast<double>(load) * 1e9 / static_cast<double>(create_wall_ns) : 0.0;
    const double cpu_delta = cpu_after.cpu_seconds - cpu_before.cpu_seconds;
    const double wall_s = static_cast<double>(create_wall_ns) / 1e9;

    std::cout << "threads=" << threads << " load=" << load << "\n";
    std::cout << "  ctor_ns=" << ctor_ns << " create_wall_ns=" << create_wall_ns
              << " create_throughput_ops_per_s=" << create_throughput << "\n";
    std::cout << "  (per-thread-worst-case percentiles -- the slowest thread, not an average)\n";
    std::cout << "    create     p50=" << create_p50_max << "ns p99=" << create_p99_max
              << "ns p999=" << create_p999_max << "ns\n";
    std::cout << "    find       p50=" << find_p50_max << "ns p99=" << find_p99_max
              << "ns p999=" << find_p999_max << "ns\n";
    std::cout << "    transition p50=" << trans_p50_max << "ns p99=" << trans_p99_max
              << "ns p999=" << trans_p999_max << "ns\n";
    std::cout << "  CPU cpu_seconds=" << cpu_delta << " wall_seconds=" << wall_s
              << " cpu_cores_used=" << (wall_s > 0 ? cpu_delta / wall_s : 0.0)
              << " (create phase only)\n";
    {
        const auto samples = depth_samples.load(std::memory_order_relaxed);
        const auto sum = depth_sum.load(std::memory_order_relaxed);
        std::cout << "  QUEUE_DEPTH (create phase, sampled by a monitor thread) max="
                  << max_depth_observed.load(std::memory_order_relaxed)
                  << " mean=" << (samples ? static_cast<double>(sum) / static_cast<double>(samples) : 0.0)
                  << " capacity_per_ring=" << oms::queue_capacity
                  << " samples=" << samples << "\n";
    }
    std::cout << "  RSS_MiB before=" << static_cast<double>(mem_before.rss_bytes) / 1048576.0
              << " after_ctor=" << static_cast<double>(mem_after_ctor.rss_bytes) / 1048576.0
              << " after_fill=" << static_cast<double>(mem_after_fill.rss_bytes) / 1048576.0
              << " after_ops=" << static_cast<double>(mem_after_ops.rss_bytes) / 1048576.0
              << " peak=" << static_cast<double>(mem_after_ops.peak_rss_bytes) / 1048576.0 << "\n";
    std::cout << "  RSS_growth_after_fill_MiB="
              << static_cast<double>(mem_after_ops.rss_bytes >= mem_after_fill.rss_bytes
                                          ? mem_after_ops.rss_bytes - mem_after_fill.rss_bytes : 0) / 1048576.0
              << "\n";
    return true;
}

}  // namespace

int main() {
    std::cout << "OMS concurrent benchmark (Phase C). shard_count == thread_count for every "
                 "combination below (one shard per producer thread; routing is still by order "
                 "key, not by producer identity). Compare against Phase A (risk_p99=100ns, "
                 "oms_p99=100ns, single order, single thread) and Phase B (direct-call, "
                 "single-threaded, e.g. 1M-occupancy create p99=512ns) -- the difference between "
                 "those numbers and the ones below is the measured cost of queue-mediated, "
                 "thread-safe access.\n\n";

    const auto hw = std::thread::hardware_concurrency();
    std::cout << "hardware_concurrency()=" << hw << "\n\n";

    bool all_ok = true;
    for (const std::size_t threads : {std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
        for (const std::size_t load : {std::size_t{1'000}, std::size_t{100'000}, std::size_t{500'000},
                                       std::size_t{1'000'000}}) {
            if (!run_combo(threads, load)) all_ok = false;
            std::cout << "\n";
        }
    }
    return all_ok ? 0 : 1;
}
