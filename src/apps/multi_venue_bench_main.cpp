#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include "oms/multi_venue.hpp"
#include "telemetry/histogram.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

// Phase F benchmark: 2 venues, measured at 1K/100K/1M aggregate orders (split
// evenly per venue). Reports orders/sec, reports/sec, routing latency
// percentiles, a baseline OMS (find) latency, per-venue and aggregate
// reconciliation time, RSS, CPU, and RSS growth after initialization as an
// allocation proxy -- the same structural evidence oms_bench_main.cpp (Phase
// B) and oms_concurrent_bench_main.cpp (Phase C) use, since ShardedOms itself
// does not expose an allocation counter. Correctness is proven separately by
// tests/cpp/phase7_multi_venue.cpp; this binary exits nonzero only if a
// required step unexpectedly fails.

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

// Same fully test-controlled adapter as tests/cpp/phase7_multi_venue.cpp:
// scripted responses, no PaperBroker latency-model noise in a benchmark that
// is measuring multi-venue orchestration overhead, not fill mechanics.
class FakeVenue final : public oms::VenueAdapter {
public:
    std::vector<oms::OrderSnapshot> open_orders;
    [[nodiscard]] std::size_t fetch_open_orders(oms::OrderSnapshot* out,
                                                std::size_t capacity) const noexcept override {
        const auto n = std::min(capacity, open_orders.size());
        for (std::size_t i = 0; i < n; ++i) out[i] = open_orders[i];
        return n;
    }
    [[nodiscard]] std::size_t fetch_positions(oms::PositionSnapshot*, std::size_t) const noexcept override {
        return 0;
    }
    [[nodiscard]] std::size_t fetch_recent_reports(oms::ExecReport*, std::size_t) const noexcept override {
        return 0;  // benchmark applies reports directly via submit_report, not via catch-up replay
    }
    [[nodiscard]] exec::AccountState fetch_account() const noexcept override { return exec::AccountState{}; }
};

