#include <iostream>

#include "book/order_book.hpp"
#include "core/event.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}
using book::DeltaAction;
using book::Side;
}

int main() {
    book::OrderBook order_book(11);
    order_book.apply_delta(Side::bid, 100, 10, DeltaAction::add, 1);
    order_book.apply_delta(Side::ask, 110, 10, DeltaAction::add, 2);

    // Removing a level we never held means our view has diverged from the
    // venue's. Phase 3 requires a rebase, not a silent no-op that would leave
    // the book quietly wrong for the rest of the session.
    const auto status = order_book.apply_delta(Side::bid, 105, 0, DeltaAction::remove, 3);
    check(status == book::BookStatus::rebase_required, "rebase_on_unknown_removal");
    check(order_book.rebases() == 1, "rebase_counted");

    // Bounded cost: the book is emptied, not repaired by traversal.
    check(order_book.depth(Side::bid) == 0 && order_book.depth(Side::ask) == 0,
          "rebase_clears_book");
    check(!order_book.has_both_sides(), "rebase_leaves_book_unusable_until_refilled");

    core::BookRebasePayload payload{};
    check(order_book.consume_rebase(payload), "rebase_emits_event");
    check(payload.symbol_id == 11, "rebase_event_symbol_id");
    check(payload.new_ref == 105, "rebase_event_new_ref");

    // Exactly one event per rebase: a caller polling every tick must not see it
    // twice and emit duplicate BookRebaseEvents onto the bus.
    core::BookRebasePayload again{};
    check(!order_book.consume_rebase(again), "rebase_event_drained_once");

    // The book is usable again once refilled, and ref values advance.
    order_book.apply_delta(Side::bid, 200, 5, DeltaAction::add, 4);
    order_book.apply_delta(Side::ask, 210, 5, DeltaAction::add, 5);
    check(order_book.has_both_sides(), "rebase_recovers_after_refill");

    order_book.apply_delta(Side::bid, 999, 0, DeltaAction::remove, 6);
    core::BookRebasePayload second{};
    check(order_book.consume_rebase(second), "rebase_second_event");
    check(second.old_ref == 105 && second.new_ref == 999, "rebase_ref_transition");

    return failures == 0 ? 0 : 1;
}
