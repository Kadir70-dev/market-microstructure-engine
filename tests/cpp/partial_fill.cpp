#include "exec_fixture.hpp"

using namespace exec_fixture;
using exec::Side;
using exec::OrderState;

int main() {
    // Available size below requested volume must produce a partial fill with
    // exact remaining accounting (Part 9.2), not an over-fill and not a silent
    // full fill at the touch.
    exec::PaperBroker broker(config());
    broker.set_symbol(sized_symbol());
    broker.on_quote(0, 100, 110, 500, 30, 1'000);   // only 30 offered

    const auto ref = broker.submit(market(0, Side::buy, 100, 1), 1'100);
    check(ref.accepted, "partial_accepted");
    const auto effective = broker.find_order(ref.ref)->ts_effective_ns;

    broker.on_quote(0, 100, 110, 500, 30, effective + 1);
    const auto* order = broker.find_order(ref.ref);
    check(order->filled_volume == 30, "partial_first_fill_capped_by_available");
    check(order->remaining() == 70, "partial_remaining_exact");
    check(order->state == OrderState::partially_filled, "partial_state");

    broker.on_quote(0, 100, 110, 500, 25, effective + 2);
    order = broker.find_order(ref.ref);
    check(order->filled_volume == 55, "partial_second_fill_accumulates");
    check(order->remaining() == 45, "partial_remaining_after_second");

    // Enough size to complete: fills exactly the remainder, never more.
    broker.on_quote(0, 100, 110, 500, 1'000, effective + 3);
    order = broker.find_order(ref.ref);
    check(order->filled_volume == 100, "partial_completes_exactly");
    check(order->remaining() == 0, "partial_no_overfill");
    check(order->state == OrderState::filled, "partial_final_state");

    // Sum of the journalled fill legs must equal the order total.
    std::int64_t summed = 0;
    int fill_records = 0;
    for (std::size_t i = 0; i < broker.journal().size(); ++i) {
        const auto& record = broker.journal().at(i);
        if (record.type == static_cast<std::uint16_t>(exec::JournalRecordType::fill)) {
            summed += record.b;
            ++fill_records;
        }
    }
    check(summed == 100, "partial_journal_legs_sum_to_total");
    check(fill_records == 3, "partial_three_legs_journalled");

    // Weighted average price must be a true weighted mean of the legs, all at
    // the same touch here, so exactly the ask.
    check(order->avg_fill_price_ticks >= 110, "partial_avg_price_at_or_worse_than_ask");
    check(!broker.halted(), "partial_no_halt");
    return failures == 0 ? 0 : 1;
}
