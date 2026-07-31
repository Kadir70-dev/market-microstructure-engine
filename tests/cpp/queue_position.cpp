#include "exec_fixture.hpp"

using namespace exec_fixture;
using exec::Side;

// Part 9.2: queue_ahead = size_at_level at placement, decremented by an
// estimated share of depth reductions, fill only at queue_ahead <= 0.
//
// Part 9.2 also mandates that on L1_ONLY / DOM_* sources these results are
// tagged NOT VALIDATED and are inadmissible for promotion. That tag is asserted
// here as a first-class property, because a queue model with no real queue
// behind it is a plausible-looking number with nothing underneath it.

int main() {
    // ---- placement sets queue_ahead from displayed size ---------------------
    {
        // Pessimistic = back of queue: the whole displayed size is ahead of us.
        exec::PaperBroker broker(config(exec::SimulationMode::pessimistic));
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 400, 400, 1'000);
        const auto ref = broker.submit(limit(0, Side::buy, 10, 100, 1), 1'100);
        check(ref.accepted, "queue_accepted");
        check(broker.find_order(ref.ref)->queue_ahead == 400, "queue_back_of_book_full_size");
    }
    {
        // Optimistic = front of queue: nothing ahead.
        exec::PaperBroker broker(config(exec::SimulationMode::optimistic));
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 400, 400, 1'000);
        const auto ref = broker.submit(limit(0, Side::buy, 10, 100, 2), 1'100);
        check(broker.find_order(ref.ref)->queue_ahead == 0, "queue_front_of_book_zero");
    }
    {
        // Base = mid queue.
        exec::PaperBroker broker(config(exec::SimulationMode::base));
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 400, 400, 1'000);
        const auto ref = broker.submit(limit(0, Side::buy, 10, 100, 3), 1'100);
        check(broker.find_order(ref.ref)->queue_ahead == 200, "queue_mid_book_half_size");
    }

    // ---- queued order does not fill while others are ahead ------------------
    {
        exec::PaperBroker broker(config(exec::SimulationMode::pessimistic));
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 400, 400, 1'000);
        const auto ref = broker.submit(limit(0, Side::buy, 10, 100, 4), 1'100);
        const auto effective = broker.find_order(ref.ref)->ts_effective_ns;

        // Market trades through our level, but we are behind 400 lots.
        broker.on_quote(0, 100, 99, 400, 400, effective + 1);
        check(broker.find_order(ref.ref)->queue_ahead > 0, "queue_still_ahead");
        check(broker.find_order(ref.ref)->filled_volume == 0, "queue_blocks_fill_while_ahead");
    }

    // ---- depth reductions credit the queue ---------------------------------
    {
        exec::PaperBroker broker(config(exec::SimulationMode::pessimistic));
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 400, 400, 1'000);
        const auto ref = broker.submit(limit(0, Side::buy, 10, 100, 5), 1'100);
        const auto initial = broker.find_order(ref.ref)->queue_ahead;

        // Same level, size falls: part of that reduction is trading ahead of us.
        broker.on_quote(0, 100, 110, 100, 400, 1'200);
        const auto credited = broker.find_order(ref.ref)->queue_ahead;
        check(credited < initial, "queue_credited_by_depth_reduction");
        check(credited >= 0, "queue_never_negative");

        // Credit accrues only on genuine reductions, so draining requires
        // repeated refill-and-trade cycles at the same level — exactly how a
        // real queue empties. A flat sequence of identical sizes credits
        // nothing, which is the correct behaviour and not a stall.
        std::uint64_t ts = 1'300;
        for (int cycle = 0; cycle < 40; ++cycle) {
            broker.on_quote(0, 100, 110, 400, 400, ts++);   // refill: no credit
            broker.on_quote(0, 100, 110, 1, 400, ts++);     // traded down: credit
        }
        check(broker.find_order(ref.ref)->queue_ahead == 0, "queue_fully_drained");

        // A flat market must not advance the queue at all.
        exec::PaperBroker flat(config(exec::SimulationMode::pessimistic));
        flat.set_symbol(sized_symbol());
        flat.on_quote(0, 100, 110, 400, 400, 1'000);
        const auto flat_ref = flat.submit(limit(0, Side::buy, 10, 100, 7), 1'100);
        const auto before_flat = flat.find_order(flat_ref.ref)->queue_ahead;
        for (int i = 0; i < 10; ++i)
            flat.on_quote(0, 100, 110, 400, 400, 1'200 + static_cast<std::uint64_t>(i));
        check(flat.find_order(flat_ref.ref)->queue_ahead == before_flat,
              "queue_unchanged_without_reduction");
    }

    // ---- NOT_VALIDATED tagging on L1 sources -------------------------------
    {
        auto cfg = config(exec::SimulationMode::pessimistic);
        cfg.queue_validated = false;                 // L1_ONLY feed
        exec::PaperBroker l1(cfg);
        check(!l1.queue_results_validated(), "queue_l1_tagged_not_validated");

        auto validated = config(exec::SimulationMode::pessimistic);
        validated.queue_validated = true;            // L2_EXCHANGE feed
        exec::PaperBroker l2(validated);
        check(l2.queue_results_validated(), "queue_l2_tagged_validated");

        exec::PaperBroker probe(cfg);
        probe.set_symbol(sized_symbol());
        probe.on_quote(0, 100, 110, 400, 400, 1'000);
        const auto ref = probe.submit(limit(0, Side::buy, 10, 100, 6), 1'100);
        check(!probe.find_order(ref.ref)->queue_validated,
              "queue_order_carries_not_validated_flag");
    }

    return failures == 0 ? 0 : 1;
}
