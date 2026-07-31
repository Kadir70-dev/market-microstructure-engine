#include "exec_fixture.hpp"

using namespace exec_fixture;
using exec::Side;
using exec::SimulationMode;

int main() {
    const auto benign = exec::classify({1, 1, 1.0, 1e-6});
    const auto adverse = exec::classify({20, 2, 500.0, 1e-3});
    check(!benign.adverse(), "latency_benign_bucket");
    check(adverse.adverse(), "latency_adverse_bucket");

    // Determinism: same key, same draw, always.
    check(exec::LatencyModel::sample_ns(7, 42, SimulationMode::base, benign) ==
          exec::LatencyModel::sample_ns(7, 42, SimulationMode::base, benign),
          "latency_deterministic");

    // Mode ordering: optimistic p10 < base p50 < pessimistic p99. Compared on
    // means, since any single draw can cross.
    {
        double sums[3] = {0, 0, 0};
        for (std::uint64_t i = 0; i < 5'000; ++i)
            for (int m = 0; m <= 2; ++m)
                sums[m] += static_cast<double>(exec::LatencyModel::sample_ns(
                    9, i, static_cast<SimulationMode>(m), benign));
        check(sums[0] < sums[1] && sums[1] < sums[2], "latency_mode_ordering");
    }

    // Regime conditionality: an adverse bucket stretches latency. Broker latency,
    // slippage and rejects spike together, so independent sampling would flatter
    // the backtest exactly when it matters (Part 9.3).
    {
        double calm = 0, stressed = 0;
        for (std::uint64_t i = 0; i < 5'000; ++i) {
            calm += static_cast<double>(
                exec::LatencyModel::sample_ns(11, i, SimulationMode::base, benign));
            stressed += static_cast<double>(
                exec::LatencyModel::sample_ns(11, i, SimulationMode::base, adverse));
        }
        check(stressed > calm, "latency_adverse_regime_slower");
    }

    // Pessimistic mode uses the adverse bucket unconditionally (Part 9.3), so
    // the observed regime must not change its draws.
    check(exec::LatencyModel::sample_ns(3, 1, SimulationMode::pessimistic, benign) ==
          exec::LatencyModel::sample_ns(3, 1, SimulationMode::pessimistic, adverse),
          "latency_pessimistic_ignores_observed_regime");

    // Part 9.2: sampled at t_send + lambda, never t_decision. The broker adds it
    // to the submit timestamp, and nothing fills before that instant.
    {
        exec::PaperBroker broker(config(SimulationMode::base));
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 500, 1'000'000);
        const auto ref = broker.submit(market(0, Side::buy, 10, 1), 2'000'000);
        const auto* order = broker.find_order(ref.ref);
        check(order->ts_submit_ns == 2'000'000, "latency_t_send_recorded");
        check(order->ts_effective_ns > order->ts_submit_ns, "latency_added_to_t_send");

        broker.on_quote(0, 100, 110, 500, 500, order->ts_effective_ns - 1);
        check(broker.find_order(ref.ref)->filled_volume == 0, "latency_no_fill_before_lambda");
        broker.on_quote(0, 100, 110, 500, 500, order->ts_effective_ns);
        check(broker.find_order(ref.ref)->filled_volume > 0, "latency_fills_at_lambda");
    }

    return failures == 0 ? 0 : 1;
}
