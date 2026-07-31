#include "exec_fixture.hpp"

using namespace exec_fixture;
using exec::Side;

int main() {
    // ---- required margin arithmetic ---------------------------------------
    {
        exec::SymbolSpec spec = sized_symbol();
        spec.tick_value_minor = 1;
        spec.margin_rate_bp = 100;            // 1%
        // notional = volume * price * tick_value = 10 * 1000 * 1 = 10000
        // margin   = 1% of 10000 = 100
        check(exec::MarginModel::required_minor(spec, 10, 1'000) == 100, "margin_one_percent");
        check(exec::MarginModel::required_minor(spec, 0, 1'000) == 0, "margin_zero_volume");
        check(exec::MarginModel::required_minor(spec, 10, 0) == 0, "margin_zero_price");
        // Rounds up: never under-reserve, because an under-reservation lets an
        // order through that the account cannot actually support.
        spec.margin_rate_bp = 1;
        check(exec::MarginModel::required_minor(spec, 1, 1) == 1, "margin_rounds_up");
    }

    // ---- submission is refused when free margin is insufficient ------------
    {
        auto cfg = config();
        cfg.initial_balance_minor = 50;       // tiny account
        exec::PaperBroker broker(cfg);
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 1'000, 1'010, 500, 500, 1'000);

        // margin for 10 @ 1010 at 1% = 101 > 50 free.
        const auto result = broker.submit(market(0, Side::buy, 10, 1), 1'100);
        check(!result.accepted, "margin_insufficient_rejected");
        check(result.reason == exec::RejectReason::insufficient_margin, "margin_reject_reason");
        check(broker.account().open_orders == 0, "margin_reject_reserves_nothing");
        check(broker.find_order(result.ref) == nullptr, "margin_reject_not_tracked");

        // Fail-closed rejections are journalled, so a refusal is auditable.
        bool journalled = false;
        for (std::size_t i = 0; i < broker.journal().size(); ++i)
            if (broker.journal().at(i).type ==
                static_cast<std::uint16_t>(exec::JournalRecordType::rejection) &&
                broker.journal().at(i).a ==
                static_cast<std::int64_t>(exec::RejectReason::insufficient_margin))
                journalled = true;
        check(journalled, "margin_rejection_journalled");
    }

    // ---- an affordable order on the same account is accepted ---------------
    {
        auto cfg = config();
        cfg.initial_balance_minor = 500;
        exec::PaperBroker broker(cfg);
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 1'000, 1'010, 500, 500, 1'000);
        check(broker.submit(market(0, Side::buy, 10, 2), 1'100).accepted, "margin_affordable_accepted");
    }

    // ---- stop-out is a first-class simulated outcome (Part 9.2) -------------
    {
        exec::AccountState account{};
        account.equity_minor = 400;
        account.margin_used_minor = 1'000;    // level = 40%
        check(exec::MarginModel::stop_out(account, 5'000), "margin_stop_out_below_level");
        account.equity_minor = 600;           // level = 60%
        check(!exec::MarginModel::stop_out(account, 5'000), "margin_no_stop_out_above_level");
        account.margin_used_minor = 0;
        check(!exec::MarginModel::stop_out(account, 5'000), "margin_no_stop_out_without_margin");
    }

    return failures == 0 ? 0 : 1;
}
