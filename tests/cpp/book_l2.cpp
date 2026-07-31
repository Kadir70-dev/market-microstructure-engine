#include <iostream>

#include "book/order_book.hpp"

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
    book::OrderBook order_book(3);

    // Insert out of order; the ladder must sort itself.
    order_book.apply_delta(Side::bid, 100, 10, DeltaAction::add, 1);
    order_book.apply_delta(Side::bid, 102, 20, DeltaAction::add, 2);
    order_book.apply_delta(Side::bid, 101, 30, DeltaAction::add, 3);
    check(order_book.depth(Side::bid) == 3, "l2_bid_depth");
    check(order_book.level(Side::bid, 0).price_ticks == 102 &&
          order_book.level(Side::bid, 1).price_ticks == 101 &&
          order_book.level(Side::bid, 2).price_ticks == 100,
          "l2_bids_sorted_descending");

    order_book.apply_delta(Side::ask, 110, 10, DeltaAction::add, 4);
    order_book.apply_delta(Side::ask, 108, 20, DeltaAction::add, 5);
    order_book.apply_delta(Side::ask, 109, 30, DeltaAction::add, 6);
    check(order_book.level(Side::ask, 0).price_ticks == 108 &&
          order_book.level(Side::ask, 1).price_ticks == 109 &&
          order_book.level(Side::ask, 2).price_ticks == 110,
          "l2_asks_sorted_ascending");

    order_book.apply_delta(Side::bid, 101, 77, DeltaAction::modify, 7);
    check(order_book.level(Side::bid, 1).size == 77, "l2_modify_existing_level");

    order_book.apply_delta(Side::bid, 101, 0, DeltaAction::remove, 8);
    check(order_book.depth(Side::bid) == 2, "l2_remove_level");
    check(order_book.level(Side::bid, 0).price_ticks == 102 &&
          order_book.level(Side::bid, 1).price_ticks == 100,
          "l2_order_preserved_after_remove");

    // A modify-to-zero is a removal whatever the venue calls it.
    order_book.apply_delta(Side::bid, 100, 0, DeltaAction::modify, 9);
    check(order_book.depth(Side::bid) == 1, "l2_modify_to_zero_is_removal");

    // Depth is bounded at max_book_depth with eviction of the worst level.
    book::OrderBook deep(4);
    for (std::size_t i = 0; i < book::max_book_depth; ++i)
        deep.apply_delta(Side::bid, static_cast<std::int64_t>(1000 + i), 1, DeltaAction::add, 10);
    check(deep.depth(Side::bid) == book::max_book_depth, "l2_depth_capped");

    // Better than everything held: inserts at the front, worst falls off.
    deep.apply_delta(Side::bid, 9999, 5, DeltaAction::add, 11);
    check(deep.depth(Side::bid) == book::max_book_depth, "l2_depth_still_capped");
    check(deep.level(Side::bid, 0).price_ticks == 9999, "l2_better_level_inserted");
    check(deep.outside_window() == 0, "l2_insert_not_counted_outside");

    // Worse than everything held on a full book: dropped, and explicitly not a
    // desync — a depth-limited book simply cannot see that far.
    deep.apply_delta(Side::bid, 1, 5, DeltaAction::add, 12);
    check(deep.outside_window() == 1, "l2_worse_level_outside_window");
    check(deep.rebases() == 0, "l2_outside_window_is_not_a_rebase");

    return failures == 0 ? 0 : 1;
}
