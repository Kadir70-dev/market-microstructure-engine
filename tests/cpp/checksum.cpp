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

void fill(book::OrderBook& order_book, const std::int64_t* prices, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        order_book.apply_delta(Side::bid, prices[i], 10 + static_cast<std::int64_t>(i),
                               DeltaAction::add, 1);
}
}

int main() {
    // Insertion order must not matter: the checksum is over the sorted ladder,
    // so two feeds delivering the same book in different orders must agree.
    // Sizes are keyed to price, not arrival index, or the comparison is vacuous.
    book::OrderBook a(5), b(5);
    a.apply_delta(Side::bid, 100, 10, DeltaAction::add, 1);
    a.apply_delta(Side::bid, 101, 11, DeltaAction::add, 1);
    a.apply_delta(Side::bid, 102, 12, DeltaAction::add, 1);
    b.apply_delta(Side::bid, 102, 12, DeltaAction::add, 1);
    b.apply_delta(Side::bid, 100, 10, DeltaAction::add, 1);
    b.apply_delta(Side::bid, 101, 11, DeltaAction::add, 1);
    check(a.checksum() == b.checksum(), "checksum_insertion_order_independent");

    // Determinism: recomputing without mutation yields the identical value.
    check(a.checksum() == a.checksum(), "checksum_stable_across_calls");

    // Sensitivity: size, price, depth and symbol all participate.
    book::OrderBook c(5);
    c.apply_delta(Side::bid, 100, 10, DeltaAction::add, 1);
    c.apply_delta(Side::bid, 101, 11, DeltaAction::add, 1);
    c.apply_delta(Side::bid, 102, 99, DeltaAction::add, 1);
    check(a.checksum() != c.checksum(), "checksum_detects_size_change");

    book::OrderBook d(5);
    d.apply_delta(Side::bid, 100, 10, DeltaAction::add, 1);
    d.apply_delta(Side::bid, 101, 11, DeltaAction::add, 1);
    d.apply_delta(Side::bid, 103, 12, DeltaAction::add, 1);
    check(a.checksum() != d.checksum(), "checksum_detects_price_change");

    book::OrderBook e(5);
    e.apply_delta(Side::bid, 100, 10, DeltaAction::add, 1);
    e.apply_delta(Side::bid, 101, 11, DeltaAction::add, 1);
    check(a.checksum() != e.checksum(), "checksum_detects_depth_change");

    book::OrderBook f(6);   // same ladder, different symbol
    f.apply_delta(Side::bid, 100, 10, DeltaAction::add, 1);
    f.apply_delta(Side::bid, 101, 11, DeltaAction::add, 1);
    f.apply_delta(Side::bid, 102, 12, DeltaAction::add, 1);
    check(a.checksum() != f.checksum(), "checksum_detects_symbol_change");

    // A bid ladder and the identical ask ladder are different books.
    book::OrderBook g(5), h(5);
    const std::int64_t prices[] = {100, 101, 102};
    fill(g, prices, 3);
    for (std::size_t i = 0; i < 3; ++i)
        h.apply_delta(Side::ask, prices[i], 10 + static_cast<std::int64_t>(i),
                      DeltaAction::add, 1);
    check(g.checksum() != h.checksum(), "checksum_side_sensitive");

    // Round trip: adding then removing returns to the empty checksum.
    book::OrderBook i(5);
    const auto empty = i.checksum();
    i.apply_delta(Side::bid, 100, 10, DeltaAction::add, 1);
    check(i.checksum() != empty, "checksum_changes_on_add");
    i.apply_delta(Side::bid, 100, 0, DeltaAction::remove, 2);
    check(i.checksum() == empty, "checksum_round_trips_to_empty");

    return failures == 0 ? 0 : 1;
}
