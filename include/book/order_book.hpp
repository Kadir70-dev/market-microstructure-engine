#pragma once

#include <array>
#include <cstdint>

#include "book/book_types.hpp"
#include "core/event.hpp"

// Phase 3 — Order Book engine.
//
// Fixed-capacity, allocation-free, comparison-sorted depth. Levels live in two
// std::array<Level, max_book_depth> and are kept ordered by linear insertion:
// bids descending, asks ascending. A linear scan over <= 32 contiguous POD
// entries is both faster and more predictable than a node-based container, and
// Part 18 Phase 3 forbids std::map on the hot path outright.
//
// Two ingestion paths, deliberately not mixed on one instance:
//   apply_quote  — L1 sources (BookSource::l1_only). Top of book is the whole
//                  book, so the instance holds exactly one level per side.
//   apply_delta  — L2/DOM sources. Incremental ADD / MODIFY / REMOVE.
// Driving one instance from both paths is a caller error: a quote would
// overwrite depth it has no knowledge of.

namespace book {

class OrderBook final {
public:
    OrderBook() noexcept = default;

    explicit OrderBook(std::uint32_t symbol_id, SymbolLimits limits = {},
                       CrossedTolerance tolerance = {}) noexcept
        : symbol_id_(symbol_id), limits_(limits), tolerance_(tolerance) {}

    // ---- ingestion -------------------------------------------------------

    // L1 path. The quote *is* the book, so both sides collapse to one level.
    BookStatus apply_quote(std::int64_t bid_price, std::int64_t ask_price,
                           std::int64_t bid_size, std::int64_t ask_size,
                           std::uint64_t ts_ns) noexcept {
        if (!limits_.price_in_range(bid_price) || !limits_.price_in_range(ask_price) ||
            !limits_.size_in_range(bid_size) || !limits_.size_in_range(ask_size)) {
            ++rejected_;
            return BookStatus::rejected;
        }
        if (ask_price - bid_price > limits_.max_spread_ticks) {
            ++rejected_;
            return BookStatus::rejected;
        }
        // Presence is keyed on price, not size. MT5 FX quotes routinely carry
        // volume 0 because the venue publishes no size at L1; treating that as
        // an empty book would discard every forex tick we record.
        bids_[0] = Level{bid_price, bid_size};
        asks_[0] = Level{ask_price, ask_size};
        bid_depth_ = 1;
        ask_depth_ = 1;
        ++updates_;
        return evaluate_crossed(ts_ns);
    }

    // L2 / DOM path.
    BookStatus apply_delta(Side side, std::int64_t price, std::int64_t new_size,
                           DeltaAction action, std::uint64_t ts_ns) noexcept {
        if (!limits_.price_in_range(price) || !limits_.size_in_range(new_size)) {
            ++rejected_;
            return BookStatus::rejected;
        }
        // A zero size is a removal however it is labelled; venues disagree on
        // whether they send MODIFY-to-zero or REMOVE, and both mean the same.
        const auto effective = (new_size == 0) ? DeltaAction::remove : action;

        auto& levels = (side == Side::bid) ? bids_ : asks_;
        auto& depth = (side == Side::bid) ? bid_depth_ : ask_depth_;
        const auto index = find(levels, depth, price);

        switch (effective) {
            case DeltaAction::remove: {
                if (index == npos) {
                    // Removing a level we never had means our view diverged
                    // from the venue's. Part 18 Phase 3 requires a rebase here
                    // rather than a silent no-op.
                    request_rebase(price);
                    return BookStatus::rebase_required;
                }
                erase_at(levels, depth, index);
                break;
            }
            case DeltaAction::modify:
            case DeltaAction::add: {
                if (index != npos) {
                    levels[index].size = new_size;
                } else if (!insert_sorted(levels, depth, side, Level{price, new_size})) {
                    // Outside the tracked window: the price is worse than every
                    // level we hold and the book is full. Dropping it is correct
                    // for a depth-limited book, not a desync.
                    ++outside_window_;
                }
                break;
            }
        }
        ++updates_;
        return evaluate_crossed(ts_ns);
    }

    // ---- rebase ----------------------------------------------------------

    // Bounded cost: clearing two fixed arrays is O(max_book_depth) with no
    // allocation and no traversal of anything unbounded.
    void request_rebase(std::int64_t new_ref) noexcept {
        rebase_old_ref_ = rebase_new_ref_;
        rebase_new_ref_ = new_ref;
        clear();
        ++rebases_;
        rebase_pending_ = true;
    }

    // Drains one pending rebase into the wire payload. Returns false when there
    // is nothing to emit, so callers can poll unconditionally.
    [[nodiscard]] bool consume_rebase(core::BookRebasePayload& out) noexcept {
        if (!rebase_pending_) return false;
        out.symbol_id = symbol_id_;
        out.old_ref = rebase_old_ref_;
        out.new_ref = rebase_new_ref_;
        rebase_pending_ = false;
        return true;
    }

    void clear() noexcept {
        bid_depth_ = 0;
        ask_depth_ = 0;
        crossed_updates_ = 0;
        crossed_since_ns_ = 0;
    }

    // ---- accessors -------------------------------------------------------

