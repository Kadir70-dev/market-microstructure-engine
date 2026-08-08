#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "oms/recovery_workflow.hpp"
#include "persist/execution_wal.hpp"
#include "telemetry/histogram.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

// Phase E benchmark: WAL append/read/tail latency, restart recovery time,
// reconciliation time at 1K/100K/1M orders, memory/RSS, reports/sec during
// catch-up, and time-to-READY. Correctness is proven separately by
// tests/cpp/phase7_recovery_fault_injection.cpp; this binary exits nonzero
// only if a required step unexpectedly fails.

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

std::filesystem::path bench_dir(const char* label) {
    const auto dir = std::filesystem::temp_directory_path() /
        (std::string("mme_phase7_bench_") + label + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

// ---- 1) WAL append/read/tail latency, and restart recovery time ----------
bool bench_wal_and_recovery(std::size_t record_count) {
    const auto dir = bench_dir("wal");
    persist::WalConfig config{};
    config.directory = dir;
    config.segment_data_bytes = 64ULL * 1024 * 1024;

    telemetry::Histogram<> append_hist, read_hist, tail_hist;
    bool ok = true;

    {
        persist::ExecutionWalWriter writer;
        persist::WalFileHeader header{};
        if (!writer.open(config, header, 0)) { std::cerr << "FAIL wal writer open\n"; return false; }
        for (std::size_t i = 0; i < record_count; ++i) {
            exec::JournalRecord rec{};
            rec.type = static_cast<std::uint16_t>(exec::JournalRecordType::command);
            rec.run_id = 1; rec.logical_order_id = i + 1; rec.b = 1; rec.c = 11; rec.d = 1;
            const auto start = now_ns();
            const bool appended = writer.append(rec, i);
            append_hist.record(now_ns() - start);
            if (!appended) { ok = false; break; }
        }
        (void)writer.flush();
        if (!ok) { std::cerr << "FAIL wal append\n"; return false; }
    }  // writer destructs (closes) -- simulates a clean-enough shutdown for the read-back measurement

    // Sequential read-back latency, one segment reader.
    {
        persist::SegmentCatalog catalog;
        if (!catalog.scan(dir) || catalog.empty()) { std::cerr << "FAIL wal catalog\n"; return false; }
        persist::ExecutionWalReader reader;
        if (!reader.open(catalog.segments()[0].path)) { std::cerr << "FAIL wal reader open\n"; return false; }
        std::size_t read_count = 0;
        for (;;) {
            exec::JournalRecord rec{};
            const auto start = now_ns();
            const auto status = reader.next(rec);
            read_hist.record(now_ns() - start);
            if (status == persist::ReadStatus::ok) { ++read_count; continue; }
            break;
        }
        if (read_count != record_count) { std::cerr << "FAIL wal read count mismatch\n"; return false; }
    }

    // Tailer latency (fresh tailer, cold start against the now-sealed segment).
    {
        persist::ExecutionWalTailer tailer;
        if (!tailer.open(dir)) { std::cerr << "FAIL tailer open\n"; return false; }
        std::size_t tail_count = 0;
        for (;;) {
            exec::JournalRecord rec{};
            const auto start = now_ns();
            const auto status = tailer.next(rec);
            tail_hist.record(now_ns() - start);
            if (status == persist::ExecTailStatus::ok) { ++tail_count; continue; }
            break;
        }
        if (tail_count != record_count) { std::cerr << "FAIL tailer read count mismatch\n"; return false; }
    }

    // Restart recovery time: WAL -> Journal -> oms::recover().
    const auto recovery_start = now_ns();
    persist::ExecutionWalTailer tailer;
    bool recovery_ok = tailer.open(dir);
    exec::Journal journal;
    if (recovery_ok) {
        for (;;) {
            exec::JournalRecord rec{};
            const auto status = tailer.next(rec);
            if (status == persist::ExecTailStatus::ok) {
                if (!journal.append(rec)) { recovery_ok = false; break; }
                continue;
            }
            break;
        }
    }
    oms::RecoveryState state(portfolio::MarginMode::hedging);
    recovery_ok = recovery_ok && oms::recover(journal, state);
    const auto recovery_ns = now_ns() - recovery_start;
    if (!recovery_ok) { std::cerr << "FAIL restart recovery\n"; return false; }
    if (state.orders.size() != record_count) { std::cerr << "FAIL recovered order count mismatch\n"; return false; }

    std::cout << "wal_and_recovery records=" << record_count << "\n";
    print_row("append", append_hist);
    print_row("read  ", read_hist);
    print_row("tail  ", tail_hist);
    std::cout << "  restart_recovery_ns=" << recovery_ns
              << " (" << (static_cast<double>(recovery_ns) / 1e6) << " ms for " << record_count << " records)\n";

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return true;
}

// ---- 2) reconciliation time / memory / catch-up throughput / time-to-READY --
bool bench_reconciliation(std::size_t order_count) {
    const auto token = approval();
    const auto mem_before = mem_usage();

    exec::BrokerConfig broker_config{};
    broker_config.run_id = 1;
    broker_config.initial_balance_minor = 1'000'000'000'000LL;
    exec::PaperBroker broker(broker_config);
    exec::SymbolSpec spec{};
    spec.symbol_id = 0; spec.tick_size_ticks = 1; spec.volume_min = 1; spec.volume_max = 1'000'000;
    spec.volume_step = 1; spec.contract_size = 1; spec.tick_value_minor = 1; spec.tradable = true;
    broker.set_symbol(spec);

    // PaperBroker itself is capped at 256 live orders (documented, unrelated
    // limitation this phase does not remove -- see report). Reconciliation
    // at 1K/100K/1M is therefore benchmarked directly against Oms/venue
    // snapshot arrays sized to those counts, which is what OrderReconciler
    // actually operates over; PaperBroker here only supplies a handful of
    // real orders for the catch-up/report-throughput measurement below.

    oms::Oms local(order_count + 16);
    std::vector<oms::OrderSnapshot> local_snap(order_count), venue_snap(order_count);
    for (std::size_t i = 0; i < order_count; ++i) {
        const exec::BrokerOrderRef ref{1, i + 1};
        auto* o = local.create(ref, 0, 1, token);
        if (o == nullptr) { std::cerr << "FAIL local create at " << i << "\n"; return false; }
        (void)local.transition(ref, exec::OrderState::sent);
        local_snap[i] = oms::OrderSnapshot{ref, exec::OrderState::sent, 1, 0};
        venue_snap[i] = oms::OrderSnapshot{ref, exec::OrderState::sent, 1, 0};  // matched, clean case
    }
    const auto mem_after_populate = mem_usage();

    std::vector<oms::OrderReconcileResult> results(order_count * 2 + 16);
    oms::OrderReconciler reconciler;
    const auto compare_start = now_ns();
    const auto result_n = reconciler.compare(local_snap.data(), local_snap.size(), venue_snap.data(),
                                             venue_snap.size(), results.data(), results.size());
    const auto compare_ns = now_ns() - compare_start;
    const auto mem_after_compare = mem_usage();

    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < result_n; ++i)
        if (results[i].status != oms::OrderReconcileStatus::matched) ++mismatches;
    if (mismatches != 0) { std::cerr << "FAIL unexpected mismatches in clean scenario\n"; return false; }

    std::cout << "reconciliation order_count=" << order_count << "\n";
    std::cout << "  compare_ns=" << compare_ns << " (" << (static_cast<double>(compare_ns) / 1e6)
              << " ms), matched=" << result_n << "\n";
    std::cout << "  RSS_MiB before=" << static_cast<double>(mem_before.rss_bytes) / 1048576.0
              << " after_populate=" << static_cast<double>(mem_after_populate.rss_bytes) / 1048576.0
              << " after_compare=" << static_cast<double>(mem_after_compare.rss_bytes) / 1048576.0
              << " peak=" << static_cast<double>(mem_after_compare.peak_rss_bytes) / 1048576.0 << "\n";

    // ---- time-to-READY + reports/sec during catch-up, via a real ---------
    // ---- disconnect -> recover -> reconcile cycle with N missed reports --
    const std::size_t catchup_n = std::min<std::size_t>(order_count, 50'000);
    oms::Oms catchup_local(catchup_n + 16);
    std::vector<exec::BrokerOrderRef> refs(catchup_n);
    for (std::size_t i = 0; i < catchup_n; ++i) {
        refs[i] = exec::BrokerOrderRef{2, i + 1};
        (void)catchup_local.create(refs[i], 0, 10, token);
        (void)catchup_local.transition(refs[i], exec::OrderState::sent);
    }

    struct FakeVenue final : public oms::VenueAdapter {
        const std::vector<exec::BrokerOrderRef>* refs_;
        [[nodiscard]] std::size_t fetch_open_orders(oms::OrderSnapshot* out, std::size_t cap) const noexcept override {
            std::size_t n = 0;
            for (const auto& ref : *refs_) {
                if (n >= cap) break;
                // Must match the *resulting* local state after applying this
                // adapter's own fetch_recent_reports (ack then fill of 3),
                // i.e. partially_filled with filled=3 -- not the pre-fill
                // acknowledged state, or reconciliation correctly (but
                // unhelpfully, for this benchmark's clean-scenario purpose)
                // flags a state mismatch.
                out[n++] = oms::OrderSnapshot{ref, exec::OrderState::partially_filled, 10, 3};
            }
            return n;
        }
        [[nodiscard]] std::size_t fetch_positions(oms::PositionSnapshot*, std::size_t) const noexcept override {
            return 0;
        }
        [[nodiscard]] std::size_t fetch_recent_reports(oms::ExecReport* out, std::size_t cap) const noexcept override {
            std::size_t n = 0;
            for (const auto& ref : *refs_) {
                if (n + 1 >= cap) break;
                out[n++] = oms::ExecReport{ref, 1, oms::ReportKind::ack, 0, 0};
                out[n++] = oms::ExecReport{ref, 2, oms::ReportKind::fill, 3, 0};
            }
            return n;
        }
        [[nodiscard]] exec::AccountState fetch_account() const noexcept override { return exec::AccountState{}; }
    } fake_venue;
    fake_venue.refs_ = &refs;

    oms::ReportSequencer catchup_seq(catchup_n + 16, 4096);
    oms::RecoveryWorkflow workflow(catchup_local, fake_venue, catchup_seq);

    const auto ready_start = now_ns();
    workflow.disconnect();
    if (!workflow.begin_recovery(true)) { std::cerr << "FAIL begin_recovery\n"; return false; }
    std::vector<oms::ExecReport> report_scratch(catchup_n * 2 + 16);
    std::vector<oms::OrderSnapshot> cl_scratch(catchup_n + 16), cv_scratch(catchup_n + 16);
    std::vector<oms::OrderReconcileResult> cr_scratch(catchup_n + 16);
    const auto outcome = workflow.run_reconciliation(
        report_scratch.data(), report_scratch.size(), cl_scratch.data(), cl_scratch.size(),
        cv_scratch.data(), cv_scratch.size(), cr_scratch.data(), cr_scratch.size());
    const auto ready_ns = now_ns() - ready_start;
    const bool reached_ready = workflow.state() == oms::ConnectionState::ready;

    const double reports_per_sec = ready_ns
        ? static_cast<double>(outcome.reports_applied) * 1e9 / static_cast<double>(ready_ns) : 0.0;
    std::cout << "  catch_up order_count=" << catchup_n << " reports_applied=" << outcome.reports_applied
              << " time_to_ready_ns=" << ready_ns << " (" << (static_cast<double>(ready_ns) / 1e6) << " ms)"
              << " reached_ready=" << (reached_ready ? 1 : 0)
              << " reports_per_sec=" << reports_per_sec << "\n";
    if (!reached_ready) { std::cerr << "FAIL did not reach READY in clean catch-up scenario\n"; return false; }

    return true;
}

}  // namespace

int main() {
    std::cout << "Recovery/reconciliation benchmark (Phase E).\n\n";
    bool all_ok = true;
    for (const std::size_t n : {std::size_t{100}, std::size_t{2'000}}) {
        if (!bench_wal_and_recovery(n)) all_ok = false;
        std::cout << "\n";
    }
    for (const std::size_t n : {std::size_t{1'000}, std::size_t{100'000}, std::size_t{1'000'000}}) {
        if (!bench_reconciliation(n)) all_ok = false;
        std::cout << "\n";
    }
    return all_ok ? 0 : 1;
}
