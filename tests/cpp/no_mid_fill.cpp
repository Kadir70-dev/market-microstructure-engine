#include "exec_fixture.hpp"

using namespace exec_fixture;
using exec::Side;

// Part 9.2 / Part 18 Phase 5: "no code path can produce a mid-price fill".
//
// This is the single most consequential guarantee in the simulator. A mid fill
// silently hands the strategy half the spread on every trade, which is enough to
// make a losing system look profitable. The check is therefore exhaustive over a
// wide sweep of spreads, sides, modes and order types rather than spot-checked.

int main() {
    int mid_fills = 0;
    int total_fills = 0;

    for (int mode_index = 0; mode_index <= 2; ++mode_index) {
        const auto mode = static_cast<exec::SimulationMode>(mode_index);
        for (std::int64_t spread = 2; spread <= 40; spread += 2) {
            for (int side_index = 0; side_index <= 1; ++side_index) {
                const auto side = static_cast<Side>(side_index);

                // Market order path.
                {
                    exec::PaperBroker broker(config(mode));
                    broker.set_symbol(sized_symbol());
                    const std::int64_t bid = 1'000;
                    const std::int64_t ask = bid + spread;
                    broker.on_quote(0, bid, ask, 500, 500, 1'000);
                    const auto ref = broker.submit(
                        market(0, side, 10, static_cast<std::uint64_t>(spread * 10 + side_index)),
                        1'100);
                    if (!ref.accepted) continue;
                    const auto effective = broker.find_order(ref.ref)->ts_effective_ns;
                    broker.on_quote(0, bid, ask, 500, 500, effective + 1);
                    const auto* order = broker.find_order(ref.ref);
                    if (order->filled_volume > 0) {
                        ++total_fills;
                        // Mid is (bid+ask)/2; compared doubled to stay integer.
                        if (order->avg_fill_price_ticks * 2 == bid + ask) ++mid_fills;
                        // Stronger: a marketable fill must be at the touch or worse.
                        const bool at_touch_or_worse = (side == Side::buy)
                            ? order->avg_fill_price_ticks >= ask
                            : order->avg_fill_price_ticks <= bid;
                        if (!at_touch_or_worse) ++mid_fills;
                    }
                }

                // Limit order path: passive fills occur at the limit price, which
                // is chosen strictly off-mid here.
                {
                    exec::PaperBroker broker(config(mode));
                    broker.set_symbol(sized_symbol());
                    const std::int64_t bid = 1'000;
                    const std::int64_t ask = bid + spread;
                    broker.on_quote(0, bid, ask, 500, 500, 1'000);
                    const auto price = (side == Side::buy) ? bid : ask;
                    const auto ref = broker.submit(
                        limit(0, side, 10, price,
                              static_cast<std::uint64_t>(5'000 + spread * 10 + side_index)),
                        1'100);
                    if (!ref.accepted) continue;
                    const auto effective = broker.find_order(ref.ref)->ts_effective_ns;
                    // Push the market strictly through the resting level.
                    if (side == Side::buy) broker.on_quote(0, bid, price - 1, 500, 500, effective + 1);
                    else broker.on_quote(0, price + 1, ask, 500, 500, effective + 1);
                    const auto* order = broker.find_order(ref.ref);
                    if (order->filled_volume > 0) {
                        ++total_fills;
                        if (order->avg_fill_price_ticks * 2 == bid + ask) ++mid_fills;
                    }
                }
            }
        }
    }

    std::cout << "no_mid_fill_total_fills=" << total_fills << '\n';
    check(total_fills > 100, "no_mid_fill_sweep_actually_filled");
    check(mid_fills == 0, "no_mid_fill_zero_mid_priced_fills");
    return failures == 0 ? 0 : 1;
}
