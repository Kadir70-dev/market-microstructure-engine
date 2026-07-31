#include "exec_fixture.hpp"

using namespace exec_fixture;
using exec::Side;
using exec::SimulationMode;

int main() {
    const auto benign = exec::classify({1, 1, 1.0, 1e-6});
    const auto adverse = exec::classify({20, 2, 500.0, 1e-3});

    // Optimistic mode has no slippage at all (Part 9.4).
    for (std::uint64_t i = 0; i < 1'000; ++i)
        if (exec::SlippageModel::sample_ticks(1, i, SimulationMode::optimistic, benign, 10) != 0) {
            check(false, "slippage_optimistic_zero");
            return 1;
        }
    check(true, "slippage_optimistic_zero");

    // Never favourable. A model that can return negative slippage would make
    // pessimistic mode non-conservative and break the Part 9.4 ordering
    // invariant, so the sign is asserted exhaustively rather than assumed.
    {
        bool all_non_negative = true;
        for (int m = 0; m <= 2; ++m)
            for (std::uint64_t i = 0; i < 20'000; ++i)
                if (exec::SlippageModel::sample_ticks(2, i, static_cast<SimulationMode>(m),
                                                      adverse, 8) < 0) all_non_negative = false;
        check(all_non_negative, "slippage_never_favourable");
    }

    check(exec::SlippageModel::sample_ticks(5, 9, SimulationMode::base, benign, 10) ==
          exec::SlippageModel::sample_ticks(5, 9, SimulationMode::base, benign, 10),
          "slippage_deterministic");

    // Pessimistic is 2x base (Part 9.4), compared on means.
    {
        double base_sum = 0, pess_sum = 0;
        for (std::uint64_t i = 0; i < 20'000; ++i) {
            base_sum += static_cast<double>(
                exec::SlippageModel::sample_ticks(6, i, SimulationMode::base, adverse, 10));
            pess_sum += static_cast<double>(
                exec::SlippageModel::sample_ticks(6, i, SimulationMode::pessimistic, adverse, 10));
        }
        check(pess_sum > base_sum, "slippage_pessimistic_worse_than_base");
    }

    // Wider spreads scale slippage.
    {
        double narrow = 0, wide = 0;
        for (std::uint64_t i = 0; i < 20'000; ++i) {
            narrow += static_cast<double>(
                exec::SlippageModel::sample_ticks(7, i, SimulationMode::base, adverse, 2));
            wide += static_cast<double>(
                exec::SlippageModel::sample_ticks(7, i, SimulationMode::base, adverse, 40));
        }
        check(wide > narrow, "slippage_scales_with_spread");
    }

    // Applied against the trader in the book: a buy never fills better than ask.
    {
        bool always_adverse = true;
        for (std::uint64_t i = 0; i < 200; ++i) {
            exec::PaperBroker broker(config(SimulationMode::pessimistic));
            broker.set_symbol(sized_symbol());
            broker.on_quote(0, 1'000, 1'010, 500, 500, 1'000);
            const auto ref = broker.submit(market(0, Side::buy, 10, i), 1'100);
            if (!ref.accepted) continue;
            const auto effective = broker.find_order(ref.ref)->ts_effective_ns;
            broker.on_quote(0, 1'000, 1'010, 500, 500, effective + 1);
            const auto* order = broker.find_order(ref.ref);
            if (order->filled_volume > 0 && order->avg_fill_price_ticks < 1'010)
                always_adverse = false;
        }
        check(always_adverse, "slippage_buy_never_better_than_ask");
    }

    return failures == 0 ? 0 : 1;
}
