#pragma once

#include <cstdint>

// Phase 5 — execution types, order identity and the Part 8 state machines.
//
// Everything here is integer. Prices are int64 ticks and volumes int64 lot-steps
// (Part 5.9: "Prices are int64 ticks everywhere on the hot path"). Money is
// int64 minor units. Floating point appears in the models, never in accounting:
// a PnL built from doubles cannot be compared bit-for-bit across runs, which
// would make the Phase 5 determinism gate unachievable.

namespace exec {

// ---- order identity (Part 8.2) -------------------------------------------

// logical_order_id = hash(strategy_id, symbol_id, seq_global). Deterministic and
// reproducible in replay, which is what lets two runs be compared order-by-order
// rather than merely in aggregate.
[[nodiscard]] constexpr std::uint64_t make_logical_order_id(
    std::uint32_t strategy_id, std::uint32_t symbol_id, std::uint64_t seq_global) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) constexpr {
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (value >> (byte * 8)) & 0xFFULL;
            hash *= 1099511628211ULL;
        }
    };
    mix(strategy_id);
    mix(symbol_id);
    mix(seq_global);
    return hash;
}

// broker_order_ref = (run_id, logical_order_id), globally unique (Part 8.2).
struct BrokerOrderRef final {
    std::uint64_t run_id{0};
    std::uint64_t logical_order_id{0};

    friend constexpr bool operator==(BrokerOrderRef a, BrokerOrderRef b) noexcept {
        return a.run_id == b.run_id && a.logical_order_id == b.logical_order_id;
    }
};

// ---- enums ----------------------------------------------------------------

enum class Side : std::uint8_t { buy = 0, sell = 1 };

// Part 9.2 specifies fill mechanics for market and limit only.
//
// DOCUMENTED ASSUMPTION — stop and stop_limit are NOT defined by Architecture
// Version 1.0. They are implemented here with the unambiguous standard
// semantic (trigger -> market, trigger -> limit) because the Phase 5 test list
// names stop_order. Any result that depends on them is an assumption, not a
// specified behaviour, and is flagged as such in the engineering report.
enum class OrderType : std::uint8_t { market = 0, limit = 1, stop = 2, stop_limit = 3 };

// Part 8.3. Terminal: risk_rejected, filled, cancelled, rejected, expired.
// unknown is deliberately NOT terminal — it holds full reserved exposure until
// reconciliation resolves it (Part 8.3, Invariant 5).
enum class OrderState : std::uint8_t {
    new_order = 0,
    risk_rejected = 1,
    pending_send = 2,
    sent = 3,
    acknowledged = 4,
    rejected = 5,
    unknown = 6,
    partially_filled = 7,
    filled = 8,
    cancel_pending = 9,
    cancelled = 10,
    expired = 11
};

[[nodiscard]] constexpr bool is_terminal(OrderState state) noexcept {
    return state == OrderState::risk_rejected || state == OrderState::filled ||
           state == OrderState::cancelled || state == OrderState::rejected ||
           state == OrderState::expired;
}

// Exposure is reserved from pending_send and released only on a terminal state
// (Part 8.5). unknown counts fully (Invariant 5).
[[nodiscard]] constexpr bool reserves_exposure(OrderState state) noexcept {
    return !is_terminal(state) && state != OrderState::new_order;
}

// Part 8.4.
enum class PositionState : std::uint8_t {
    opening = 0, open = 1, modify_pending = 2, closing = 3, closed = 4, reconcile_unknown = 5
};

// Every way an order can be refused. Each is fail-closed: the order does not
// enter the book and reserves nothing.
enum class RejectReason : std::uint8_t {
    none = 0,
    invalid_price,
    invalid_volume,
    invalid_timestamp,
    duplicate_order_id,
    unknown_symbol,
    insufficient_margin,
    volume_below_minimum,
    volume_above_maximum,
    volume_not_step_aligned,
    price_not_tick_aligned,
    stops_level_violation,
    market_closed,
    stale_quote,
    impossible_fill,
    corrupted_state,
    probabilistic_reject,
    reduce_only_no_position,   // no opposing position to reduce
    reduce_only_excess_volume  // would exceed reducible quantity
};

