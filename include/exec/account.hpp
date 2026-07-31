#pragma once

#include <array>
#include <cstdint>

#include "exec/exec_types.hpp"

// Phase 5 — simulated account, margin model and position book.
//
// All money is int64 minor units and all arithmetic is integer. Part 9.1
// requires byte-identical output across runs; a balance accumulated in double
// would drift by an ulp under any reordering and make that unachievable.
//
// Positions are keyed by position_ticket with a derived per-symbol aggregate,
// exactly as Part 8.1 specifies. That one representation serves both netting and
// hedging: netting simply never holds more than one ticket per symbol. Modelling
// net-only would produce the permanent false reconciliation mismatch on hedging
// accounts that Part 8.1 exists to prevent.

namespace exec {

inline constexpr std::size_t max_positions = 64;
inline constexpr std::size_t max_symbols = 8;

struct AccountState final {
    std::int64_t balance_minor{0};
    std::int64_t equity_minor{0};
    std::int64_t margin_used_minor{0};
    std::int64_t free_margin_minor{0};
    std::int64_t realized_pnl_minor{0};
    std::int64_t unrealized_pnl_minor{0};
    std::int64_t commission_paid_minor{0};
    std::uint32_t open_positions{0};
    std::uint32_t open_orders{0};
    bool stopped_out{false};
};

class MarginModel final {
public:
    // Notional * margin_rate. Integer with explicit rounding so the value is
    // reproducible rather than dependent on FP rounding mode.
    [[nodiscard]] static std::int64_t required_minor(const SymbolSpec& spec,
                                                     std::int64_t volume,
                                                     std::int64_t price_ticks) noexcept {
        if (volume <= 0 || price_ticks <= 0) return 0;
        const auto notional = volume * price_ticks * spec.tick_value_minor;
        return (notional * spec.margin_rate_bp + 9'999) / 10'000;   // round up, never under-reserve
    }

    // Broker stop-out. Modelled as a first-class outcome per Part 9.2 rather
    // than an error: a strategy that survives only because the simulator forgot
    // to stop it out is not a strategy that survived.
    [[nodiscard]] static bool stop_out(const AccountState& account,
                                       std::int64_t stop_out_level_bp) noexcept {
        if (account.margin_used_minor <= 0) return false;
        const auto level_bp = (account.equity_minor * 10'000) / account.margin_used_minor;
        return level_bp < stop_out_level_bp;
    }
};

class PositionBook final {
public:
    // Returns nullptr when full rather than growing: the hot path may not
    // allocate, and an unbounded position book would be an unbounded risk.
    [[nodiscard]] Position* open(std::uint64_t ticket, std::uint32_t symbol_id, Side side,
                                 std::int64_t volume, std::int64_t price_ticks,
                                 std::uint64_t ts_ns) noexcept {
        // A closed ticket is history, and history lives in the journal — it has
        // already been emitted as a position_update before it could be closed.
        // Keeping its slot occupied made `max_positions` a cap on the number of
        // round trips a run may ever perform, not on concurrent exposure: in
        // hedging mode every entry opens a fresh ticket, so the book filled after
        // exactly 64 lifetime trades and the broker fail-closed with
        // corrupted_state. Reclaim closed slots so the bound means what it says.
        // Tickets remain monotonic (`ticket_seq_` never rewinds), so a reused
        // slot can never be confused with the position that vacated it.
        std::size_t slot = max_positions;
        for (std::size_t i = 0; i < count_; ++i) {
            if (positions_[i].state == PositionState::closed) { slot = i; break; }
        }
        if (slot == max_positions) {
            if (count_ >= max_positions) return nullptr;
            slot = count_++;
        }
        auto& position = positions_[slot];
        position = Position{};
        position.position_ticket = ticket;
        position.symbol_id = symbol_id;
        position.side = side;
        position.state = PositionState::open;
        position.volume = volume;
        position.avg_price_ticks = price_ticks;
        position.opened_ns = ts_ns;
        return &position;
    }

    [[nodiscard]] Position* find(std::uint64_t ticket) noexcept {
        for (std::size_t i = 0; i < count_; ++i)
            if (positions_[i].position_ticket == ticket &&
                positions_[i].state != PositionState::closed) return &positions_[i];
        return nullptr;
    }

    // Adds to an existing ticket at a volume-weighted average price. Integer
    // weighted mean with explicit rounding, so it is reproducible.
    void add(Position& position, std::int64_t volume, std::int64_t price_ticks) noexcept {
        const auto total = position.volume + volume;
        if (total <= 0) return;
        const auto weighted = position.avg_price_ticks * position.volume + price_ticks * volume;
        position.avg_price_ticks = (weighted + total / 2) / total;
        position.volume = total;
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] const Position& at(std::size_t index) const noexcept { return positions_[index]; }
    [[nodiscard]] Position& at(std::size_t index) noexcept { return positions_[index]; }

    [[nodiscard]] std::uint32_t open_count() const noexcept {
        std::uint32_t open = 0;
        for (std::size_t i = 0; i < count_; ++i)
            if (positions_[i].state != PositionState::closed) ++open;
        return open;
    }

    // Per-symbol aggregate view used for risk limits (Part 8.1).
    [[nodiscard]] std::int64_t net_volume(std::uint32_t symbol_id) const noexcept {
        std::int64_t net = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& position = positions_[i];
            if (position.symbol_id != symbol_id || position.state == PositionState::closed) continue;
            net += (position.side == Side::buy) ? position.volume : -position.volume;
        }
        return net;
    }

    void clear() noexcept { count_ = 0; }

private:
    std::array<Position, max_positions> positions_{};
    std::size_t count_{0};
};

// Signed PnL in minor units for a position marked at `mark_ticks`.
[[nodiscard]] inline std::int64_t position_pnl_minor(const Position& position,
                                                     const SymbolSpec& spec,
                                                     std::int64_t mark_ticks) noexcept {
    if (position.volume <= 0 || mark_ticks <= 0) return 0;
    const auto move = (position.side == Side::buy)
        ? (mark_ticks - position.avg_price_ticks)
        : (position.avg_price_ticks - mark_ticks);
    return move * position.volume * spec.tick_value_minor;
}

}  // namespace exec
