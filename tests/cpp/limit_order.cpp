#include "exec_fixture.hpp"

using namespace exec_fixture;
using exec::Side;
using exec::OrderState;

int main() {
    // Part 9.2: a limit order fills only on genuine market activity *through*
    // the level, never on touch. Touch-fills are the single most common way a
    // backtest invents profit that never existed.
    {
        exec::PaperBroker broker(config());
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 500, 1'000);

        const auto ref = broker.submit(limit(0, Side::buy, 10, 105, 1), 1'100);
        check(ref.accepted, "limit_accepted");
        const auto effective = broker.find_order(ref.ref)->ts_effective_ns;

        // Ask touches the limit exactly: must NOT fill.
        broker.on_quote(0, 100, 105, 500, 500, effective + 1);
        check(broker.find_order(ref.ref)->filled_volume == 0, "limit_no_fill_on_touch");

        // Ask trades through the limit: fills.
        broker.on_quote(0, 100, 104, 500, 500, effective + 2);
        const auto* order = broker.find_order(ref.ref);
        check(order->filled_volume > 0, "limit_fills_through_level");
        check(order->avg_fill_price_ticks == 105, "limit_fills_at_own_price");
        check(order->avg_fill_price_ticks != 102, "limit_not_mid_of_new_quote");
    }

    // Sell limit mirrors: fills only when the bid trades strictly above it.
    {
        exec::PaperBroker broker(config());
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 500, 1'000);
        const auto ref = broker.submit(limit(0, Side::sell, 10, 105, 2), 1'100);
        const auto effective = broker.find_order(ref.ref)->ts_effective_ns;

        broker.on_quote(0, 105, 110, 500, 500, effective + 1);
        check(broker.find_order(ref.ref)->filled_volume == 0, "limit_sell_no_fill_on_touch");
        broker.on_quote(0, 106, 110, 500, 500, effective + 2);
        const auto* order = broker.find_order(ref.ref);
        check(order->filled_volume > 0, "limit_sell_fills_through_level");
        check(order->avg_fill_price_ticks == 105, "limit_sell_fills_at_own_price");
    }

    // Validation is fail-closed on price.
    {
        exec::PaperBroker broker(config());
        auto spec = sized_symbol();
        spec.tick_size_ticks = 5;
        broker.set_symbol(spec);
        broker.on_quote(0, 100, 110, 500, 500, 1'000);

        check(!broker.submit(limit(0, Side::buy, 10, 0, 3), 1'100).accepted,
              "limit_zero_price_rejected");
        const auto misaligned = broker.submit(limit(0, Side::buy, 10, 103, 4), 1'100);
        check(!misaligned.accepted, "limit_misaligned_price_rejected");
        check(misaligned.reason == exec::RejectReason::price_not_tick_aligned,
              "limit_tick_alignment_reason");
    }

    // Expiry is honoured for a resting order that never trades through.
    {
        exec::PaperBroker broker(config());
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 500, 1'000);
        auto request = limit(0, Side::buy, 10, 50, 5);
        request.expire_ns = 5'000;
        const auto ref = broker.submit(request, 1'100);
        check(ref.accepted, "limit_expiry_accepted");
        broker.on_quote(0, 100, 110, 500, 500, 6'000);
        check(broker.find_order(ref.ref)->state == OrderState::expired, "limit_expired");
        check(broker.find_order(ref.ref)->filled_volume == 0, "limit_expired_unfilled");
    }

    return failures == 0 ? 0 : 1;
}
