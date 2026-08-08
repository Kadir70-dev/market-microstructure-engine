#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>

#include "oms/report_sequencer.hpp"
#include "oms/sharded_oms.hpp"
#include "telemetry/histogram.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

// Phase D benchmark: execution-report processing latency/throughput at 1K,
// 100K, 1M live orders. Standalone (single-threaded, direct calls into
// ReportSequencer::process) is the apples-to-apples comparison against Phase
// B's direct-call Oms numbers; the ShardedOms-integrated pass is the
// comparison against Phase C's queue-mediated numbers. Correctness is proven
// separately by tests/cpp/phase6_report_sequencer.cpp; this binary exits
// nonzero only if a report that must succeed does not.

namespace {

struct MemUsage final { std::uint64_t rss_bytes{0}, peak_rss_bytes{0}; };

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
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void print_row(const char* op, const telemetry::Histogram<>& h) {
    std::cout << "    " << op << " n=" << h.count() << " p50=" << h.percentile(0.50)
              << "ns p95=" << h.percentile(0.95) << "ns p99=" << h.percentile(0.99)
              << "ns p999=" << h.percentile(0.999) << "ns\n";
}

risk::Approval approval() {
    static risk::RiskEngine e(risk::Limits{});
    risk::Request q{};
    q.volume = 1; q.risk_minor = 1; q.free_margin = 1'000'000; q.warm_mask = 1; q.session_open = true;
    return e.check(q).token;
}

bool run_standalone(std::size_t n) {
    const auto token = approval();
    const auto mem_before = mem_usage();

    oms::Oms o(n);
    // Gap pool / terminal cache are fixed-capacity regardless of n -- the
    // whole point of "bounded/preallocated storage" -- sized generously here
    // only so the gap-phase sample below (up to 50,000 held reports at once)
    // fits; this is a benchmark-scenario choice, not a per-order cost.
    oms::ReportSequencer seq(/*gap_pool_capacity=*/60'000, /*terminal_cache_capacity=*/4096);
    const auto mem_after_ctor = mem_usage();

    telemetry::Histogram<> ack_hist, fill_hist, dup_hist, gap_hold_hist, gap_cascade_hist;
    bool ok = true;

    // ---- setup: create + internal sent + ack every order -------------------
    const auto setup_start = now_ns();
    for (std::size_t i = 1; i <= n; ++i) {
        const exec::BrokerOrderRef ref{1, i};
        if (o.create(ref, static_cast<std::uint32_t>(i % 8), 10, token) == nullptr) { ok = false; break; }
        if (!o.transition(ref, exec::OrderState::sent)) { ok = false; break; }
        const auto op_start = now_ns();
        const auto outcome = seq.process(o, oms::ExecReport{ref, 1, oms::ReportKind::ack, 0, 0});
        ack_hist.record(now_ns() - op_start);
        if (outcome != oms::ReportOutcome::applied) { ok = false; break; }
    }
    const auto setup_ns = now_ns() - setup_start;
    if (!ok) { std::cerr << "FAIL standalone n=" << n << " setup\n"; return false; }

    // ---- primary metric: in-sequence fill report per order, no gaps -------
    const auto fill_wall_start = now_ns();
    for (std::size_t i = 1; i <= n; ++i) {
        const exec::BrokerOrderRef ref{1, i};
        const auto op_start = now_ns();
        const auto outcome = seq.process(o, oms::ExecReport{ref, 2, oms::ReportKind::fill, 1, 0});
        fill_hist.record(now_ns() - op_start);
        if (outcome != oms::ReportOutcome::applied) { ok = false; break; }
    }
    const auto fill_wall_ns = now_ns() - fill_wall_start;
    const auto mem_after_fill = mem_usage();
    if (!ok) { std::cerr << "FAIL standalone n=" << n << " fill\n"; return false; }

    // ---- duplicate-rejection path -------------------------------------------
    for (std::size_t i = 1; i <= n; ++i) {
        const exec::BrokerOrderRef ref{1, i};
        const auto op_start = now_ns();
        const auto outcome = seq.process(o, oms::ExecReport{ref, 2, oms::ReportKind::fill, 1, 0});
        dup_hist.record(now_ns() - op_start);
        if (outcome != oms::ReportOutcome::duplicate) { ok = false; break; }
    }
    if (!ok) { std::cerr << "FAIL standalone n=" << n << " duplicate\n"; return false; }

    // ---- gap hold + cascade-apply path, sampled -----------------------------
    const std::size_t gap_samples = std::min<std::size_t>(n, 50'000);
    for (std::size_t i = 1; i <= gap_samples; ++i) {
        const exec::BrokerOrderRef ref{1, i};
        auto hold_start = now_ns();
        const auto held = seq.process(o, oms::ExecReport{ref, 4, oms::ReportKind::fill, 1, 0});  // seq 3 skipped
        gap_hold_hist.record(now_ns() - hold_start);
        if (held != oms::ReportOutcome::held_for_gap) { ok = false; break; }
    }
    for (std::size_t i = 1; i <= gap_samples; ++i) {
        const exec::BrokerOrderRef ref{1, i};
        auto cascade_start = now_ns();
        const auto filled = seq.process(o, oms::ExecReport{ref, 3, oms::ReportKind::fill, 1, 0});
        gap_cascade_hist.record(now_ns() - cascade_start);
        if (filled != oms::ReportOutcome::applied) { ok = false; break; }
    }
    const auto mem_after_gap = mem_usage();
    if (!ok) { std::cerr << "FAIL standalone n=" << n << " gap\n"; return false; }

    const double fill_throughput = fill_wall_ns
        ? static_cast<double>(n) * 1e9 / static_cast<double>(fill_wall_ns) : 0.0;

    std::cout << "standalone n=" << n << "\n";
    std::cout << "  setup_ns=" << setup_ns << " fill_wall_ns=" << fill_wall_ns
              << " fill_throughput_reports_per_s=" << fill_throughput << "\n";
    print_row("ack (setup)      ", ack_hist);
    print_row("fill (in-seq)    ", fill_hist);
    print_row("duplicate-reject ", dup_hist);
    print_row("gap-hold         ", gap_hold_hist);
    print_row("gap-cascade-apply", gap_cascade_hist);
    std::cout << "  RSS_MiB before=" << static_cast<double>(mem_before.rss_bytes) / 1048576.0
              << " after_ctor=" << static_cast<double>(mem_after_ctor.rss_bytes) / 1048576.0
              << " after_fill=" << static_cast<double>(mem_after_fill.rss_bytes) / 1048576.0
              << " after_gap=" << static_cast<double>(mem_after_gap.rss_bytes) / 1048576.0 << "\n";
    std::cout << "  RSS_growth_fill_to_gap_MiB="
              << static_cast<double>(mem_after_gap.rss_bytes >= mem_after_fill.rss_bytes
                                          ? mem_after_gap.rss_bytes - mem_after_fill.rss_bytes : 0) / 1048576.0
              << " (sequencer's own storage is fixed-capacity, independent of n)\n";
    return true;
}

bool run_sharded(std::size_t n) {
    // shard_count must match producer count (1 here), the same convention
    // oms_concurrent_bench_main.cpp uses -- Phase C measured and documented
    // that idle shard-owner threads outnumbering producers causes severe
    // oversubscription-driven slowdowns on this 4-core box (explicitly not
    // being re-optimized in this phase). A single producer against 4 shards
    // would just reproduce that already-documented issue here and conflate
    // it with what this benchmark is actually trying to isolate: the cost
    // report sequencing adds on top of the queue round trip.
    const auto shard_count = std::size_t{1};
    oms::ShardedOms s(shard_count, n / shard_count + 16, 1, n / shard_count + 16,
                      /*gap_pool_capacity_per_shard=*/1024, /*terminal_cache_capacity_per_shard=*/4096);

    telemetry::Histogram<> fill_hist;
    bool ok = true;

    for (std::size_t i = 1; i <= n; ++i) {
        const exec::BrokerOrderRef ref{1, i};
        oms::Completion c{};
        if (!s.submit_create(0, ref, static_cast<std::uint32_t>(i % 8), 10, c) || !c.ok) { ok = false; break; }
        oms::Completion sent{};
        if (!s.submit_transition(0, ref, exec::OrderState::sent, sent) || !sent.ok) { ok = false; break; }
        oms::Completion ack{};
        if (!s.submit_report(0, oms::ExecReport{ref, 1, oms::ReportKind::ack, 0, 0}, ack) ||
            ack.report_outcome != oms::ReportOutcome::applied) { ok = false; break; }
    }
    if (!ok) { std::cerr << "FAIL sharded n=" << n << " setup\n"; return false; }

    const auto wall_start = now_ns();
    for (std::size_t i = 1; i <= n; ++i) {
        const exec::BrokerOrderRef ref{1, i};
        const auto op_start = now_ns();
        oms::Completion c{};
        const bool accepted = s.submit_report(0, oms::ExecReport{ref, 2, oms::ReportKind::fill, 1, 0}, c);
        fill_hist.record(now_ns() - op_start);
        if (!accepted || c.report_outcome != oms::ReportOutcome::applied) { ok = false; break; }
    }
    const auto wall_ns = now_ns() - wall_start;
    if (!ok) { std::cerr << "FAIL sharded n=" << n << " fill\n"; return false; }

    const double throughput = wall_ns ? static_cast<double>(n) * 1e9 / static_cast<double>(wall_ns) : 0.0;
    std::cout << "sharded (" << shard_count << " shard, 1 producer) n=" << n << "\n";
    std::cout << "  fill_wall_ns=" << wall_ns << " fill_throughput_reports_per_s=" << throughput << "\n";
    print_row("fill (queue round trip)", fill_hist);
    return true;
}

}  // namespace

int main() {
    std::cout << "Report sequencer benchmark (Phase D). Compare 'fill (in-seq)' below against "
                 "Phase B direct-call Oms::fill()/transition() (e.g. 1M-occupancy p99=512ns) and "
                 "Phase C ShardedOms queue round trip (e.g. 1M/1-thread create p99=512ns) -- the "
                 "difference is the cost of the dedup/gap check added on top.\n\n";

    bool all_ok = true;
    for (const std::size_t n : {std::size_t{1'000}, std::size_t{100'000}, std::size_t{1'000'000}}) {
        if (!run_standalone(n)) all_ok = false;
        std::cout << "\n";
        if (!run_sharded(n)) all_ok = false;
        std::cout << "\n";
    }
    return all_ok ? 0 : 1;
}
