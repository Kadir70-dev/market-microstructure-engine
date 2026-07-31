#include "exec_fixture.hpp"

using namespace exec_fixture;
using exec::Side;

// Every way the simulator could manufacture a fill that could not have happened.
// These are the failures that matter most: they do not crash, they quietly make
// the backtest better than reality.

int main() {
    // ---- stop / stop-limit are refused, not invented ------------------------
    // Architecture Version 1.0 Part 9.2 defines fill mechanics for market and
    // limit only. Implementing a trigger rule would put unspecified semantics
    // into a determinism gate, so submission is refused outright.
    {
        exec::PaperBroker broker(config());
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 500, 1'000);

        auto stop = market(0, Side::buy, 10, 1);
        stop.type = exec::OrderType::stop;
        check(!broker.submit(stop, 1'100).accepted, "impossible_stop_order_refused");

        auto stop_limit = market(0, Side::buy, 10, 2);
        stop_limit.type = exec::OrderType::stop_limit;
        check(!broker.submit(stop_limit, 1'100).accepted, "impossible_stop_limit_refused");
    }

    // ---- volume validation is fail-closed -----------------------------------
    {
        exec::PaperBroker broker(config());
        auto spec = sized_symbol();
        spec.volume_min = 10;
        spec.volume_max = 1'000;
        spec.volume_step = 10;
        broker.set_symbol(spec);
        broker.on_quote(0, 100, 110, 500, 500, 1'000);

        check(broker.submit(market(0, Side::buy, 0, 3), 1'100).reason ==
              exec::RejectReason::invalid_volume, "impossible_zero_volume");
        check(broker.submit(market(0, Side::buy, -5, 4), 1'100).reason ==
              exec::RejectReason::invalid_volume, "impossible_negative_volume");
        check(broker.submit(market(0, Side::buy, 5, 5), 1'100).reason ==
              exec::RejectReason::volume_below_minimum, "impossible_below_min_volume");
        check(broker.submit(market(0, Side::buy, 5'000, 6), 1'100).reason ==
              exec::RejectReason::volume_above_maximum, "impossible_above_max_volume");
        check(broker.submit(market(0, Side::buy, 15, 7), 1'100).reason ==
              exec::RejectReason::volume_not_step_aligned, "impossible_step_misaligned");
    }

    // ---- timestamps must not go backwards -----------------------------------
    {
        exec::PaperBroker broker(config());
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 500, 1'000);
        check(broker.submit(market(0, Side::buy, 10, 8), 5'000).accepted, "impossible_ts_forward_ok");
        const auto backwards = broker.submit(market(0, Side::buy, 10, 9), 4'000);
        check(!backwards.accepted, "impossible_backwards_timestamp_rejected");
        check(backwards.reason == exec::RejectReason::invalid_timestamp, "impossible_ts_reason");
    }

    // ---- unknown symbol ------------------------------------------------------
    {
        exec::PaperBroker broker(config());
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 500, 1'000);
        const auto result = broker.submit(market(99, Side::buy, 10, 10), 1'100);
        check(!result.accepted, "impossible_unknown_symbol_rejected");
        check(result.reason == exec::RejectReason::unknown_symbol, "impossible_unknown_symbol_reason");
    }

    // ---- untradable symbol ---------------------------------------------------
    {
        exec::PaperBroker broker(config());
        auto spec = sized_symbol();
        spec.tradable = false;
        broker.set_symbol(spec);
        broker.on_quote(0, 100, 110, 500, 500, 1'000);
        check(broker.submit(market(0, Side::buy, 10, 11), 1'100).reason ==
              exec::RejectReason::market_closed, "impossible_untradable_rejected");
    }

    // ---- stale quote ---------------------------------------------------------
    {
        auto cfg = config();
        cfg.stale_quote_ns = 1'000;
        exec::PaperBroker broker(cfg);
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 500, 1'000);
        const auto result = broker.submit(market(0, Side::buy, 10, 12), 1'000'000);
        check(!result.accepted, "impossible_stale_quote_rejected");
        check(result.reason == exec::RejectReason::stale_quote, "impossible_stale_reason");
    }

    // ---- crossed / invalid market never fills --------------------------------
    {
        exec::PaperBroker broker(config());
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 110, 100, 500, 500, 1'000);   // crossed: ask < bid
        const auto result = broker.submit(market(0, Side::buy, 10, 13), 1'100);
        check(!result.accepted, "impossible_crossed_market_no_entry");
    }

    // ---- an accepted order never fills beyond its remaining volume -----------
    {
        exec::PaperBroker broker(config());
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 500, 1'000);
        const auto ref = broker.submit(market(0, Side::buy, 10, 14), 1'100);
        const auto effective = broker.find_order(ref.ref)->ts_effective_ns;
        // Enormous available size on every subsequent quote.
        for (int i = 0; i < 20; ++i)
            broker.on_quote(0, 100, 110, 1'000'000, 1'000'000,
                            effective + static_cast<std::uint64_t>(i));
        const auto* order = broker.find_order(ref.ref);
        check(order->filled_volume == 10, "impossible_never_overfills");
        check(order->remaining() == 0, "impossible_remaining_zero");
        check(!broker.halted(), "impossible_no_halt_on_valid_path");
    }

    return failures == 0 ? 0 : 1;
}
