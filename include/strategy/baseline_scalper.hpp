#pragma once

#include <cstdint>

#include "book/order_book.hpp"
#include "features/feature_engine.hpp"
#include "strategy/signals.hpp"
#include "strategy/strategy_types.hpp"

// Phase 7 — baseline scalper.
//
// PAPER ONLY. Emits Intents; it cannot construct an order, cannot reach a
// broker, and has no transport of any kind. The Approval token that Oms::create
// demands is constructible only by RiskEngine, so there is no type-legal path
// from this class to an order.
//
// Decision order is deliberate and load-bearing:
//   1. halt          — nothing else runs
//   2. exits         — evaluated FIRST and unconditionally
//   3. cooldown      — gates entries only
//   4. entries       — gated on warm signals and every filter
//
// Exits precede entries and are not gated on the warm mask because Part 18
// Phase 7 requires the time stop and hard stop to be "always honoured". A stop
// that stops working when features go cold is not a stop, and cold features are
// exactly what a disorderly market produces.

namespace strategy {

class BaselineScalper final {
public:
    explicit BaselineScalper(StrategyConfig config) noexcept
        : config_(config), valid_(config.valid()) {}

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] StrategyState state() const noexcept { return state_; }
    [[nodiscard]] Veto last_veto() const noexcept { return veto_; }
    [[nodiscard]] ExitReason last_exit_reason() const noexcept { return exit_reason_; }
    [[nodiscard]] const OpenPosition& position() const noexcept { return position_; }
    [[nodiscard]] const SignalSet& signals() const noexcept { return signals_; }
    [[nodiscard]] std::uint64_t entries_emitted() const noexcept { return entries_; }
    [[nodiscard]] std::uint64_t exits_emitted() const noexcept { return exits_; }
    [[nodiscard]] std::uint64_t vetoes() const noexcept { return veto_count_; }

    void halt() noexcept {
        if (state_ != StrategyState::halted) state_ = StrategyState::halted;
    }

    // Single decision point. Pure with respect to wall time: every timestamp is
    // the replayed virtual time, so a decision is reproducible.
    [[nodiscard]] Intent on_market(std::uint32_t symbol_id, const book::OrderBook& b,
                                   const features::FeatureVector& fv,
                                   book::BookSource source, std::uint64_t ts_ns) noexcept {
        Intent intent{};
        intent.strategy_id = config_.strategy_id;
        intent.symbol_id = symbol_id;
        intent.ts_ns = ts_ns;
        veto_ = Veto::none;

        if (!valid_ || state_ == StrategyState::halted) {
            veto_ = Veto::halted;
            ++veto_count_;
            return intent;
        }

        signals_ = engine_.compute(b, fv, source);
        const auto top = top_of_book(b);

        // ---- 1. exits, first and unconditional ----------------------------
        if (state_ == StrategyState::in_position && position_.active) {
            const auto reason = exit_due(top, ts_ns);
            if (reason != ExitReason::none) {
                exit_reason_ = reason;
                transition(StrategyState::exit_pending);
                intent.kind = IntentKind::exit;
                // Closing is the opposite side of the open, and reduce_only so
                // risk rejects anything that would increase exposure.
                intent.side = (position_.side == exec::Side::buy) ? exec::Side::sell
                                                                  : exec::Side::buy;
                intent.volume = position_.volume;
                intent.reduce_only = true;
                ++exits_;
                return intent;
            }
            return intent;   // hold
        }

        // ---- 2. cooldown expiry ------------------------------------------
        if (state_ == StrategyState::cooldown) {
            if (ts_ns >= cooldown_until_ns_) transition(StrategyState::idle);
            else { veto_ = Veto::cooldown_active; ++veto_count_; return intent; }
        }

        // ---- 3. warm-up ---------------------------------------------------
        const bool warm = (signals_.warm & config_.required_signals) == config_.required_signals;
        if (state_ == StrategyState::cold) {
            if (!warm) { veto_ = Veto::cold_signals; ++veto_count_; return intent; }
            transition(StrategyState::idle);
        }
        if (!warm) { veto_ = Veto::cold_signals; ++veto_count_; return intent; }

        if (state_ != StrategyState::idle) { veto_ = Veto::wrong_state; ++veto_count_; return intent; }

        // Averaging down is prohibited. Structurally unreachable while
        // max_positions is 1 and entries only leave `idle`, but asserted
        // explicitly so the prohibition survives a future config change.
        if (position_.active && !StrategyConfig::allow_averaging_down) {
            veto_ = Veto::averaging_down;
            ++veto_count_;
            return intent;
        }
        if (open_positions_ >= config_.max_positions) {
            veto_ = Veto::max_positions;
            ++veto_count_;
            return intent;
        }

        // ---- 4. filters ---------------------------------------------------
        if (!top.valid) { veto_ = Veto::cold_signals; ++veto_count_; return intent; }
        if (top.spread_ticks() > config_.max_spread_ticks) {
            veto_ = Veto::spread_too_wide;
            ++veto_count_;
            return intent;
        }
        if (signals_.ready(Signal::volatility)) {
            if (signals_.volatility < config_.min_volatility ||
                signals_.volatility > config_.max_volatility) {
                veto_ = Veto::volatility_out_of_band;
                ++veto_count_;
                return intent;
            }
        }

        const auto direction = entry_direction();
        if (direction == 0) {
            veto_ = Veto::signal_below_threshold;
            ++veto_count_;
            return intent;
        }

        transition(StrategyState::entry_pending);
        intent.kind = IntentKind::enter;
        intent.side = (direction > 0) ? exec::Side::buy : exec::Side::sell;
        intent.volume = config_.volume;
        intent.reduce_only = false;
        ++entries_;
        return intent;
    }

