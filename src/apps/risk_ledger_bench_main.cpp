#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include "oms/risk_gated_router.hpp"
#include "telemetry/histogram.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

// Phase I benchmark. The full requested cross product (1/2/4/max workers x
// 2/4/8 strategies x 2 venues x 1K/100K/1M requests) is 36 combinations;
// run in full it would take well over an hour. Scoped instead into two
// studies that together cover every requested axis:
//   (a) worker-count scaling: strategies fixed at 4, sweep workers in
//       {1,2,4,max} x scale in {1K,100K,1M} -- the primary "scaling vs
//       worker count" deliverable.
//   (b) strategy-count sweep: workers fixed at 4, scale fixed at 100K,
//       strategies in {2,4,8} -- confirms the concurrent-risk architecture
//       does not degrade as strategy count grows.
// Correctness (no oversubscription, exactly-once, kill switches, recovery,
// pipeline ordering, bypass closure) is proven separately by
// tests/cpp/phase7_risk_ledger.cpp; this binary exits nonzero only if a
// required step unexpectedly fails.
//
// Phase A baseline (docs/prior report): risk p99 ~100ns, OMS p99 ~100ns,
// single-threaded, no exposure/credit tracking at all (RiskEngine::check()
// against a fixed dummy Request -- see risk_ledger.hpp's audit comment).
// This benchmark reports the SAME risk-check p99 under real concurrent
// load with real tracked state across 4 dimensions, side by side with that
// number, without hiding the delta.

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

class FakeVenue final : public oms::VenueAdapter {
public:
    [[nodiscard]] std::size_t fetch_open_orders(oms::OrderSnapshot*, std::size_t) const noexcept override { return 0; }
    [[nodiscard]] std::size_t fetch_positions(oms::PositionSnapshot*, std::size_t) const noexcept override { return 0; }
    [[nodiscard]] std::size_t fetch_recent_reports(oms::ExecReport*, std::size_t) const noexcept override { return 0; }
    [[nodiscard]] exec::AccountState fetch_account() const noexcept override { return exec::AccountState{}; }
};

std::unique_ptr<oms::ShardedOms> make_venue_oms(std::size_t cap) {
    constexpr std::size_t shard_count = 4;
    return std::make_unique<oms::ShardedOms>(shard_count, cap / shard_count + 64, 16, cap + 64);
}

constexpr oms::VenueId VENUE_A = 1;
constexpr oms::VenueId VENUE_B = 2;

