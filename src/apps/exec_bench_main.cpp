#include <cstdint>
#include <iostream>
#include <memory>

#include "core/clock.hpp"
#include "exec/paper_broker.hpp"
#include "telemetry/histogram.hpp"

// Phase 5 benchmark. Part 18 target: >= 50,000 simulated fills/s.
//
// Throughput is measured over the whole loop including quote ingestion and
// account recomputation, because that is what a replay actually pays. Reporting
// fills/s while excluding the surrounding work would flatter the number.

int main() {
    exec::BrokerConfig config{};
    config.run_id = 1;
    config.run_seed = 20260731;
    config.mode = exec::SimulationMode::base;
    config.hedging = false;
    config.initial_balance_minor = 1'000'000'000'000LL;

    auto broker = std::make_unique<exec::PaperBroker>(config);

    exec::SymbolSpec spec{};
    spec.symbol_id = 0;
    spec.tick_size_ticks = 1;
    spec.volume_min = 1;
    spec.volume_max = 1'000'000;
    spec.volume_step = 1;
    spec.contract_size = 100'000;
    spec.tick_value_minor = 1;
    spec.commission_per_lot_minor = 700;
    spec.margin_rate_bp = 1;
    spec.tradable = true;
    broker->set_symbol(spec);

    telemetry::Histogram<> submit_ns;
    telemetry::Histogram<> quote_ns;

    // Stay below the fixed journal capacity: this measures steady-state fill
    // throughput, not fail-closed capacity exhaustion.
    constexpr std::uint64_t iterations = 1'000;
    std::uint64_t ts = 1'000'000;
    std::uint64_t submitted = 0;

    const auto start = core::monotonic_now_ns();
    for (std::uint64_t i = 0; i < iterations && !broker->halted(); ++i) {
        const std::int64_t drift = static_cast<std::int64_t>(i % 32);

        const auto q0 = core::monotonic_now_ns();
        broker->on_quote(0, 1'000 + drift, 1'010 + drift, 500, 500, ts);
        const auto q1 = core::monotonic_now_ns();
        quote_ns.record(q1 - q0);
        ts += 1'000;

        exec::OrderRequest request{};
        request.corr_id = i;
        request.strategy_id = 1;
        request.symbol_id = 0;
        request.side = exec::Side::buy;
        request.type = exec::OrderType::market;
        request.volume = 1;
        request.seq_global = i;

        const auto s0 = core::monotonic_now_ns();
        const auto result = broker->submit(request, ts);
        const auto s1 = core::monotonic_now_ns();
        submit_ns.record(s1 - s0);
        if (result.accepted) ++submitted;
        // Advance virtual time beyond the NOT_CALIBRATED base latency range so
        // the next quote activates and fills this order.
        ts += 20'000'000;
    }
    const auto wall_ns = core::monotonic_now_ns() - start;

    const double seconds = static_cast<double>(wall_ns) / 1e9;
    const double fills_per_second =
        seconds > 0.0 ? static_cast<double>(broker->fills()) / seconds : 0.0;

    std::cout << "PHASE5_BENCH iterations=" << iterations
              << " submitted=" << submitted
              << " fills=" << broker->fills()
              << " wall_seconds=" << seconds
              << " fills_per_second=" << fills_per_second
              << " submit_p50_ns=" << submit_ns.percentile(0.50)
              << " submit_p99_ns=" << submit_ns.percentile(0.99)
              << " quote_p50_ns=" << quote_ns.percentile(0.50)
              << " quote_p99_ns=" << quote_ns.percentile(0.99)
              << " journal_records=" << broker->journal().size()
              << " halted=" << (broker->halted() ? 1 : 0) << '\n';

    const bool throughput_ok = fills_per_second >= 50'000.0;
    std::cout << "PHASE5_TARGETS fills_per_second_over_50k=" << (throughput_ok ? 1 : 0) << '\n';
    return throughput_ok ? 0 : 1;
}
