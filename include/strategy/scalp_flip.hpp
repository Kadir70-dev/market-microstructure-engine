#pragma once

#include <cstdint>

#include "book/order_book.hpp"
#include "strategy/l1_signals.hpp"
#include "strategy/strategy_types.hpp"

// scalp_flip_v1 — DEMO EXECUTION strategy. L1 only.
//
// Directional scalp-and-flip: enter with short-term momentum, leave as soon as
// the position is worth a small POSITIVE NET amount after costs, and reverse
// when momentum turns against the open side.
//
// Three things make this different from mt5_l1_scalper_v1, which is left
// untouched:
//
//  1. The profit target is expressed in NET ticks after costs, not in raw price
//     movement. A long is marked at the bid and was filled at the ask, so the
//     mark already carries the spread; commission and a slippage allowance are
//     subtracted on top. `min_net_profit_ticks` is what is left over. Targeting
//     raw movement is how a "winning" scalper books a loss on every fill.
//
//  2. A reversal closes first and re-enters afterwards. `flip_pending()` tells
//     the runtime an exit is a reversal, so it can sequence close-then-open and
//     never hold both sides. One position, always.
//
//  3. Holding time is short and the stop is hard, because a scalp that has not
//     worked in seconds is not a scalp any more.
//
// Decision order is load-bearing and matches the rest of the codebase:
//   1. halt   2. exits/flips (unconditional)   3. cooldown   4. entries (gated)
//
// Exits are never gated on the warm mask: a stop that stops working when
// signals go cold is not a stop.

namespace strategy {

struct ScalpFlipConfig final {
    std::uint32_t strategy_id{3};
    std::int64_t volume{1};

    // ---- entry ----
    double entry_momentum{5.0e-7};
    // A reversal must be clearly opposite, not noise around zero, or the
    // strategy flips on every tick and pays the spread each time.
    double flip_momentum{1.0e-6};

    // ---- cost model, in ticks (points) ----
    // Round-trip cost that must be cleared before a position is "profitable".
    // commission_ticks covers broker commission; slippage_allowance_ticks covers
    // the adverse fill on the way out. Both are DECLARED, not calibrated.
    std::int64_t commission_ticks{2};
    std::int64_t slippage_allowance_ticks{3};
    // What must remain AFTER those costs for an exit to be taken.
    std::int64_t min_net_profit_ticks{5};

    // ---- risk ----
    std::int64_t stop_loss_ticks{120};              // hard stop
    std::uint64_t max_hold_ns{45'000'000'000ULL};   // 45 s
    std::uint64_t cooldown_ns{0};                   // immediate re-entry

    // ---- gates ----
    std::int64_t max_spread_ticks{60};
    std::uint32_t max_positions{1};
    std::uint32_t max_consecutive_losses{8};

    std::uint32_t required_signals{l1_bit(L1Signal::spread) | l1_bit(L1Signal::momentum)};

    static constexpr bool allow_averaging_down = false;

    // Total ticks of favourable movement needed before an exit is worth taking.
    [[nodiscard]] constexpr std::int64_t profit_threshold_ticks() const noexcept {
        return min_net_profit_ticks + commission_ticks + slippage_allowance_ticks;
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return volume > 0 && max_positions >= 1 && stop_loss_ticks > 0 &&
               max_hold_ns > 0 && max_spread_ticks > 0 && entry_momentum > 0.0 &&
               flip_momentum >= entry_momentum && min_net_profit_ticks > 0 &&
               commission_ticks >= 0 && slippage_allowance_ticks >= 0 &&
               max_consecutive_losses > 0 &&
               // A stop that sits inside the profit threshold would take the
               // loss before the win could ever be reached.
               stop_loss_ticks > profit_threshold_ticks();
    }
};

class ScalpFlip final {
public:
    explicit ScalpFlip(ScalpFlipConfig config) noexcept
        : config_(config), valid_(config.valid()) {}

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] StrategyState state() const noexcept { return state_; }
    [[nodiscard]] Veto last_veto() const noexcept { return veto_; }
    [[nodiscard]] ExitReason last_exit_reason() const noexcept { return exit_reason_; }
    [[nodiscard]] const OpenPosition& position() const noexcept { return position_; }
    [[nodiscard]] const L1SignalSet& signals() const noexcept { return signals_; }
    [[nodiscard]] std::uint64_t entries_emitted() const noexcept { return entries_; }
    [[nodiscard]] std::uint64_t exits_emitted() const noexcept { return exits_; }
    [[nodiscard]] std::uint64_t flips() const noexcept { return flips_; }
    [[nodiscard]] std::uint64_t vetoes() const noexcept { return veto_count_; }
    [[nodiscard]] std::uint32_t consecutive_losses() const noexcept { return consecutive_losses_; }
    [[nodiscard]] std::int64_t last_net_ticks() const noexcept { return last_net_ticks_; }

