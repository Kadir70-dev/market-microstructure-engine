#include "exec_fixture.hpp"

using namespace exec_fixture;
using exec::Side;
using exec::OrderState;

int main() {
    exec::PaperBroker broker(config());
    broker.set_symbol(sized_symbol());

    // A quote must exist before an order can be priced; submitting into a void
    // is a stale-quote rejection, not a fill at an imagined price.
    {
        exec::PaperBroker cold(config());
        cold.set_symbol(sized_symbol());
        const auto result = cold.submit(market(0, Side::buy, 10, 1), 1'000);
        check(!result.accepted, "market_rejected_without_quote");
        check(result.reason == exec::RejectReason::stale_quote, "market_stale_quote_reason");
    }

    broker.on_quote(0, 100, 110, 500, 500, 1'000);

    const auto submit = broker.submit(market(0, Side::buy, 10, 1), 1'100);
    check(submit.accepted, "market_accepted");
    const auto* order = broker.find_order(submit.ref);
    check(order != nullptr, "market_order_tracked");
    check(order->state == OrderState::sent, "market_state_sent_before_latency");
    check(order->filled_volume == 0, "market_unfilled_in_flight");
    check(order->ts_effective_ns > order->ts_submit_ns, "market_latency_applied_to_t_send");

    // Nothing fills until virtual time reaches t_send + lambda.
    broker.on_quote(0, 100, 110, 500, 500, order->ts_effective_ns - 1);
    check(broker.find_order(submit.ref)->filled_volume == 0, "market_no_fill_before_effective");

    broker.on_quote(0, 100, 110, 500, 500, order->ts_effective_ns + 1);
    const auto* filled = broker.find_order(submit.ref);
    check(filled->state == OrderState::filled, "market_filled");
    check(filled->filled_volume == 10, "market_full_volume");
    check(filled->remaining() == 0, "market_no_remainder");

    // A buy crosses the spread and pays the ask or worse — never better, and
    // never the mid.
    check(filled->avg_fill_price_ticks >= 110, "market_buy_pays_ask_or_worse");
    check(filled->avg_fill_price_ticks != 105, "market_buy_not_mid");

    // Sell side crosses the other way.
    {
        exec::PaperBroker seller(config());
        seller.set_symbol(sized_symbol());
        seller.on_quote(0, 100, 110, 500, 500, 1'000);
        const auto ref = seller.submit(market(0, Side::sell, 10, 2), 1'100);
        check(ref.accepted, "market_sell_accepted");
        const auto effective = seller.find_order(ref.ref)->ts_effective_ns;
        seller.on_quote(0, 100, 110, 500, 500, effective + 1);
        const auto* sold = seller.find_order(ref.ref);
        check(sold->state == OrderState::filled, "market_sell_filled");
        check(sold->avg_fill_price_ticks <= 100, "market_sell_receives_bid_or_worse");
        check(sold->avg_fill_price_ticks != 105, "market_sell_not_mid");
    }

    check(broker.fills() >= 1, "market_fill_counted");
    check(!broker.halted(), "market_no_halt");
    check(broker.journal().size() > 0, "market_journalled");
    return failures == 0 ? 0 : 1;
}