bool run_scale(std::size_t aggregate_n) {
    const std::size_t per_venue = aggregate_n / 2;
    constexpr std::size_t shard_count = 4;
    const std::size_t cap_per_shard = per_venue / shard_count + 64;
    const std::size_t log_cap = per_venue + 64;

    const auto mem_before_init = mem_usage();
    const auto cpu_before = cpu_usage();

    oms::VenueRegistry registry;
    auto adapter_a = std::make_unique<FakeVenue>();
    auto adapter_b = std::make_unique<FakeVenue>();
    auto* fake_a = adapter_a.get();
    auto* fake_b = adapter_b.get();
    registry.add(VENUE_A, std::make_unique<oms::ShardedOms>(shard_count, cap_per_shard, 1, log_cap),
                std::move(adapter_a));
    registry.add(VENUE_B, std::make_unique<oms::ShardedOms>(shard_count, cap_per_shard, 1, log_cap),
                std::move(adapter_b));

    const auto mem_after_init = mem_usage();

    // ---- orders/sec + routing latency (submit_create through VenueRegistry) --
    telemetry::Histogram<> routing_hist;
    const auto orders_start = now_ns();
    auto create_venue = [&](oms::VenueId venue) {
        for (std::size_t i = 0; i < per_venue; ++i) {
            oms::Completion c{};
            const auto start = now_ns();
            const bool submitted = registry.submit_create(venue, 0, exec::BrokerOrderRef{venue, i + 1},
                                                           static_cast<std::uint32_t>(i % exec::max_symbols), 10, c);
            routing_hist.record(now_ns() - start);
            if (!submitted || !c.ok) { std::cerr << "FAIL create venue=" << venue << " i=" << i << "\n"; }
        }
    };
    std::thread ta(create_venue, VENUE_A);
    std::thread tb(create_venue, VENUE_B);
    ta.join();
    tb.join();
    const auto orders_ns = now_ns() - orders_start;
    const double orders_per_sec = orders_ns ? static_cast<double>(aggregate_n) * 1e9 / static_cast<double>(orders_ns) : 0.0;

    // ---- OMS baseline latency (submit_find), sampled ------------------------
    telemetry::Histogram<> find_hist;
    const std::size_t sample_n = std::min<std::size_t>(per_venue, 20'000);
    for (std::size_t i = 0; i < sample_n; ++i) {
        oms::Completion c{};
        const auto start = now_ns();
        (void)registry.find(VENUE_A)->oms().submit_find(0, exec::BrokerOrderRef{VENUE_A, i + 1}, c);
        find_hist.record(now_ns() - start);
    }

    // ---- reports/sec: ack + partial fill (stays live) per order, both venues -
    telemetry::Histogram<> report_hist;
    const auto reports_start = now_ns();
    auto report_venue = [&](oms::VenueId venue) {
        auto& venue_oms = registry.find(venue)->oms();
        for (std::size_t i = 0; i < per_venue; ++i) {
            const exec::BrokerOrderRef ref{venue, i + 1};
            // create() leaves pending_send; ack legally applies only from
            // sent, so advance the local send-lifecycle first.
            oms::Completion sent{};
            (void)venue_oms.submit_transition(0, ref, exec::OrderState::sent, sent);
            oms::Completion ack{}, fill{};
            oms::ExecReport rep_ack{ref, 1, oms::ReportKind::ack, 0, 0};
            oms::ExecReport rep_fill{ref, 2, oms::ReportKind::fill, 4, 0};  // 4 of 10 -> stays partially_filled
            auto start = now_ns();
            (void)venue_oms.submit_report(0, rep_ack, ack);
            report_hist.record(now_ns() - start);
            start = now_ns();
            (void)venue_oms.submit_report(0, rep_fill, fill);
            report_hist.record(now_ns() - start);
        }
    };
    std::thread ra(report_venue, VENUE_A);
    std::thread rb(report_venue, VENUE_B);
    ra.join();
    rb.join();
    const auto reports_ns = now_ns() - reports_start;
    const double reports_per_sec =
        reports_ns ? static_cast<double>(aggregate_n) * 2.0 * 1e9 / static_cast<double>(reports_ns) : 0.0;

    const auto mem_after_workload = mem_usage();

    // ---- reconciliation latency, per venue + aggregate -----------------------
    fake_a->open_orders.reserve(per_venue);
    fake_b->open_orders.reserve(per_venue);
    for (std::size_t i = 0; i < per_venue; ++i) {
        fake_a->open_orders.push_back(
            oms::OrderSnapshot{exec::BrokerOrderRef{VENUE_A, i + 1}, exec::OrderState::partially_filled, 10, 4});
        fake_b->open_orders.push_back(
            oms::OrderSnapshot{exec::BrokerOrderRef{VENUE_B, i + 1}, exec::OrderState::partially_filled, 10, 4});
    }

    std::vector<oms::ExecReport> report_scratch(16);
    std::vector<exec::Order> snap_scratch(cap_per_shard + 16);
    std::vector<oms::OrderSnapshot> local_scratch(per_venue + 16), venue_scratch(per_venue + 16);
    std::vector<oms::OrderReconcileResult> result_scratch(per_venue * 2 + 16);

    std::uint64_t reconcile_a_ns = 0, reconcile_b_ns = 0;
    bool reconcile_ok = true;
    for (const auto venue : {VENUE_A, VENUE_B}) {
        auto* conn = registry.find(venue);
        conn->disconnect();
        if (!conn->begin_recovery()) { reconcile_ok = false; continue; }
        const auto start = now_ns();
        const auto outcome = conn->run_reconciliation(
            0, report_scratch.data(), report_scratch.size(), snap_scratch.data(), snap_scratch.size(),
            local_scratch.data(), local_scratch.size(), venue_scratch.data(), venue_scratch.size(),
            result_scratch.data(), result_scratch.size());
        const auto elapsed = now_ns() - start;
        (venue == VENUE_A ? reconcile_a_ns : reconcile_b_ns) = elapsed;
        if (!outcome.clean() || conn->state() != oms::ConnectionState::ready) {
            std::cerr << "FAIL reconciliation venue=" << venue << " local_only=" << outcome.local_only
                      << " venue_only=" << outcome.venue_only << " state_mismatch=" << outcome.state_mismatch
                      << " volume_mismatch=" << outcome.volume_mismatch << "\n";
            reconcile_ok = false;
        }
    }
    const auto reconcile_aggregate_ns = reconcile_a_ns + reconcile_b_ns;

    // ---- cross-venue exposure (correctness spot-check, not just timing) -----
    std::vector<exec::Order> exposure_scratch(cap_per_shard + 16);
    const auto exposure_start = now_ns();
    const auto exposure = oms::aggregate_exposure(registry, 0, exposure_scratch.data(), exposure_scratch.size());
    const auto exposure_ns = now_ns() - exposure_start;
    // Every order: requested 10, filled 4 -> open 6, buy side (default) -> +6.
    const std::int64_t expected_aggregate = static_cast<std::int64_t>(aggregate_n) * 6;
    if (exposure.aggregate_open_volume != expected_aggregate) {
        std::cerr << "FAIL exposure aggregate=" << exposure.aggregate_open_volume
                  << " expected=" << expected_aggregate << "\n";
        reconcile_ok = false;
    }

    const auto mem_after = mem_usage();
    const auto cpu_after = cpu_usage();

    std::cout << "aggregate_orders=" << aggregate_n << " (per_venue=" << per_venue << ")\n";
    std::cout << "  orders_per_sec=" << orders_per_sec << " (" << (static_cast<double>(orders_ns) / 1e6) << " ms)\n";
    print_row("routing(create)", routing_hist);
    print_row("oms_find       ", find_hist);
    std::cout << "  reports_per_sec=" << reports_per_sec << " (" << (static_cast<double>(reports_ns) / 1e6)
              << " ms)\n";
    print_row("report", report_hist);
    std::cout << "  reconciliation_ns venue_a=" << reconcile_a_ns << " venue_b=" << reconcile_b_ns
              << " aggregate=" << reconcile_aggregate_ns << " (" << (static_cast<double>(reconcile_aggregate_ns) / 1e6)
              << " ms)\n";
    std::cout << "  exposure_compute_ns=" << exposure_ns << " aggregate_open_volume="
              << exposure.aggregate_open_volume << "\n";
    std::cout << "  RSS_MiB before_init=" << static_cast<double>(mem_before_init.rss_bytes) / 1048576.0
              << " after_init=" << static_cast<double>(mem_after_init.rss_bytes) / 1048576.0
              << " after_workload=" << static_cast<double>(mem_after_workload.rss_bytes) / 1048576.0
              << " final=" << static_cast<double>(mem_after.rss_bytes) / 1048576.0
              << " peak=" << static_cast<double>(mem_after.peak_rss_bytes) / 1048576.0 << "\n";
    std::cout << "  RSS_growth_after_init_MiB="
              << (static_cast<double>(mem_after_workload.rss_bytes) - static_cast<double>(mem_after_init.rss_bytes)) /
                     1048576.0
              << "  (allocation proxy: ShardedOms/VenueConnection preallocate at construction, submit_*/"
                 "reconciliation/exposure never allocate on their own paths -- growth here is scratch vectors "
                 "this benchmark itself allocates once, not per-op)\n";
    std::cout << "  cpu_seconds=" << (cpu_after.cpu_seconds - cpu_before.cpu_seconds) << "\n";

    return reconcile_ok;
}

}  // namespace

int main() {
    std::cout << "Multi-venue benchmark (Phase F). 2 venues.\n\n";
    bool all_ok = true;
    for (const std::size_t n : {std::size_t{1'000}, std::size_t{100'000}, std::size_t{1'000'000}}) {
        if (!run_scale(n)) all_ok = false;
        std::cout << "\n";
    }
    return all_ok ? 0 : 1;
}
