#include "exec_fixture.hpp"

using namespace exec_fixture;
using exec::Side;

// Part 8.5: "Duplicate prevention: in-flight ref set checked before every send."
// Part 10.1 Invariant 4: "No two orders share a broker_order_ref."
//
// A duplicate that slipped through would double the position while the OMS
// tracked one order — the exposure and the record would disagree permanently.

int main() {
    exec::PaperBroker broker(config());
    broker.set_symbol(sized_symbol());
    broker.on_quote(0, 100, 110, 500, 500, 1'000);

    // logical_order_id = hash(strategy_id, symbol_id, seq_global), so the same
    // triple must produce the same id and therefore collide.
    const auto id_a = exec::make_logical_order_id(1, 0, 5);
    const auto id_b = exec::make_logical_order_id(1, 0, 5);
    check(id_a == id_b, "duplicate_id_deterministic");
    check(exec::make_logical_order_id(1, 0, 6) != id_a, "duplicate_id_varies_with_seq");
    check(exec::make_logical_order_id(2, 0, 5) != id_a, "duplicate_id_varies_with_strategy");
    check(exec::make_logical_order_id(1, 1, 5) != id_a, "duplicate_id_varies_with_symbol");

    auto request = limit(0, Side::buy, 10, 50, 5);
    const auto first = broker.submit(request, 1'100);
    check(first.accepted, "duplicate_first_accepted");

    const auto second = broker.submit(request, 1'200);
    check(!second.accepted, "duplicate_second_rejected");
    check(second.reason == exec::RejectReason::duplicate_order_id, "duplicate_reject_reason");
    check(second.ref == first.ref, "duplicate_same_ref_computed");
    check(broker.account().open_orders == 1, "duplicate_only_one_order_live");

    // A different seq_global is a different order and is accepted.
    const auto distinct = broker.submit(limit(0, Side::buy, 10, 50, 6), 1'300);
    check(distinct.accepted, "duplicate_distinct_seq_accepted");
    check(!(distinct.ref == first.ref), "duplicate_distinct_ref");
    check(broker.account().open_orders == 2, "duplicate_two_orders_live");

    // The rejection is journalled so the collision is auditable.
    bool journalled = false;
    for (std::size_t i = 0; i < broker.journal().size(); ++i) {
        const auto& record = broker.journal().at(i);
        if (record.type == static_cast<std::uint16_t>(exec::JournalRecordType::rejection) &&
            record.a == static_cast<std::int64_t>(exec::RejectReason::duplicate_order_id))
            journalled = true;
    }
    check(journalled, "duplicate_rejection_journalled");

    // broker_order_ref combines run_id with the logical id, so the same logical
    // order in a different run is a different ref (Part 8.2).
    {
        auto other = config();
        other.run_id = 99;
        exec::PaperBroker second_run(other);
        second_run.set_symbol(sized_symbol());
        second_run.on_quote(0, 100, 110, 500, 500, 1'000);
        const auto ref = second_run.submit(request, 1'100);
        check(ref.accepted, "duplicate_other_run_accepted");
        check(ref.ref.logical_order_id == first.ref.logical_order_id,
              "duplicate_logical_id_stable_across_runs");
        check(ref.ref.run_id != first.ref.run_id, "duplicate_run_id_differs");
    }

    // Terminal slots are recyclable: max_live_orders is a concurrent exposure
    // bound, not a lifetime order cap. Historical IDs remain protected after
    // their in-memory slot is reused by the broker's dedicated seen-ref set,
    // which is independent of the journal and so survives journal rotation.
    {
        auto recyclable_cfg = config(exec::SimulationMode::optimistic, /*hedging=*/false);
        exec::PaperBroker recyclable(recyclable_cfg);
        recyclable.set_symbol(sized_symbol());
        std::uint64_t now = 1'000;
        recyclable.on_quote(0, 100, 110, 0, 0, now);
        bool all_accepted = true;
        for (std::uint64_t seq = 0; seq < 300; ++seq) {
            const auto submitted = recyclable.submit(market(0, Side::buy, 1, seq), now);
            all_accepted = all_accepted && submitted.accepted;
            now += 1'000'000;
            recyclable.on_quote(0, 100, 110, 0, 0, now);
        }
        check(all_accepted, "duplicate_terminal_slots_recycled");
        const auto reused = recyclable.submit(market(0, Side::buy, 1, 0), now + 1);
        check(!reused.accepted, "duplicate_recycled_id_rejected");
        check(reused.reason == exec::RejectReason::duplicate_order_id,
              "duplicate_recycled_id_reason");
    }

    return failures == 0 ? 0 : 1;
}
