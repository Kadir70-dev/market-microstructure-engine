#pragma once

#include <cstdint>

#include "exec/exec_types.hpp"
#include "strategy/signals.hpp"

// Phase 7 — strategy intents, lifecycle FSM and configuration.
//
// Part 10: "A strategy submits an Intent; risk returns Approve | Resize |
// Reject. Only the risk engine can construct an Order." Nothing here can build
// an exec::Order — the strategy's entire output surface is an Intent, and the
// Approval token needed by Oms::create is constructible only by RiskEngine.
// That is enforced by the type system, not by convention.

namespace strategy {

enum class IntentKind : std::uint8_t { none = 0, enter = 1, exit = 2 };

struct Intent final {
    IntentKind kind{IntentKind::none};
    std::uint32_t strategy_id{0};
    std::uint32_t symbol_id{0};
    exec::Side side{exec::Side::buy};
    std::int64_t volume{0};
    std::uint64_t ts_ns{0};
    bool reduce_only{false};

    [[nodiscard]] constexpr bool actionable() const noexcept { return kind != IntentKind::none; }
};

// Lifecycle FSM. `cold` is distinct from `idle` on purpose: a strategy whose
// features have never warmed has not decided to stand aside, it is structurally
// unable to decide at all.
enum class StrategyState : std::uint8_t {
    cold = 0,
    idle = 1,
    entry_pending = 2,
    in_position = 3,
    exit_pending = 4,
    cooldown = 5,
    halted = 6
};

[[nodiscard]] constexpr bool legal(StrategyState a, StrategyState b) noexcept {
    using S = StrategyState;
    if (b == S::halted) return a != S::halted;          // halt is always reachable
    switch (a) {
        case S::cold:          return b == S::idle;
        case S::idle:          return b == S::entry_pending || b == S::cold;
        case S::entry_pending: return b == S::in_position || b == S::idle;
        case S::in_position:   return b == S::exit_pending;
        case S::exit_pending:  return b == S::cooldown || b == S::in_position;
        case S::cooldown:      return b == S::idle;
        case S::halted:        return false;            // manual re-arm only
    }
    return false;
}

// Why an entry was not taken. Recorded rather than discarded: "no trade" with no
// reason is indistinguishable from a broken signal path.
enum class Veto : std::uint8_t {
    none = 0,
    cold_signals,
    wrong_state,
    cooldown_active,
    max_positions,
    spread_too_wide,
    volatility_out_of_band,
    signal_below_threshold,
    averaging_down,
    halted
};

struct StrategyConfig final {
    std::uint32_t strategy_id{1};
    std::int64_t volume{1};

    // ---- entry rules ----
    double entry_queue_imbalance{0.35};   // |queue imbalance| required
    double entry_ofi{0.0};                // OFI must agree in sign, magnitude >= this
    std::int64_t max_spread_ticks{20};
    double min_volatility{0.0};
    double max_volatility{1.0};
    bool require_ofi_agreement{true};

    // ---- exit rules ----
    std::int64_t take_profit_ticks{10};
    std::int64_t stop_loss_ticks{10};
    std::uint64_t max_hold_ns{120'000'000'000ULL};   // time stop, 120 s

    // ---- gates ----
    std::uint64_t cooldown_ns{30'000'000'000ULL};
    std::uint32_t max_positions{1};

    // Signals that must be warm before any entry. Exits are deliberately NOT
    // gated on this: Part 18 Phase 7 requires time-stop and hard-stop to be
    // "always honoured", and a stop that stops working when features go cold is
    // not a stop.
    std::uint32_t required_signals{signal_bit(Signal::queue_imbalance) |
                                   signal_bit(Signal::spread)};

    // Averaging down is prohibited outright (Part 18 Phase 7). Present as a
    // named constant so the prohibition is visible, not implicit.
    static constexpr bool allow_averaging_down = false;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return volume > 0 && max_positions >= 1 && take_profit_ticks > 0 &&
               stop_loss_ticks > 0 && max_hold_ns > 0 && max_spread_ticks > 0 &&
               entry_queue_imbalance > 0.0 && entry_queue_imbalance <= 1.0 &&
               min_volatility >= 0.0 && max_volatility >= min_volatility;
    }
};

// The strategy's own view of what it holds. Kept separate from the PMS: the
// strategy reasons about its own intent, the PMS is the record of truth, and a
// divergence between them is a reconciliation break rather than something the
// strategy should paper over.
struct OpenPosition final {
    bool active{false};
    exec::Side side{exec::Side::buy};
    std::int64_t volume{0};
    std::int64_t entry_ticks{0};
    std::uint64_t opened_ns{0};
};

// `signal_flip` is appended, never inserted: these values are written into the
// trade log and compared across runs, so renumbering the existing four would
// silently reinterpret every record already on disk.
enum class ExitReason : std::uint8_t {
    none = 0, take_profit, stop_loss, time_stop, halt, signal_flip
};

}  // namespace strategy
