#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include "exec/exec_types.hpp"
#include "exec/journal.hpp"
#include "oms/order_state.hpp"
#include "risk/risk_engine.hpp"
namespace oms {

// Phase B — capacity/storage redesign.
//
// Was: std::array<Order, 256>, append-only, O(n) find(). Two independent
// "256" constants (this file and exec::max_live_orders in paper_broker.hpp)
// had to be kept in sync by convention, and there was no reclaim at all: a
// terminal order held its slot forever, so 256 was really a lifetime cap on
// orders ever created, not a live-order cap.
//
// Now: capacity is a constructor parameter (a single preallocated block, one
// allocation, at construction only — never on create/find/transition/reclaim),
// a fixed open-addressed hash index gives O(1) average find() by
// BrokerOrderRef, and a terminal transition reclaims its slot via swap-with-
// last-live so [0, size()) stays dense with no holes — the exact invariant
// recovery.hpp and the byte-identical-replay tests already depend on.
//
// The default capacity is exec::max_journal_records: the true, provable upper
// bound on distinct orders derivable from one journal (Part 11.1), which is
// how every existing default-constructed Oms (RecoveryState, risk_bench,
// phase6 tests) is actually used. Scale beyond that — up to and including
// millions of simultaneously live orders — is a constructor argument, proven
// by tests/cpp/phase6_oms_capacity.cpp and src/apps/oms_bench_main.cpp, not a
// silent default paid by every small caller.

inline constexpr std::size_t default_capacity = exec::max_journal_records;

namespace detail {

[[nodiscard]] constexpr std::size_t next_pow2(std::size_t n) noexcept {
    std::size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// splitmix64-style finalizer, matching the mixing already used for
// PaperBroker's duplicate-order set (paper_broker.hpp) for a consistent,
// previously-reviewed avalanche construction.
[[nodiscard]] constexpr std::uint64_t mix_ref(exec::BrokerOrderRef ref) noexcept {
    std::uint64_t x = ref.logical_order_id ^ (ref.run_id * 0x9E3779B97F4A7C15ULL);
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27; x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

}  // namespace detail

class Oms final {
public:
    // exec::Order has trailing struct padding (after `triggered`/
    // `reduce_only`, and after `queue_validated`). make_unique<T[]>(n) is
    // specified to value-initialize, which should zero that padding, but a
    // recovery.hpp bug fixed earlier in this project (RecoveryState's
    // compiler-generated assignment bulk-copying AccountState's padding
    // verbatim) and an intermittent (0/70 direct runs, 1/1 the one time it
    // ran immediately after a heavy multi-threaded test via ctest) failure
    // of the byte-identical replay/digest tests are both explained by the
    // same root cause: relying on implicit value-init to guarantee zero
    // padding is not reliable enough for a byte-for-byte determinism gate
    // in this toolchain. An explicit memset closes the gap unconditionally,
    // the same fix already proven for AccountState.
    explicit Oms(std::size_t capacity = default_capacity)
        : capacity_(capacity),
          index_capacity_(detail::next_pow2((capacity_ ? capacity_ : 1) * 2)),
          orders_(std::make_unique<exec::Order[]>(capacity_)),
          index_(std::make_unique<std::uint32_t[]>(index_capacity_)) {
        std::memset(orders_.get(), 0, capacity_ * sizeof(exec::Order));
    }

    Oms(const Oms& other) : Oms(other.capacity_) { *this = other; }

    Oms& operator=(const Oms& other) {
        if (this == &other) return *this;
        if (capacity_ != other.capacity_ || index_capacity_ != other.index_capacity_) {
            capacity_ = other.capacity_;
            index_capacity_ = other.index_capacity_;
            orders_ = std::make_unique<exec::Order[]>(capacity_);
            index_ = std::make_unique<std::uint32_t[]>(index_capacity_);
        }
        // copy_n's per-element assignment has the same padding gap as any
        // other memberwise assignment; zero first so the destination's
        // padding is deterministic before named fields are overwritten.
        std::memset(orders_.get(), 0, capacity_ * sizeof(exec::Order));
        std::copy_n(other.orders_.get(), capacity_, orders_.get());
        std::copy_n(other.index_.get(), index_capacity_, index_.get());
        count_ = other.count_;
        reserved_ = other.reserved_;
        tombstones_ = other.tombstones_;
        return *this;
    }

    Oms(Oms&&) noexcept = default;
    Oms& operator=(Oms&&) noexcept = default;
    ~Oms() = default;

    // Invariant 5: an order reserves its FULL requested volume, not the unfilled
    // remainder — an UNKNOWN order may have executed any part of itself. Reserve
    // and release must therefore be symmetric on requested_volume, or a partial
    // fill silently strands reservation that Part 8.5 says is released only on a
    // terminal state.
    [[nodiscard]] exec::Order* restore(const exec::Order& o) noexcept {
        if (find(o.ref) || count_ >= capacity_ || o.requested_volume <= 0 ||
            o.filled_volume < 0 || o.filled_volume > o.requested_volume)
            return nullptr;
        const auto slot = count_;
        orders_[slot] = o;
        index_insert(o.ref, static_cast<std::uint32_t>(slot));
        if (exec::reserves_exposure(o.state)) reserved_ += o.requested_volume;
        ++count_;
        return &orders_[slot];
    }

    [[nodiscard]] exec::Order* create(exec::BrokerOrderRef ref, std::uint32_t symbol,
                                      std::int64_t volume, const risk::Approval&) noexcept {
        if (find(ref) || count_ >= capacity_) return nullptr;
        const auto slot = count_++;
        auto& o = orders_[slot];
        o = {};
        o.ref = ref;
        o.symbol_id = symbol;
        o.requested_volume = volume;
        o.state = exec::OrderState::pending_send;
        index_insert(ref, static_cast<std::uint32_t>(slot));
        reserved_ += volume;
        return &o;
    }

    [[nodiscard]] bool transition(exec::BrokerOrderRef ref, exec::OrderState next) noexcept {
        const auto pos = index_probe_find(ref);
        if (pos == index_capacity_) return false;
        const auto slot = index_[pos] - occupied_bias;
        auto& o = orders_[slot];
        if (!legal(o.state, next)) return false;
        const auto was = exec::reserves_exposure(o.state);
        o.state = next;
        const auto now = exec::reserves_exposure(next);
        if (was && !now) reserved_ -= o.requested_volume;
        if (exec::is_terminal(next)) reclaim(slot);
        return true;
    }

    [[nodiscard]] bool fill(exec::BrokerOrderRef ref, std::int64_t volume) noexcept {
        auto* o = find(ref);
        if (!o || volume <= 0 || volume > o->remaining()) return false;
        o->filled_volume += volume;
        return transition(ref, o->remaining() ? exec::OrderState::partially_filled
                                              : exec::OrderState::filled);
    }

    [[nodiscard]] bool recover_fill(exec::BrokerOrderRef ref, std::int64_t volume) noexcept {
        auto* o = find(ref);
        if (!o || volume <= 0 || volume > o->remaining()) return false;
        o->filled_volume += volume;
        return true;
    }

    [[nodiscard]] bool mark_unknown_on_restart(exec::BrokerOrderRef ref) noexcept {
        auto* o = find(ref);
        if (!o || exec::is_terminal(o->state)) return false;
        o->state = exec::OrderState::unknown;
        return true;
    }

    [[nodiscard]] bool resolve_unknown(exec::BrokerOrderRef ref, exec::OrderState terminal) noexcept {
        auto* o = find(ref);
        return o && o->state == exec::OrderState::unknown && exec::is_terminal(terminal) &&
               transition(ref, terminal);
    }

    [[nodiscard]] exec::Order* find(exec::BrokerOrderRef ref) noexcept {
        const auto pos = index_probe_find(ref);
        return pos == index_capacity_ ? nullptr : &orders_[index_[pos] - occupied_bias];
    }
    [[nodiscard]] const exec::Order* find(exec::BrokerOrderRef ref) const noexcept {
        const auto pos = index_probe_find(ref);
        return pos == index_capacity_ ? nullptr : &orders_[index_[pos] - occupied_bias];
    }

    [[nodiscard]] const exec::Order& at(std::size_t i) const noexcept { return orders_[i]; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::int64_t reserved_exposure() const noexcept { return reserved_; }

private:
    static constexpr std::uint32_t empty_marker = 0;
    static constexpr std::uint32_t tombstone_marker = 1;
    static constexpr std::uint32_t occupied_bias = 2;   // stored value = slot + occupied_bias

    // Fixed open-addressed probe, wrapping the whole table. index_capacity_ is
    // sized 2x capacity_ (power of two) and tombstones_ is kept below
    // capacity_ by rehash(), so live+tombstone occupancy never exceeds ~50% and
    // an EMPTY slot is always reachable within one full pass.
    [[nodiscard]] std::size_t index_probe_find(exec::BrokerOrderRef ref) const noexcept {
        const auto home = static_cast<std::size_t>(detail::mix_ref(ref)) & (index_capacity_ - 1);
        for (std::size_t step = 0; step < index_capacity_; ++step) {
            const auto pos = (home + step) & (index_capacity_ - 1);
            const auto v = index_[pos];
            if (v == empty_marker) return index_capacity_;        // probe run truly ended
            if (v != tombstone_marker && orders_[v - occupied_bias].ref == ref) return pos;
        }
        return index_capacity_;
    }

    // Caller guarantees ref is not already present (find() checked first in
    // every insert path above).
    void index_insert(exec::BrokerOrderRef ref, std::uint32_t slot) noexcept {
        const auto home = static_cast<std::size_t>(detail::mix_ref(ref)) & (index_capacity_ - 1);
        for (std::size_t step = 0; step < index_capacity_; ++step) {
            const auto pos = (home + step) & (index_capacity_ - 1);
            const auto v = index_[pos];
            if (v == empty_marker || v == tombstone_marker) {
                if (v == tombstone_marker) --tombstones_;
                index_[pos] = slot + occupied_bias;
                return;
            }
        }
    }

    // Terminal reclaim: erase this order's index entry, then swap the last
    // live order into the vacated array slot so [0, count_) stays dense with
    // no holes — recovery.hpp's replay loop and the byte-identical-replay
    // tests iterate exactly that range and rely on it enumerating precisely
    // the live set. The moved order's ref never changes, only its physical
    // slot, so its existing index entry is updated in place rather than
    // re-probed from scratch.
    void reclaim(std::size_t slot) noexcept {
        const auto pos = index_probe_find(orders_[slot].ref);
        index_[pos] = tombstone_marker;
        ++tombstones_;
        const auto last = count_ - 1;
        if (slot != last) {
            orders_[slot] = orders_[last];
            const auto moved_pos = index_probe_find(orders_[slot].ref);
            index_[moved_pos] = static_cast<std::uint32_t>(slot) + occupied_bias;
        }
        --count_;
        // Amortized O(1): a rehash clears every tombstone, so at most one
        // rehash happens per capacity_ reclaims. No allocation — it reuses
        // the existing index_ buffer in place.
        if (tombstones_ > capacity_) rehash();
    }

    void rehash() noexcept {
        std::fill_n(index_.get(), index_capacity_, empty_marker);
        tombstones_ = 0;
        for (std::size_t slot = 0; slot < count_; ++slot)
            index_insert(orders_[slot].ref, static_cast<std::uint32_t>(slot));
    }

    std::size_t capacity_;
    std::size_t index_capacity_;
    std::unique_ptr<exec::Order[]> orders_;
    std::unique_ptr<std::uint32_t[]> index_;
    std::size_t count_{0};
    std::size_t tombstones_{0};
    std::int64_t reserved_{0};
};
}