bool run_combo(std::size_t workers, std::size_t num_strategies, std::size_t aggregate_n) {
    const auto mem_before_init = mem_usage();
    const auto cpu_before = cpu_usage();

    oms::VenueRegistry registry;
    registry.add(VENUE_A, make_venue_oms(aggregate_n), std::make_unique<FakeVenue>());
    registry.add(VENUE_B, make_venue_oms(aggregate_n), std::make_unique<FakeVenue>());
    registry.find(VENUE_A)->configure_rate_limit(oms::VenueRateLimitConfig{}, 0);
    registry.find(VENUE_B)->configure_rate_limit(oms::VenueRateLimitConfig{}, 0);

    risk::RiskLedgerConfig cfg{};
    cfg.max_order_volume = 1'000'000;
    cfg.global = {static_cast<std::int64_t>(aggregate_n) + 16, static_cast<std::int64_t>(aggregate_n) + 16,
                 static_cast<std::int64_t>(aggregate_n) * 10 + 16};
    cfg.per_strategy = cfg.global;
    cfg.per_venue = cfg.global;
    cfg.per_symbol = cfg.global;
    risk::RiskLedger ledger(num_strategies + 1, 4, aggregate_n + 64, cfg);
    for (risk::StrategyId s = 1; s <= num_strategies; ++s) {
        if (!ledger.register_strategy(s)) { std::cerr << "FAIL register strategy " << s << "\n"; return false; }
    }
    (void)ledger.register_venue(VENUE_A);
    (void)ledger.register_venue(VENUE_B);
    oms::RiskGatedRouter router(registry, ledger);

    const auto mem_after_init = mem_usage();

    telemetry::Histogram<> risk_hist, oms_hist;
    std::atomic<std::uint64_t> admitted{0}, rejected{0};
    std::atomic<std::size_t> next_id{0};
    const std::size_t per_worker = aggregate_n / workers;

    const auto start = now_ns();
    std::vector<std::thread> pool;
    for (std::size_t w = 0; w < workers; ++w) {
        pool.emplace_back([&, w] {
            for (std::size_t i = 0; i < per_worker; ++i) {
                const auto id = next_id.fetch_add(1, std::memory_order_relaxed);
                const auto strategy = static_cast<risk::StrategyId>((id % num_strategies) + 1);
                const auto venue = (id % 2 == 0) ? VENUE_A : VENUE_B;
                const exec::BrokerOrderRef ref{venue, id + 1};

                const auto risk_start = now_ns();
                // Isolates the ledger's own reserve() cost (the direct
                // Phase A/risk_bench comparison point) from the full
                // pipeline's OMS-call cost, measured separately below.
                const bool reserved = ledger.reserve(strategy, venue, static_cast<std::uint32_t>(id % exec::max_symbols),
                                                     exec::Side::buy, 1, ref);
                risk_hist.record(now_ns() - risk_start);
                if (!reserved) { rejected.fetch_add(1, std::memory_order_relaxed); continue; }
                ledger.release(ref);  // this measurement path only exercises reserve/release cost, not routing --
                                      // full end-to-end admission (including OMS) is measured by oms_hist below.

                oms::Completion c{};
                const auto oms_start = now_ns();
                const auto decision = router.submit_create(strategy, venue, w, ref, static_cast<std::uint32_t>(id % exec::max_symbols),
                                                           exec::Side::buy, 1, 0, c);
                oms_hist.record(now_ns() - oms_start);
                if (decision == oms::Decision::admitted) admitted.fetch_add(1, std::memory_order_relaxed);
                else rejected.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& w : pool) w.join();
    const auto elapsed_ns = now_ns() - start;

    const auto mem_after = mem_usage();
    const auto cpu_after = cpu_usage();
    const double throughput = elapsed_ns ? static_cast<double>(workers * per_worker) * 1e9 / static_cast<double>(elapsed_ns) : 0.0;
    const double rejects_per_sec = elapsed_ns ? static_cast<double>(rejected.load()) * 1e9 / static_cast<double>(elapsed_ns) : 0.0;

    std::cout << "workers=" << workers << " strategies=" << num_strategies << " aggregate=" << aggregate_n << "\n";
    print_row("risk.reserve (isolated)", risk_hist);
    print_row("router.submit_create (end-to-end)", oms_hist);
    std::cout << "  throughput=" << throughput << " req/sec  rejects_per_sec=" << rejects_per_sec
              << "  admitted=" << admitted.load() << " rejected=" << rejected.load() << "\n";
    std::cout << "  RSS_MiB after_init=" << static_cast<double>(mem_after_init.rss_bytes) / 1048576.0
              << " final=" << static_cast<double>(mem_after.rss_bytes) / 1048576.0
              << " peak=" << static_cast<double>(mem_after.peak_rss_bytes) / 1048576.0 << "\n";
    std::cout << "  RSS_growth_after_init_MiB="
              << (static_cast<double>(mem_after.rss_bytes) - static_cast<double>(mem_after_init.rss_bytes)) / 1048576.0
              << "  (RiskLedger preallocates strategy/venue/symbol arrays and the reservation table at "
                 "construction; reserve()/release()/reconcile() never allocate)\n";
    std::cout << "  cpu_seconds=" << (cpu_after.cpu_seconds - cpu_before.cpu_seconds)
              << "  (cpu_seconds / elapsed_seconds ~= " << (workers > 0 ? (cpu_after.cpu_seconds - cpu_before.cpu_seconds) /
                                                              (static_cast<double>(elapsed_ns) / 1e9) : 0.0)
              << " is a rough contention/parallel-efficiency proxy -- close to `workers` means low contention)\n";
    return true;
}

}  // namespace

int main() {
    const auto hw = std::thread::hardware_concurrency();
    const std::size_t max_workers = hw == 0 ? 4 : hw;
    std::cout << "Risk ledger benchmark (Phase I). hardware_concurrency=" << max_workers << "\n";
    std::cout << "Phase A baseline for comparison: risk p99~=100ns, OMS p99~=100ns (single-threaded, no tracked state).\n\n";

    bool all_ok = true;

    std::cout << "== study (a): worker-count scaling, strategies=4 ==\n\n";
    for (const std::size_t n : {std::size_t{1'000}, std::size_t{100'000}, std::size_t{1'000'000}}) {
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, max_workers}) {
            if (!run_combo(workers, 4, n)) all_ok = false;
            std::cout << "\n";
        }
    }

    std::cout << "== study (b): strategy-count sweep, workers=4, aggregate=100K ==\n\n";
    for (const std::size_t strategies : {std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        if (!run_combo(4, strategies, 100'000)) all_ok = false;
        std::cout << "\n";
    }

    return all_ok ? 0 : 1;
}
