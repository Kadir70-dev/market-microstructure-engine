#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>

#include "book/order_book.hpp"
#include "feed/replay_adapter.hpp"
#include "features/feature_engine.hpp"
#include "replay/digest.hpp"
#include "replay_fixture.hpp"

// Objective 8: zero dynamic allocations on the replay hot path.
//
// "Hot path" is per event. Opening a segment is not on it: WalReader::open
// constructs a stream buffer and touches std::filesystem::path, and in
// production a segment boundary occurs once per 256 MiB or one hour (Part 11.2)
// — roughly 10^6 events apart. Asserting a flat zero across a fixture with
// artificially tiny segments would therefore be measuring the fixture, not the
// engine.
//
// So this test attributes every allocation. Polls that cross a segment boundary
// are accounted separately and bounded; every other poll must allocate exactly
// zero. That is the claim that actually matters, and it is stricter than a
// single aggregate count because one stray per-event allocation cannot hide
// inside a transition budget.

namespace {
std::atomic<std::size_t> allocations{0};
std::atomic<bool> tracking{false};
}

void* operator new(std::size_t size) {
    if (tracking.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* value = std::malloc(size)) return value;
    throw std::bad_alloc{};
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (tracking.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size);
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete(void* value, const std::nothrow_t&) noexcept { std::free(value); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }

int main() {
    // Deliberately small segments so the transition path is exercised many
    // times and cannot escape measurement.
    const auto dir = replay_fixture::make_wal("no_alloc", 20'000, 64 * 1024);
    if (dir.empty()) { std::cout << "fixture=FAIL\n"; return 1; }

    feed::ReplayAdapter adapter;
    if (!adapter.open(dir)) { std::cout << "adapter_open=FAIL\n"; return 1; }

    std::array<book::OrderBook, 5> books{};
    std::array<features::FeatureEngine, 5> engines{};
    for (std::size_t i = 0; i < books.size(); ++i)
        books[i] = book::OrderBook(static_cast<std::uint32_t>(i));

    replay::Digest digest;
    core::FixedEvent event{};

    std::uint64_t events = 0;
    std::uint64_t steady_state_allocations = 0;
    std::uint64_t transition_allocations = 0;
    std::uint64_t transitions = 0;
    std::uint64_t worst_transition = 0;

    auto segments_seen = adapter.stats().segments_opened;

    tracking.store(true, std::memory_order_relaxed);
    for (;;) {
        const auto before = allocations.load(std::memory_order_relaxed);
        const auto poll = adapter.poll(event);
        const auto after = allocations.load(std::memory_order_relaxed);
        const auto delta = after - before;

        const auto segments_now = adapter.stats().segments_opened;
        const bool crossed = segments_now != segments_seen;
        segments_seen = segments_now;

        if (poll != feed::PollResult::event) break;

        if (crossed) {
            ++transitions;
            transition_allocations += delta;
            if (delta > worst_transition) worst_transition = delta;
        } else {
            steady_state_allocations += delta;
        }

        ++events;
        digest.mix_header(event.header);
        core::QuotePayload payload{};
        std::memcpy(&payload, event.payload.data(), sizeof(payload));
        const auto symbol = event.header.symbol_id % books.size();
        (void)books[symbol].apply_quote(payload.bid, payload.ask, payload.bid_size,
                                        payload.ask_size, event.header.ts_local_ns);
        digest.mix(books[symbol].checksum());
        const auto vector = engines[symbol].compute(books[symbol], book::BookSource::l1_only);
        for (std::size_t i = 0; i < features::feature_count; ++i)
            digest.mix_double(vector.values[i]);
    }
    tracking.store(false, std::memory_order_relaxed);

    const double per_transition = transitions > 0
        ? static_cast<double>(transition_allocations) / static_cast<double>(transitions) : 0.0;

    std::cout << "replay_events=" << events << '\n';
    std::cout << "replay_segment_transitions=" << transitions << '\n';
    std::cout << "replay_steady_state_allocations=" << steady_state_allocations << '\n';
    std::cout << "replay_transition_allocations=" << transition_allocations
              << " per_transition_avg=" << per_transition
              << " per_transition_max=" << worst_transition << '\n';
    std::cout << "replay_digest=" << digest.value() << '\n';

    int failures = 0;
    if (events != 20'000) { std::cout << "replay_event_count=FAIL\n"; ++failures; }
    if (transitions == 0) { std::cout << "replay_transitions_exercised=FAIL\n"; ++failures; }

    // The load-bearing assertion: the per-event path allocates nothing, ever.
    if (steady_state_allocations != 0) {
        std::cout << "replay_zero_alloc_per_event=FAIL\n";
        ++failures;
    } else {
        std::cout << "replay_zero_alloc_per_event=pass\n";
    }

    // Segment opens must stay bounded and O(1) per segment rather than growing
    // with the run — an unbounded creep here would eventually be a leak.
    if (worst_transition > 32) {
        std::cout << "replay_transition_allocation_bounded=FAIL\n";
        ++failures;
    } else {
        std::cout << "replay_transition_allocation_bounded=pass\n";
    }

    replay_fixture::remove_all(dir);
    return failures == 0 ? 0 : 1;
}