    // True when the exit now in flight is a reversal: the runtime should open
    // the opposite side once the close is confirmed, and not before.
    [[nodiscard]] bool flip_pending() const noexcept { return flip_pending_; }
    [[nodiscard]] exec::Side flip_side() const noexcept { return flip_side_; }

    void halt() noexcept { if (state_ != StrategyState::halted) state_ = StrategyState::halted; }

    [[nodiscard]] Intent on_market(std::uint32_t symbol_id, const book::OrderBook& b,
                                   std::uint64_t ts_ns) noexcept {
        Intent intent{};
        intent.strategy_id = config_.strategy_id;
        intent.symbol_id = symbol_id;
        intent.ts_ns = ts_ns;
        veto_ = Veto::none;

        if (!valid_ || state_ == StrategyState::halted) {
            veto_ = Veto::halted; ++veto_count_; return intent;
        }
        if (!b.has_both_sides()) { veto_ = Veto::cold_signals; ++veto_count_; return intent; }

        const auto bid = b.best(book::Side::bid).price_ticks;
        const auto ask = b.best(book::Side::ask).price_ticks;
        signals_ = engine_.update(bid, ask, ts_ns);

        // ---- 1. exits and flips, first and unconditional --------------------
        if (state_ == StrategyState::in_position && position_.active) {
            const auto reason = exit_due(bid, ask, ts_ns);
            if (reason != ExitReason::none) {
                exit_reason_ = reason;
                if (reason == ExitReason::signal_flip) {
                    flip_pending_ = true;
                    flip_side_ = (position_.side == exec::Side::buy) ? exec::Side::sell
                                                                     : exec::Side::buy;
                    ++flips_;
                }
                transition(StrategyState::exit_pending);
                intent.kind = IntentKind::exit;
                intent.side = (position_.side == exec::Side::buy) ? exec::Side::sell
                                                                  : exec::Side::buy;
                intent.volume = position_.volume;
                intent.reduce_only = true;
                ++exits_;
                return intent;
            }
            return intent;
        }

        // ---- 2. cooldown ----------------------------------------------------
        if (state_ == StrategyState::cooldown) {
            if (ts_ns >= cooldown_until_ns_) transition(StrategyState::idle);
            else { veto_ = Veto::cooldown_active; ++veto_count_; return intent; }
        }

        // ---- 3. warm-up -----------------------------------------------------
        const bool warm = (signals_.warm & config_.required_signals) == config_.required_signals;
        if (state_ == StrategyState::cold) {
            if (!warm) { veto_ = Veto::cold_signals; ++veto_count_; return intent; }
            transition(StrategyState::idle);
        }
        if (!warm) { veto_ = Veto::cold_signals; ++veto_count_; return intent; }
        if (state_ != StrategyState::idle) { veto_ = Veto::wrong_state; ++veto_count_; return intent; }
        if (position_.active && !ScalpFlipConfig::allow_averaging_down) {
            veto_ = Veto::averaging_down; ++veto_count_; return intent;
        }
        if (open_positions_ >= config_.max_positions) {
            veto_ = Veto::max_positions; ++veto_count_; return intent;
        }
        if ((ask - bid) > config_.max_spread_ticks) {
            veto_ = Veto::spread_too_wide; ++veto_count_; return intent;
        }

        // ---- 4. direction ---------------------------------------------------
        // A pending flip forces the side: the reversal that closed the last
        // position is the reason this one is being opened.
        int direction = 0;
        if (forced_side_valid_) {
            direction = (forced_side_ == exec::Side::buy) ? 1 : -1;
            forced_side_valid_ = false;
        } else {
            direction = entry_direction();
        }
        if (direction == 0) {
            veto_ = Veto::signal_below_threshold; ++veto_count_; return intent;
        }

        transition(StrategyState::entry_pending);
        intent.kind = IntentKind::enter;
        intent.side = (direction > 0) ? exec::Side::buy : exec::Side::sell;
        intent.volume = config_.volume;
        ++entries_;
        return intent;
    }

