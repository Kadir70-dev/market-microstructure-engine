#include "exec_fixture.hpp"

using namespace exec_fixture;
using exec::Side;

// Part 8.1: positions are keyed by position_ticket with a derived per-symbol
// aggregate, and that single representation must serve both margin modes.
// Modelling net-only produces the permanent false reconciliation mismatch on
// hedging accounts that Part 8.1 exists to prevent — and this broker is hedging.

int main() {
    // ---- hedging: opposing exposure coexists as distinct tickets -----------
    {
        exec::PaperBroker broker(config(exec::SimulationMode::optimistic, /*hedging=*/true));
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 500, 1'000);

        const auto buy = broker.submit(market(0, Side::buy, 10, 1), 1'100);
        auto effective = broker.find_order(buy.ref)->ts_effective_ns;
        broker.on_quote(0, 100, 110, 500, 500, effective + 1);

        const auto sell = broker.submit(market(0, Side::sell, 4, 2), effective + 2);
        effective = broker.find_order(sell.ref)->ts_effective_ns;
        broker.on_quote(0, 100, 110, 500, 500, effective + 1);

        check(broker.positions().open_count() == 2, "hedging_two_tickets_coexist");
        check(broker.positions().at(0).position_ticket != broker.positions().at(1).position_ticket,
              "hedging_distinct_tickets");
        check(broker.positions().at(0).side != broker.positions().at(1).side,
              "hedging_opposing_sides_held");

        // Nothing was netted away: both legs keep their own volume.
        check(broker.positions().at(0).volume == 10, "hedging_long_leg_intact");
        check(broker.positions().at(1).volume == 4, "hedging_short_leg_intact");

        // The per-symbol aggregate is derived, not stored: 10 long - 4 short = 6.
        check(broker.positions().net_volume(0) == 6, "hedging_derived_net_aggregate");

        // No realised PnL: nothing was closed.
        check(broker.account().realized_pnl_minor == 0, "hedging_no_realisation_on_opposing_open");
    }

    // ---- netting: the same sequence nets to one signed position ------------
    {
        exec::PaperBroker broker(config(exec::SimulationMode::optimistic, /*hedging=*/false));
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 500, 1'000);

        const auto buy = broker.submit(market(0, Side::buy, 10, 1), 1'100);
        auto effective = broker.find_order(buy.ref)->ts_effective_ns;
        broker.on_quote(0, 100, 110, 500, 500, effective + 1);

        const auto sell = broker.submit(market(0, Side::sell, 4, 2), effective + 2);
        effective = broker.find_order(sell.ref)->ts_effective_ns;
        broker.on_quote(0, 100, 110, 500, 500, effective + 1);

        check(broker.positions().open_count() == 1, "netting_single_position");
        check(broker.positions().net_volume(0) == 6, "netting_reduced_to_six");
        // Closing 4 at bid 100 against entry 110: (100-110)*4 = -40.
        check(broker.account().realized_pnl_minor == -40, "netting_realises_on_reduction");
    }

    // ---- hedging: same-side fills accumulate into one ticket ---------------
    {
        exec::PaperBroker broker(config(exec::SimulationMode::optimistic, /*hedging=*/true));
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 20, 1'000);

        const auto ref = broker.submit(market(0, Side::buy, 40, 1), 1'100);
        const auto effective = broker.find_order(ref.ref)->ts_effective_ns;
        broker.on_quote(0, 100, 110, 500, 20, effective + 1);    // partial 20
        broker.on_quote(0, 100, 120, 500, 20, effective + 2);    // partial 20 at worse ask

        const auto* order = broker.find_order(ref.ref);
        check(order->filled_volume == 40, "hedging_partials_completed");
        check(broker.positions().open_count() == 1, "hedging_one_ticket_per_order");
        const auto& position = broker.positions().at(0);
        check(position.volume == 40, "hedging_ticket_accumulated_volume");
        // Weighted mean of 20@110 and 20@120 is 115.
        check(position.avg_price_ticks == 115, "hedging_weighted_average_entry");
    }

    // ---- position book capacity is bounded and fails closed ---------------
    {
        exec::PositionBook book;
        std::size_t opened = 0;
        for (std::size_t i = 0; i < exec::max_positions + 8; ++i)
            if (book.open(i + 1, 0, Side::buy, 1, 100, 0) != nullptr) ++opened;
        check(opened == exec::max_positions, "hedging_position_book_bounded");
        check(book.open(9'999, 0, Side::buy, 1, 100, 0) == nullptr, "hedging_book_refuses_when_full");
    }

    return failures == 0 ? 0 : 1;
}
