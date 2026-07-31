#include "exec_fixture.hpp"

using namespace exec_fixture;
using exec::Side;

namespace {
// Fills an order to completion and returns the average fill price.
std::int64_t fill(exec::PaperBroker& b, const exec::OrderRequest& req, std::uint64_t& ts,
                  std::int64_t bid, std::int64_t ask) {
    const auto ref = b.submit(req, ts);
    if (!ref.accepted) return -1;
    const auto effective = b.find_order(ref.ref)->ts_effective_ns;
    ts = effective + 1;
    b.on_quote(0, bid, ask, 500, 500, ts);
    const auto* o = b.find_order(ref.ref);
    return (o != nullptr && o->state == exec::OrderState::filled) ? o->avg_fill_price_ticks : -1;
}

exec::OrderRequest reduce(std::uint32_t sym, Side side, std::int64_t vol, std::uint64_t seq) {
    auto r = market(sym, side, vol, seq);
    r.reduce_only = true;
    return r;
}
}

int main() {
    const auto cfg = [] { return config(exec::SimulationMode::optimistic, /*hedging=*/true); };

    // ---- long entry -> sell reduce_only full close -------------------------
    {
        exec::PaperBroker b(cfg());
        b.set_symbol(sized_symbol());
        std::uint64_t ts = 1'000;
        b.on_quote(0, 100, 110, 500, 500, ts);
        const auto entry = fill(b, market(0, Side::buy, 10, 1), ts, 100, 110);
        check(entry == 110, "long_entry_at_ask");
        check(b.positions().open_count() == 1, "long_entry_one_position");

        b.on_quote(0, 200, 210, 500, 500, ++ts);
        const auto exit = fill(b, reduce(0, Side::sell, 10, 2), ts, 200, 210);
        check(exit == 200, "long_exit_at_bid");
        check(b.positions().open_count() == 0, "long_full_close_zero_open");
        // (200 - 110) * 10 = 900
        check(b.account().realized_pnl_minor == 900, "long_realized_pnl");
        check(b.account().unrealized_pnl_minor == 0, "long_no_residual_unrealized");
        check(b.positions().net_volume(0) == 0, "long_zero_residual_exposure");
    }

    // ---- short entry -> buy reduce_only full close -------------------------
    {
        exec::PaperBroker b(cfg());
        b.set_symbol(sized_symbol());
        std::uint64_t ts = 1'000;
        b.on_quote(0, 200, 210, 500, 500, ts);
        const auto entry = fill(b, market(0, Side::sell, 10, 1), ts, 200, 210);
        check(entry == 200, "short_entry_at_bid");

        b.on_quote(0, 100, 110, 500, 500, ++ts);
        const auto exit = fill(b, reduce(0, Side::buy, 10, 2), ts, 100, 110);
        check(exit == 110, "short_exit_at_ask");
        check(b.positions().open_count() == 0, "short_full_close_zero_open");
        // (200 - 110) * 10 = 900
        check(b.account().realized_pnl_minor == 900, "short_realized_pnl");
        check(b.positions().net_volume(0) == 0, "short_zero_residual_exposure");
    }

    // ---- partial close ------------------------------------------------------
    {
        exec::PaperBroker b(cfg());
        b.set_symbol(sized_symbol());
        std::uint64_t ts = 1'000;
        b.on_quote(0, 100, 110, 500, 500, ts);
        (void)fill(b, market(0, Side::buy, 10, 1), ts, 100, 110);

        b.on_quote(0, 200, 210, 500, 500, ++ts);
        (void)fill(b, reduce(0, Side::sell, 4, 2), ts, 200, 210);
        check(b.positions().open_count() == 1, "partial_position_still_open");
        check(b.positions().net_volume(0) == 6, "partial_remaining_volume");
        // (200 - 110) * 4 = 360, proportional to the closed quantity only
        check(b.account().realized_pnl_minor == 360, "partial_proportional_realized");

        (void)fill(b, reduce(0, Side::sell, 6, 3), ts, 200, 210);
        check(b.positions().open_count() == 0, "partial_then_full_close");
        check(b.account().realized_pnl_minor == 900, "partial_total_realized");
        check(b.positions().net_volume(0) == 0, "partial_zero_residual");
    }

    // ---- rejections ---------------------------------------------------------
    {
        exec::PaperBroker b(cfg());
        b.set_symbol(sized_symbol());
        std::uint64_t ts = 1'000;
        b.on_quote(0, 100, 110, 500, 500, ts);

        // No position at all.
        auto r = b.submit(reduce(0, Side::sell, 5, 1), ts);
        check(!r.accepted, "reject_no_position");
        check(r.reason == exec::RejectReason::reduce_only_no_position, "reject_no_position_reason");

        (void)fill(b, market(0, Side::buy, 10, 2), ts, 100, 110);

        // Same side as the open position would increase exposure.
        r = b.submit(reduce(0, Side::buy, 5, 3), ++ts);
        check(!r.accepted, "reject_same_side_would_increase");
        check(r.reason == exec::RejectReason::reduce_only_no_position, "reject_same_side_reason");

        // More than the reducible quantity.
        r = b.submit(reduce(0, Side::sell, 25, 4), ++ts);
        check(!r.accepted, "reject_excess_volume");
        check(r.reason == exec::RejectReason::reduce_only_excess_volume, "reject_excess_reason");

        check(b.positions().net_volume(0) == 10, "rejects_left_exposure_unchanged");
        check(b.positions().open_count() == 1, "rejects_left_position_count");
    }

    // ---- normal (non-reduce_only) hedging entries still open tickets --------
    {
        exec::PaperBroker b(cfg());
        b.set_symbol(sized_symbol());
        std::uint64_t ts = 1'000;
        b.on_quote(0, 100, 110, 500, 500, ts);
        (void)fill(b, market(0, Side::buy, 10, 1), ts, 100, 110);
        (void)fill(b, market(0, Side::sell, 4, 2), ts, 100, 110);
        check(b.positions().open_count() == 2, "hedging_plain_orders_still_coexist");
        check(b.account().realized_pnl_minor == 0, "hedging_plain_orders_no_realisation");
    }

    // ---- realized PnL includes spread and commission ------------------------
    {
        auto spec = sized_symbol();
        spec.commission_per_lot_minor = 700;
        spec.contract_size = 100;          // 7 minor per lot-step
        exec::PaperBroker b(cfg());
        b.set_symbol(spec);
        std::uint64_t ts = 1'000;
        b.on_quote(0, 100, 110, 500, 500, ts);
        (void)fill(b, market(0, Side::buy, 10, 1), ts, 100, 110);   // pays ask 110
        b.on_quote(0, 100, 110, 500, 500, ++ts);
        (void)fill(b, reduce(0, Side::sell, 10, 2), ts, 100, 110);  // receives bid 100

        // Round trip at an unchanged quote loses exactly the spread: 10 ticks.
        check(b.account().realized_pnl_minor == -100, "spread_cost_realised");
        check(b.account().commission_paid_minor == 140, "commission_both_legs");
        check(b.account().balance_minor == 100'000'000 - 100 - 140, "balance_net_of_costs");
        check(b.positions().net_volume(0) == 0, "cost_test_flat");
    }

    return failures == 0 ? 0 : 1;
}
