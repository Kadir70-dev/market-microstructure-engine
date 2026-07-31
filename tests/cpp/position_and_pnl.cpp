#include "exec_fixture.hpp"

using namespace exec_fixture;
using exec::Side;

// All accounting is integer minor units. Every figure below is hand-computable,
// which is the point: a PnL you cannot verify by hand is a PnL you cannot trust.

int main() {
    // ---- pure PnL arithmetic ----------------------------------------------
    {
        exec::SymbolSpec spec = sized_symbol();
        spec.tick_value_minor = 2;

        exec::Position long_position{};
        long_position.side = Side::buy;
        long_position.volume = 10;
        long_position.avg_price_ticks = 100;
        // (120 - 100) * 10 * 2 = 400
        check(exec::position_pnl_minor(long_position, spec, 120) == 400, "pnl_long_gain");
        check(exec::position_pnl_minor(long_position, spec, 90) == -200, "pnl_long_loss");
        check(exec::position_pnl_minor(long_position, spec, 100) == 0, "pnl_long_flat");

        exec::Position short_position = long_position;
        short_position.side = Side::sell;
        check(exec::position_pnl_minor(short_position, spec, 120) == -400, "pnl_short_loss");
        check(exec::position_pnl_minor(short_position, spec, 90) == 200, "pnl_short_gain");
    }

    // ---- position opens from a fill and marks to market --------------------
    {
        exec::PaperBroker broker(config(exec::SimulationMode::optimistic));
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 500, 1'000);

        const auto ref = broker.submit(market(0, Side::buy, 10, 1), 1'100);
        const auto effective = broker.find_order(ref.ref)->ts_effective_ns;
        broker.on_quote(0, 100, 110, 500, 500, effective + 1);

        check(broker.positions().open_count() == 1, "pnl_position_opened");
        check(broker.account().open_positions == 1, "pnl_account_position_count");
        const auto& position = broker.positions().at(0);
        check(position.volume == 10, "pnl_position_volume");
        check(position.avg_price_ticks == 110, "pnl_position_entry_at_ask");
        check(position.side == Side::buy, "pnl_position_side");

        // Long is marked at the bid: 100 vs entry 110 = -10 ticks * 10 = -100.
        check(broker.account().unrealized_pnl_minor == -100, "pnl_unrealized_marked_at_bid");
        check(broker.account().equity_minor ==
              broker.account().balance_minor + broker.account().unrealized_pnl_minor,
              "pnl_equity_is_balance_plus_unrealized");
        check(broker.account().free_margin_minor ==
              broker.account().equity_minor - broker.account().margin_used_minor,
              "pnl_free_margin_identity");

        // Market moves up: unrealized follows deterministically.
        broker.on_quote(0, 130, 140, 500, 500, effective + 2);
        check(broker.account().unrealized_pnl_minor == 200, "pnl_unrealized_tracks_market");
    }

    // ---- netting realises PnL on the closing leg ---------------------------
    {
        exec::PaperBroker broker(config(exec::SimulationMode::optimistic, /*hedging=*/false));
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 500, 1'000);

        const auto buy = broker.submit(market(0, Side::buy, 10, 1), 1'100);
        auto effective = broker.find_order(buy.ref)->ts_effective_ns;
        broker.on_quote(0, 100, 110, 500, 500, effective + 1);   // long 10 @ 110

        broker.on_quote(0, 200, 210, 500, 500, effective + 2);
        const auto sell = broker.submit(market(0, Side::sell, 10, 2), effective + 3);
        effective = broker.find_order(sell.ref)->ts_effective_ns;
        broker.on_quote(0, 200, 210, 500, 500, effective + 1);   // close at bid 200

        // Realised = (200 - 110) * 10 * 1 = 900.
        check(broker.account().realized_pnl_minor == 900, "pnl_netting_realised");
        check(broker.positions().open_count() == 0, "pnl_netting_position_closed");
        check(broker.account().unrealized_pnl_minor == 0, "pnl_netting_no_residual_unrealized");
        check(broker.account().balance_minor == 100'000'000 + 900, "pnl_netting_balance_updated");
    }

    // ---- journal carries the account and PnL trail -------------------------
    {
        exec::PaperBroker broker(config(exec::SimulationMode::optimistic));
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 500, 1'000);
        const auto ref = broker.submit(market(0, Side::buy, 10, 1), 1'100);
        const auto effective = broker.find_order(ref.ref)->ts_effective_ns;
        broker.on_quote(0, 100, 110, 500, 500, effective + 1);

        int account_records = 0, pnl_records = 0, position_records = 0;
        for (std::size_t i = 0; i < broker.journal().size(); ++i) {
            const auto type = broker.journal().at(i).type;
            if (type == static_cast<std::uint16_t>(exec::JournalRecordType::account_update))
                ++account_records;
            if (type == static_cast<std::uint16_t>(exec::JournalRecordType::pnl_update))
                ++pnl_records;
            if (type == static_cast<std::uint16_t>(exec::JournalRecordType::position_update))
                ++position_records;
        }
        check(account_records > 0, "pnl_account_updates_journalled");
        check(pnl_records > 0, "pnl_updates_journalled");
        check(position_records > 0, "pnl_position_updates_journalled");
    }

    return failures == 0 ? 0 : 1;
}
