#include "exec_fixture.hpp"

using namespace exec_fixture;
using exec::Side;
using exec::OrderState;

int main() {
    // ---- cancel -----------------------------------------------------------
    {
        exec::PaperBroker broker(config());
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 500, 1'000);
        const auto ref = broker.submit(limit(0, Side::buy, 10, 50, 1), 1'100);
        check(ref.accepted, "cancel_order_accepted");

        check(broker.cancel(ref.ref, 2'000), "cancel_succeeds");
        const auto* order = broker.find_order(ref.ref);
        check(order->state == OrderState::cancelled, "cancel_terminal_state");
        check(exec::is_terminal(order->state), "cancel_is_terminal");
        check(order->filled_volume == 0, "cancel_unfilled");

        // Cancelling twice must fail: the order is terminal and its exposure has
        // already been released. A second release would corrupt the reservation.
        check(!broker.cancel(ref.ref, 2'100), "cancel_idempotent_rejects_second");

        // A cancelled order must never fill afterwards.
        broker.on_quote(0, 100, 40, 500, 500, 3'000);
        check(broker.find_order(ref.ref)->filled_volume == 0, "cancel_never_fills_after");
    }

    // ---- replace ----------------------------------------------------------
    {
        exec::PaperBroker broker(config());
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 500, 1'000);
        const auto ref = broker.submit(limit(0, Side::buy, 10, 50, 2), 1'100);
        check(ref.accepted, "replace_order_accepted");

        check(broker.replace(ref.ref, 60, 20, 2'000), "replace_succeeds");
        const auto* order = broker.find_order(ref.ref);
        check(order->limit_price_ticks == 60, "replace_price_applied");
        check(order->requested_volume == 20, "replace_volume_applied");
        // The ref survives a replace so journal linkage is unbroken (Part 8.2).
        check(order->ref == ref.ref, "replace_preserves_broker_order_ref");

        // Reducing volume below what is already filled is incoherent.
        check(!broker.replace(ref.ref, 60, 0, 2'100), "replace_rejects_zero_volume");
        check(!broker.replace(ref.ref, 0, 20, 2'100), "replace_rejects_zero_price");

        // Replace on a terminal order must fail.
        check(broker.cancel(ref.ref, 2'200), "replace_cancel_for_terminal_test");
        check(!broker.replace(ref.ref, 70, 20, 2'300), "replace_rejects_terminal_order");
    }

    // ---- journal linkage --------------------------------------------------
    {
        exec::PaperBroker broker(config());
        broker.set_symbol(sized_symbol());
        broker.on_quote(0, 100, 110, 500, 500, 1'000);
        const auto ref = broker.submit(limit(0, Side::buy, 10, 50, 3), 1'100);
        (void)broker.replace(ref.ref, 60, 20, 2'000);
        (void)broker.cancel(ref.ref, 3'000);

        int replace_records = 0, cancel_records = 0;
        for (std::size_t i = 0; i < broker.journal().size(); ++i) {
            const auto& record = broker.journal().at(i);
            if (record.type == static_cast<std::uint16_t>(exec::JournalRecordType::replace)) {
                ++replace_records;
                check(record.logical_order_id == ref.ref.logical_order_id,
                      "replace_journal_linked_to_order");
                check(record.a == 50 && record.b == 60, "replace_journal_old_and_new_price");
            }
            if (record.type == static_cast<std::uint16_t>(exec::JournalRecordType::cancel))
                ++cancel_records;
        }
        check(replace_records == 1, "replace_journalled_once");
        check(cancel_records == 1, "cancel_journalled_once");
    }

    return failures == 0 ? 0 : 1;
}