    void on_entry_filled(exec::Side side, std::int64_t volume, std::int64_t price_ticks,
                         std::uint64_t ts_ns) noexcept {
        if (state_ != StrategyState::entry_pending) return;
        position_.active = true;
        position_.side = side;
        position_.volume = volume;
        position_.entry_ticks = price_ticks;
        position_.opened_ns = ts_ns;
        ++open_positions_;
        transition(StrategyState::in_position);
    }

    void on_entry_rejected() noexcept {
        if (state_ == StrategyState::entry_pending) transition(StrategyState::idle);
    }

    void on_exit_filled(std::uint64_t ts_ns, std::int64_t net_minor) noexcept {
        if (state_ != StrategyState::exit_pending) return;
        position_ = OpenPosition{};
        if (open_positions_ > 0) --open_positions_;
        cooldown_until_ns_ = ts_ns + config_.cooldown_ns;
        if (net_minor < 0) ++consecutive_losses_; else consecutive_losses_ = 0;
        // Carry the reversal across the close so the next entry takes the
        // opposite side rather than re-deriving it from a momentum reading that
        // may have decayed in the milliseconds the close took.
        if (flip_pending_) {
            forced_side_ = flip_side_;
            forced_side_valid_ = true;
            flip_pending_ = false;
        }
        transition(StrategyState::cooldown);
        if (consecutive_losses_ >= config_.max_consecutive_losses) halt();
    }

    void on_exit_rejected() noexcept {
        if (state_ == StrategyState::exit_pending) transition(StrategyState::in_position);
    }

private:
    void transition(StrategyState next) noexcept { if (legal(state_, next)) state_ = next; }

    // Marked against the price the position would actually CLOSE at — bid for a
    // long, ask for a short — never the mid. `move` is therefore already net of
    // the spread; costs are subtracted on top of it.
    [[nodiscard]] ExitReason exit_due(std::int64_t bid, std::int64_t ask,
                                      std::uint64_t ts_ns) noexcept {
        const auto mark = (position_.side == exec::Side::buy) ? bid : ask;
        const auto move = (position_.side == exec::Side::buy)
            ? (mark - position_.entry_ticks) : (position_.entry_ticks - mark);
        last_net_ticks_ = move - config_.commission_ticks - config_.slippage_allowance_ticks;

        // Hard stop and time stop outrank the profit target.
        if (move <= -config_.stop_loss_ticks) return ExitReason::stop_loss;
        if (ts_ns >= position_.opened_ns &&
            (ts_ns - position_.opened_ns) >= config_.max_hold_ns)
            return ExitReason::time_stop;

        // Small POSITIVE net after costs — the whole point of the strategy.
        if (move >= config_.profit_threshold_ticks()) return ExitReason::take_profit;

        // Reversal: momentum has turned decisively against the open side.
        if (signals_.ready(L1Signal::momentum)) {
            const auto m = signals_.momentum;
            if (position_.side == exec::Side::buy && m <= -config_.flip_momentum)
                return ExitReason::signal_flip;
            if (position_.side == exec::Side::sell && m >= config_.flip_momentum)
                return ExitReason::signal_flip;
        }
        return ExitReason::none;
    }

    [[nodiscard]] int entry_direction() const noexcept {
        if (!signals_.ready(L1Signal::momentum)) return 0;
        const auto m = signals_.momentum;
        if (m >= config_.entry_momentum) return 1;
        if (m <= -config_.entry_momentum) return -1;
        return 0;
    }

    ScalpFlipConfig config_{};
    L1SignalEngine engine_{};
    L1SignalSet signals_{};
    OpenPosition position_{};
    StrategyState state_{StrategyState::cold};
    Veto veto_{Veto::none};
    ExitReason exit_reason_{ExitReason::none};
    std::uint64_t cooldown_until_ns_{0};
    std::uint32_t open_positions_{0};
    std::uint32_t consecutive_losses_{0};
    std::uint64_t entries_{0}, exits_{0}, veto_count_{0}, flips_{0};
    std::int64_t last_net_ticks_{0};
    bool flip_pending_{false};
    bool forced_side_valid_{false};
    exec::Side flip_side_{exec::Side::buy};
    exec::Side forced_side_{exec::Side::buy};
    bool valid_{false};
};

}  // namespace strategy