// Part 9.4. Only pessimistic is promotion-grade.
enum class SimulationMode : std::uint8_t { optimistic = 0, base = 1, pessimistic = 2 };

// ---- symbol specification -------------------------------------------------

// Explicit subset of Part 5.9 symbol metadata. Named SymbolSpec rather than
// SymbolMeta so it cannot be mistaken for the full frozen structure, which is a
// feed-layer concern and does not yet exist as a C++ type.
struct SymbolSpec final {
    std::uint32_t symbol_id{0};
    std::int64_t tick_size_ticks{1};       // price granularity, in ticks
    std::int64_t volume_min{1};            // in lot-steps
    std::int64_t volume_max{100'000};
    std::int64_t volume_step{1};
    std::int64_t stops_level_ticks{0};
    std::int64_t freeze_level_ticks{0};
    std::int64_t contract_size{100'000};   // units per lot
    std::int64_t tick_value_minor{1};      // account minor units per tick per lot-step
    std::int64_t commission_per_lot_minor{0};
    std::int64_t margin_rate_bp{100};      // basis points of notional, 100bp = 1%
    bool tradable{true};

    [[nodiscard]] constexpr bool volume_valid(std::int64_t volume) const noexcept {
        return volume >= volume_min && volume <= volume_max &&
               volume_step > 0 && (volume % volume_step) == 0;
    }
    [[nodiscard]] constexpr bool price_aligned(std::int64_t price_ticks) const noexcept {
        return tick_size_ticks > 0 && (price_ticks % tick_size_ticks) == 0;
    }
};

// ---- order ----------------------------------------------------------------

struct Order final {
    BrokerOrderRef ref{};
    std::uint64_t corr_id{0};
    std::uint32_t symbol_id{0};
    Side side{Side::buy};
    OrderType type{OrderType::market};
    OrderState state{OrderState::new_order};
    RejectReason reject_reason{RejectReason::none};

    std::int64_t requested_volume{0};
    std::int64_t filled_volume{0};
    std::int64_t limit_price_ticks{0};     // limit / stop_limit
    std::int64_t trigger_price_ticks{0};   // stop / stop_limit
    bool triggered{false};
    bool reduce_only{false};               // must never increase absolute exposure

    std::uint64_t ts_submit_ns{0};
    std::uint64_t ts_effective_ns{0};      // t_send + latency (Part 9.2)
    std::uint64_t ts_terminal_ns{0};
    std::uint64_t expire_ns{0};            // 0 = good-till-cancelled

    // Queue position (Part 9.2). Only meaningful with real depth; on
    // L1_ONLY / DOM_* sources the run is tagged NOT VALIDATED.
    std::int64_t queue_ahead{0};
    bool queue_validated{false};

    std::int64_t avg_fill_price_ticks{0};
    std::int64_t commission_minor{0};

    [[nodiscard]] constexpr std::int64_t remaining() const noexcept {
        return requested_volume - filled_volume;
    }
};

struct Position final {
    std::uint64_t position_ticket{0};
    std::uint32_t symbol_id{0};
    Side side{Side::buy};
    PositionState state{PositionState::opening};
    std::int64_t volume{0};
    std::int64_t avg_price_ticks{0};
    std::int64_t realized_pnl_minor{0};
    std::uint64_t opened_ns{0};
    std::uint64_t closed_ns{0};
};

struct Fill final {
    BrokerOrderRef ref{};
    std::uint64_t position_ticket{0};
    std::uint32_t symbol_id{0};
    Side side{Side::buy};
    std::int64_t volume{0};
    std::int64_t price_ticks{0};
    std::int64_t commission_minor{0};
    std::int64_t remaining{0};
    std::uint64_t ts_ns{0};
    bool is_final{false};
};

}  // namespace exec
