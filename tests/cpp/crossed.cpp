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
    book::CrossedTolerance tolerance{};
    tolerance.max_consecutive_updates = 3;
    tolerance.max_duration_ns = 1'000'000;   // 1 ms

    {
        // A momentarily crossed book is normal in fast markets. Part 21 requires
        // a tolerance window: halting on the first occurrence is a false positive.
        book::OrderBook order_book(1, limits, tolerance);
        check(order_book.apply_quote(110, 100, 1, 1, 0) == book::BookStatus::crossed_tolerated,
              "crossed_first_is_tolerated_not_halt");
        check(order_book.is_crossed(), "crossed_detected");
        check(order_book.apply_quote(110, 100, 1, 1, 1) == book::BookStatus::crossed_tolerated,
              "crossed_second_tolerated");
        check(order_book.apply_quote(110, 100, 1, 1, 2) == book::BookStatus::crossed_tolerated,
              "crossed_third_tolerated");
        // Fourth exceeds max_consecutive_updates = 3.
        check(order_book.apply_quote(110, 100, 1, 1, 3) == book::BookStatus::crossed_halt,
              "crossed_exhausts_update_budget");
    }

    {
        // Uncrossing must reset the window, otherwise unrelated crossings hours
        // apart would accumulate into a spurious halt.
        book::OrderBook order_book(2, limits, tolerance);
        check(order_book.apply_quote(110, 100, 1, 1, 0) == book::BookStatus::crossed_tolerated,
              "reset_crossed_once");
        check(order_book.apply_quote(100, 110, 1, 1, 1) == book::BookStatus::ok,
              "reset_uncrossed");
        check(!order_book.is_crossed(), "reset_not_crossed_now");
        check(order_book.apply_quote(110, 100, 1, 1, 2) == book::BookStatus::crossed_tolerated,
              "reset_window_restarts_tolerated");
        check(order_book.apply_quote(110, 100, 1, 1, 3) == book::BookStatus::crossed_tolerated,
              "reset_window_counts_from_zero");
    }

    {
        // Duration is an independent trip: few updates, but crossed too long.
        book::OrderBook order_book(3, limits, tolerance);
        check(order_book.apply_quote(110, 100, 1, 1, 1'000'000) == book::BookStatus::crossed_tolerated,
              "duration_first_tolerated");
        check(order_book.apply_quote(110, 100, 1, 1, 3'000'000) == book::BookStatus::crossed_halt,
              "duration_exceeded_halts");
    }

    {
        // Touching (bid == ask) counts as crossed: it is not a tradable book.
        book::OrderBook order_book(4, limits, tolerance);
        check(order_book.apply_quote(100, 100, 1, 1, 0) == book::BookStatus::crossed_tolerated,
              "locked_book_treated_as_crossed");
    }

    return failures == 0 ? 0 : 1;
}