    // ---- execution callbacks ---------------------------------------------

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

    void on_exit_filled(std::uint64_t ts_ns) noexcept {
        if (state_ != StrategyState::exit_pending) return;
        position_ = OpenPosition{};
        if (open_positions_ > 0) --open_positions_;
        cooldown_until_ns_ = ts_ns + config_.cooldown_ns;
        transition(StrategyState::cooldown);
    }

    void on_exit_rejected() noexcept {
        // Back to in_position with the stop intact; the next tick re-evaluates.
        if (state_ == StrategyState::exit_pending) transition(StrategyState::in_position);
    }

private:
    void transition(StrategyState next) noexcept {
        if (legal(state_, next)) state_ = next;
    }

    // Take profit, hard stop, then time stop. Marked against the price the
    // position would actually close at — the bid for a long, the ask for a
    // short — never the mid, which would flatter every exit by half the spread.
    [[nodiscard]] ExitReason exit_due(const TopOfBook& top, std::uint64_t ts_ns) const noexcept {
        if (ts_ns >= position_.opened_ns &&
            (ts_ns - position_.opened_ns) >= config_.max_hold_ns)
            return ExitReason::time_stop;
        if (!top.valid) return ExitReason::none;

        const auto mark = (position_.side == exec::Side::buy) ? top.bid_ticks : top.ask_ticks;
        const auto move = (position_.side == exec::Side::buy)
            ? (mark - position_.entry_ticks) : (position_.entry_ticks - mark);
        if (move >= config_.take_profit_ticks) return ExitReason::take_profit;
        if (move <= -config_.stop_loss_ticks) return ExitReason::stop_loss;
        return ExitReason::none;
    }

    // +1 long, -1 short, 0 stand aside. Queue imbalance sets the direction; OFI
    // must agree in sign when required, because imbalance alone is a snapshot
    // while OFI carries the direction of the change that produced it.
    [[nodiscard]] int entry_direction() const noexcept {
        if (!signals_.ready(Signal::queue_imbalance)) return 0;
        const auto qi = signals_.queue_imbalance;
        int direction = 0;
        if (qi >= config_.entry_queue_imbalance) direction = 1;
        else if (qi <= -config_.entry_queue_imbalance) direction = -1;
        if (direction == 0) return 0;

        if (config_.require_ofi_agreement) {
            if (!signals_.ready(Signal::ofi)) return 0;
            const auto ofi = signals_.ofi;
            if (direction > 0 && !(ofi > 0.0 && ofi >= config_.entry_ofi)) return 0;
            if (direction < 0 && !(ofi < 0.0 && -ofi >= config_.entry_ofi)) return 0;
        }
        return direction;
    }

    StrategyConfig config_{};
    SignalEngine engine_{};
    SignalSet signals_{};
    OpenPosition position_{};
    StrategyState state_{StrategyState::cold};
    Veto veto_{Veto::none};
    ExitReason exit_reason_{ExitReason::none};
    std::uint64_t cooldown_until_ns_{0};
    std::uint32_t open_positions_{0};
    std::uint64_t entries_{0};
    std::uint64_t exits_{0};
    std::uint64_t veto_count_{0};
    bool valid_{false};
};

}  // namespace strategy