    [[nodiscard]] std::size_t depth(Side side) const noexcept {
        return (side == Side::bid) ? bid_depth_ : ask_depth_;
    }
    [[nodiscard]] Level level(Side side, std::size_t index) const noexcept {
        const auto& levels = (side == Side::bid) ? bids_ : asks_;
        const auto d = depth(side);
        return (index < d) ? levels[index] : Level{};
    }
    [[nodiscard]] bool has_both_sides() const noexcept {
        return bid_depth_ > 0 && ask_depth_ > 0;
    }
    [[nodiscard]] Level best(Side side) const noexcept { return level(side, 0); }
    [[nodiscard]] bool is_crossed() const noexcept {
        return has_both_sides() && bids_[0].price_ticks >= asks_[0].price_ticks;
    }

    [[nodiscard]] std::uint32_t symbol_id() const noexcept { return symbol_id_; }
    [[nodiscard]] std::uint64_t updates() const noexcept { return updates_; }
    [[nodiscard]] std::uint64_t rebases() const noexcept { return rebases_; }
    [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }
    [[nodiscard]] std::uint64_t outside_window() const noexcept { return outside_window_; }

    // ---- checksum --------------------------------------------------------

    // FNV-1a over the integer level ladder. Pure integer arithmetic in a fixed
    // traversal order, so it is reproducible across runs and machines — a
    // precondition for the Part 11.3 determinism gate. Never floating point.
    [[nodiscard]] std::uint64_t checksum() const noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
        hash = mix(hash, static_cast<std::uint64_t>(symbol_id_));
        hash = mix(hash, static_cast<std::uint64_t>(bid_depth_));
        hash = mix(hash, static_cast<std::uint64_t>(ask_depth_));
        for (std::size_t i = 0; i < bid_depth_; ++i) {
            hash = mix(hash, static_cast<std::uint64_t>(bids_[i].price_ticks));
            hash = mix(hash, static_cast<std::uint64_t>(bids_[i].size));
        }
        for (std::size_t i = 0; i < ask_depth_; ++i) {
            hash = mix(hash, static_cast<std::uint64_t>(asks_[i].price_ticks));
            hash = mix(hash, static_cast<std::uint64_t>(asks_[i].size));
        }
        return hash;
    }

private:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    static std::uint64_t mix(std::uint64_t hash, std::uint64_t value) noexcept {
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (value >> (byte * 8)) & 0xFFULL;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    static std::size_t find(const std::array<Level, max_book_depth>& levels,
                            std::size_t depth, std::int64_t price) noexcept {
        for (std::size_t i = 0; i < depth; ++i)
            if (levels[i].price_ticks == price) return i;
        return npos;
    }

    static void erase_at(std::array<Level, max_book_depth>& levels,
                         std::size_t& depth, std::size_t index) noexcept {
        for (std::size_t i = index; i + 1 < depth; ++i) levels[i] = levels[i + 1];
        if (depth > 0) --depth;
    }

    // Bids sort descending, asks ascending, so index 0 is always the best.
    static bool insert_sorted(std::array<Level, max_book_depth>& levels,
                              std::size_t& depth, Side side, Level entry) noexcept {
        std::size_t position = depth;
        for (std::size_t i = 0; i < depth; ++i) {
            const bool better = (side == Side::bid) ? (entry.price_ticks > levels[i].price_ticks)
                                                    : (entry.price_ticks < levels[i].price_ticks);
            if (better) { position = i; break; }
        }
        if (position == max_book_depth) return false;   // full and strictly worse
        if (depth == max_book_depth) {
            if (position >= max_book_depth) return false;
            --depth;                                     // evict the worst level
        }
        for (std::size_t i = depth; i > position; --i) levels[i] = levels[i - 1];
        levels[position] = entry;
        ++depth;
        return true;
    }

    BookStatus evaluate_crossed(std::uint64_t ts_ns) noexcept {
        if (!is_crossed()) {
            crossed_updates_ = 0;
            crossed_since_ns_ = 0;
            return BookStatus::ok;
        }
        if (crossed_updates_ == 0) crossed_since_ns_ = ts_ns;
        ++crossed_updates_;
        const bool too_many = crossed_updates_ > tolerance_.max_consecutive_updates;
        const bool too_long = ts_ns >= crossed_since_ns_ &&
                              (ts_ns - crossed_since_ns_) > tolerance_.max_duration_ns;
        return (too_many || too_long) ? BookStatus::crossed_halt : BookStatus::crossed_tolerated;
    }

    std::array<Level, max_book_depth> bids_{};
    std::array<Level, max_book_depth> asks_{};
    std::size_t bid_depth_{0};
    std::size_t ask_depth_{0};

    std::uint32_t symbol_id_{0};
    SymbolLimits limits_{};
    CrossedTolerance tolerance_{};

    std::uint32_t crossed_updates_{0};
    std::uint64_t crossed_since_ns_{0};

    bool rebase_pending_{false};
    std::int64_t rebase_old_ref_{0};
    std::int64_t rebase_new_ref_{0};

    std::uint64_t updates_{0};
    std::uint64_t rebases_{0};
    std::uint64_t rejected_{0};
    std::uint64_t outside_window_{0};
};

}  // namespace book
