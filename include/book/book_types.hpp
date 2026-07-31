#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

// Phase 3 — Order Book types.
//
// Depth is fixed at Architecture Part 7.3's MAX_BOOK_DEPTH = 32, the same bound
// the 1,040 B BookSnapshot slab is sized against. Every structure here is a
// trivially copyable POD held in fixed arrays: the hot path may not allocate,
// may not use std::map, and may not touch strings (Part 3).

namespace book {

inline constexpr std::size_t max_book_depth = 32;

enum class Side : std::uint8_t { bid = 0, ask = 1 };

// Wire-compatible with BookDeltaPayload::action.
enum class DeltaAction : std::uint8_t { add = 0, modify = 1, remove = 2 };

// Wire values match MmeBookSource in mme_protocol.mqh and the book_source byte
// carried on HeartbeatPayload. Ordering is meaningful: higher is strictly more
// informative, so feature validity can be expressed as a minimum source.
enum class BookSource : std::uint8_t {
    l1_only = 0, dom_aggregated = 1, dom_synthetic = 2, l2_exchange = 3, l3_mbo = 4
};

// Outcome of applying an update. Deliberately not a bool: Part 21 distinguishes
// a tolerated transient crossed book from one that has persisted long enough to
// be a halt condition, and a rebase from either.
enum class BookStatus : std::uint8_t {
    ok = 0,
    crossed_tolerated = 1,   // within the tolerance window; book still usable
    crossed_halt = 2,        // tolerance exhausted -> caller halts the symbol
    rebase_required = 3,     // inconsistent delta; book cleared, event emitted
    rejected = 4             // failed SymbolLimits sanity; update not applied
};

struct Level final {
    std::int64_t price_ticks{0};
    std::int64_t size{0};
};

static_assert(std::is_trivially_copyable_v<Level>);
static_assert(std::is_standard_layout_v<Level>);

// The subset of Part 5.9 symbol metadata the book needs for sanity checks.
// Deliberately narrow: this is not a stand-in for the full SymbolMeta, which
// belongs to the feed layer.
struct SymbolLimits final {
    std::int64_t min_price_ticks{1};
    std::int64_t max_price_ticks{std::int64_t{1} << 56};
    std::int64_t max_spread_ticks{std::int64_t{1} << 40};
    std::int64_t max_size{std::int64_t{1} << 56};

    [[nodiscard]] constexpr bool price_in_range(std::int64_t price) const noexcept {
        return price >= min_price_ticks && price <= max_price_ticks;
    }
    [[nodiscard]] constexpr bool size_in_range(std::int64_t size) const noexcept {
        return size >= 0 && size <= max_size;
    }
};

static_assert(std::is_trivially_copyable_v<SymbolLimits>);

// Crossed-book tolerance. Part 21 requires a tolerance window rather than an
// instant halt: a momentarily crossed book is normal across venues and during
// fast markets, and halting on the first occurrence would be a false positive.
struct CrossedTolerance final {
    std::uint32_t max_consecutive_updates{8};
    std::uint64_t max_duration_ns{50'000'000ULL};  // 50 ms
};

}  // namespace book
