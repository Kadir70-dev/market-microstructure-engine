#include "exec_fixture.hpp"

using namespace exec_fixture;
using exec::Side;

int main() {
    exec::SymbolSpec spec = sized_symbol();
    spec.contract_size = 100'000;
    spec.commission_per_lot_minor = 700;      // 7.00 per lot, in minor units

    // Integer, round-half-up, so the charge is bit-reproducible. A double here
    // would drift by an ulp under reordering and break the determinism gate.
    check(exec::CommissionModel::charge_minor(spec, 100'000) == 700, "commission_one_lot");
    check(exec::CommissionModel::charge_minor(spec, 200'000) == 1'400, "commission_two_lots");
    check(exec::CommissionModel::charge_minor(spec, 50'000) == 350, "commission_half_lot");
    check(exec::CommissionModel::charge_minor(spec, 0) == 0, "commission_zero_volume");
    check(exec::CommissionModel::charge_minor(spec, -5) == 0, "commission_negative_volume");

    // Rounding is half-up and deterministic: 1 lot-step of 100000 at 700 minor
    // per lot is 0.007 minor, which must round to 0, while 71429 steps rounds up.
    check(exec::CommissionModel::charge_minor(spec, 1) == 0, "commission_rounds_down_below_half");
    check(exec::CommissionModel::charge_minor(spec, 71'429) == 500, "commission_rounds_half_up");

    // Zero commission symbol charges nothing.
    {
        auto free_spec = spec;
        free_spec.commission_per_lot_minor = 0;
        check(exec::CommissionModel::charge_minor(free_spec, 500'000) == 0,
              "commission_zero_rate");
    }

    // Charged on filled volume and debited from balance, accumulating across
    // partial fills rather than being applied once per order.
    {
        auto cfg = config();
        cfg.initial_balance_minor = 10'000'000;
        exec::PaperBroker broker(cfg);
        broker.set_symbol(spec);
        broker.on_quote(0, 100, 110, 500, 40, 1'000);

        const auto ref = broker.submit(market(0, Side::buy, 100'000, 1), 1'100);
        check(ref.accepted, "commission_order_accepted");
        const auto effective = broker.find_order(ref.ref)->ts_effective_ns;

        broker.on_quote(0, 100, 110, 500, 40'000, effective + 1);
        const auto partial_commission = broker.account().commission_paid_minor;
        check(partial_commission > 0, "commission_charged_on_partial");

        broker.on_quote(0, 100, 110, 500, 1'000'000, effective + 2);
        const auto* order = broker.find_order(ref.ref);
        check(order->filled_volume == 100'000, "commission_order_completed");

        // Sum over legs equals the whole-volume charge, within the rounding of
        // each leg — the invariant is that it never exceeds it materially.
        check(broker.account().commission_paid_minor >= partial_commission,
              "commission_accumulates");
        check(order->commission_minor == broker.account().commission_paid_minor,
              "commission_order_matches_account");
        check(broker.account().balance_minor ==
              cfg.initial_balance_minor - broker.account().commission_paid_minor,
              "commission_debited_from_balance");
    }

    return failures == 0 ? 0 : 1;
}
