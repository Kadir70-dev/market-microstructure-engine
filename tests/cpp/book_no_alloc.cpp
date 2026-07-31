#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>

#include "book/order_book.hpp"
#include "features/feature_engine.hpp"

// Part 18 Phase 3: "no std::map, no allocation, no strings on the hot path —
// enforced by test". Absence of std::map and strings is a compile-time property
// of the headers; absence of allocation is not, so it is asserted at runtime by
// hooking the global allocator across a representative update storm.

namespace {
std::atomic<std::size_t> allocations{0};
std::atomic<bool> tracking{false};
}

void* operator new(std::size_t size) {
    if (tracking.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* value = std::malloc(size)) return value;
    throw std::bad_alloc{};
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }

int main() {
    book::OrderBook order_book(1);
    features::FeatureEngine engine(32);
    core::BookRebasePayload rebase{};

    tracking.store(true, std::memory_order_relaxed);
    for (std::int64_t i = 0; i < 200'000; ++i) {
        // L1 path.
        order_book.apply_quote(100 + (i % 50), 110 + (i % 50), 1 + (i % 7), 1 + (i % 5),
                               static_cast<std::uint64_t>(i));
        (void)engine.compute(order_book, book::BookSource::l1_only);

        // L2 path, including insertion, modification, eviction and the rebase
        // branch, so no code path escapes the measurement.
        order_book.apply_delta(book::Side::bid, 100 + (i % 40), 5, book::DeltaAction::add,
                               static_cast<std::uint64_t>(i));
        order_book.apply_delta(book::Side::ask, 200 + (i % 40), 5, book::DeltaAction::add,
                               static_cast<std::uint64_t>(i));
        order_book.apply_delta(book::Side::bid, 100 + (i % 40), 9, book::DeltaAction::modify,
                               static_cast<std::uint64_t>(i));
        order_book.apply_delta(book::Side::bid, 100 + (i % 40), 0, book::DeltaAction::remove,
                               static_cast<std::uint64_t>(i));
        if ((i % 1000) == 0) {
            order_book.apply_delta(book::Side::bid, 999'999, 0, book::DeltaAction::remove,
                                   static_cast<std::uint64_t>(i));
            (void)order_book.consume_rebase(rebase);
        }
        (void)engine.compute(order_book, book::BookSource::dom_aggregated);
        (void)order_book.checksum();
    }
    tracking.store(false, std::memory_order_relaxed);

    const auto count = allocations.load(std::memory_order_relaxed);
    std::cout << "book_hot_path_allocations=" << count << '\n';
    if (count != 0) {
        std::cout << "book_no_alloc=FAIL\n";
        return 1;
    }
    std::cout << "book_no_alloc=pass\n";
    return 0;
}
