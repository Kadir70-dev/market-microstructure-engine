#include <iostream>

#include "book/order_book.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}
}

int main() {
    book::SymbolLimits limits{};
    limits.min_price_ticks = 1;
    limits.max_price_ticks = 1'000'000'000;
    limits.max_spread_ticks = 1'000;

    book::OrderBook order_book(7, limits);

    check(order_book.apply_quote(115260, 115268, 0, 0, 1000) == book::BookStatus::ok,
          "l1_quote_accepted");

    // MT5 forex publishes no L1 size. Depth must still be 1 on each side or the
    // entire recorded forex feed would present as an empty book.
    check(order_book.depth(book::Side::bid) == 1 && order_book.depth(book::Side::ask) == 1,
          "l1_zero_volume_still_has_depth");
    check(order_book.has_both_sides(), "l1_has_both_sides");
    check(order_book.best(book::Side::bid).price_ticks == 115260, "l1_best_bid");
    check(order_book.best(book::Side::ask).price_ticks == 115268, "l1_best_ask");
    check(!order_book.is_crossed(), "l1_not_crossed");

    // A quote replaces top of book rather than accumulating levels.
    order_book.apply_quote(115262, 115270, 5, 9, 2000);
    check(order_book.depth(book::Side::bid) == 1, "l1_quote_replaces_not_appends");
    check(order_book.best(book::Side::bid).size == 5, "l1_size_applied");
    check(order_book.best(book::Side::ask).size == 9, "l1_ask_size_applied");

    // SymbolLimits sanity: out of range price and absurd spread both rejected,
    // and a rejected update must not mutate the book.
    const auto before = order_book.checksum();
    check(order_book.apply_quote(0, 115270, 1, 1, 3000) == book::BookStatus::rejected,
          "l1_price_below_min_rejected");
    check(order_book.apply_quote(115262, 2'000'000'000, 1, 1, 3000) == book::BookStatus::rejected,
          "l1_price_above_max_rejected");
    check(order_book.apply_quote(1, 999'999, 1, 1, 3000) == book::BookStatus::rejected,
          "l1_spread_beyond_limit_rejected");
    check(order_book.checksum() == before, "l1_rejected_update_does_not_mutate");
    check(order_book.rejected() == 3, "l1_rejection_counted");

    return failures == 0 ? 0 : 1;
}
