#pragma once

#include <cstdint>

#include "book/order_book.hpp"
#include "strategy/l1_signals.hpp"
#include "strategy/strategy_types.hpp"

// mt5_l1_scalper_v1 — PAPER ONLY.
//
// A separate strategy, not a variant of the depth-based baseline, which is left
// untouched. It trades only what an MT5 L1 forex feed genuinely publishes:
// bid/ask, spread and time. No queue imbalance, no OFI, no volume, no
// synthesised depth.
//
// It emits Intents. It cannot construct an exec::Order — the Approval token
// Oms::create requires is constructible only by RiskEngine — so there is no
// type-legal path from here to an order, paper or otherwise.
//
// Decision order matches the baseline and is load-bearing:
//   1. halt      2. exits (unconditional)   3. cooldown   4. entries (gated)
//
// Exits are evaluated first and are NOT gated on the warm mask. A stop that
// stops working when signals go cold is not a stop, and cold signals are what a
// disorderly market produces.

namespace strategy {

struct L1ScalperConfig final {
    std::uint32_t strategy_id{2};
    std::int64_t volume{1};

    // ---- entry ----
    // Momentum is a mean log return per quote; forex ticks are ~1e-5 moves, so
    // the default is deliberately small. It is a structural default, not a
    // fitted value — nothing here has been optimised against any dataset.
    double entry_momentum{2.0e-6};
    bool require_acceleration_agreement{true};
    std::int64_t max_spread_ticks{20};
    double max_spread_ratio{1.50};      // refuse entries while the spread is expanding
    double min_volatility{1.0e-7};
    double max_volatility{5.0e-5};
    double min_quote_rate{0.5};         // quotes/second; liquidity proxy without volume

    // ---- exit ----
    // A position is marked against the side it would CLOSE at, so it opens at
    // roughly -spread and can be pushed further by entry slippage. A stop at or
    // near the spread is therefore hit on the first tick: the audit showed a
    // trade stopped out 2 ms after entry. The stop must clear spread + slippage,
    // and the target must clear round-trip cost (~spread + commission) or a win
    // is arithmetically impossible.
    std::int64_t take_profit_ticks{60};
    std::int64_t stop_loss_ticks{40};
    // Time stop was ending nearly every trade before the target could be
    // reached; lengthened so the target is actually reachable.
    std::uint64_t max_hold_ns{300'000'000'000ULL};

    // ---- gates ----
    std::uint64_t cooldown_ns{30'000'000'000ULL};
    std::uint32_t max_positions{1};
    std::uint32_t max_consecutive_losses{5};

    std::uint32_t required_signals{l1_bit(L1Signal::spread) | l1_bit(L1Signal::momentum) |
                                   l1_bit(L1Signal::volatility)};

    static constexpr bool allow_averaging_down = false;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return volume > 0 && max_positions >= 1 && take_profit_ticks > 0 &&
               stop_loss_ticks > 0 && max_hold_ns > 0 && max_spread_ticks > 0 &&
               entry_momentum > 0.0 && max_spread_ratio > 0.0 &&
               min_volatility >= 0.0 && max_volatility >= min_volatility &&
               min_quote_rate >= 0.0 && max_consecutive_losses > 0;
    }
};

class Mt5L1Scalper final {
public:
    explicit Mt5L1Scalper(L1ScalperConfig config) noexcept
        : config_(config), valid_(config.valid()) {}

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] StrategyState state() const noexcept { return state_; }
    [[nodiscard]] Veto last_veto() const noexcept { return veto_; }
    [[nodiscard]] ExitReason last_exit_reason() const noexcept { return exit_reason_; }
    [[nodiscard]] const OpenPosition& position() const noexcept { return position_; }
    [[nodiscard]] const L1SignalSet& signals() const noexcept { return signals_; }
    [[nodiscard]] std::uint64_t entries_emitted() const noexcept { return entries_; }
    [[nodiscard]] std::uint64_t exits_emitted() const noexcept { return exits_; }
    [[nodiscard]] std::uint64_t vetoes() const noexcept { return veto_count_; }
    [[nodiscard]] std::uint32_t consecutive_losses() const noexcept { return consecutive_losses_; }

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

        // ---- 1. exits, first and unconditional ----------------------------
        if (state_ == StrategyState::in_position && position_.active) {
            const auto reason = exit_due(bid, ask, ts_ns);
            if (reason != ExitReason::none) {
                exit_reason_ = reason;
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

        // ---- 2. cooldown --------------------------------------------------
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

        if (position_.active && !L1ScalperConfig::allow_averaging_down) {
            veto_ = Veto::averaging_down; ++veto_count_; return intent;
        }
        if (open_positions_ >= config_.max_positions) {
            veto_ = Veto::max_positions; ++veto_count_; return intent;
        }

        // ---- 4. filters ---------------------------------------------------
        if ((ask - bid) > config_.max_spread_ticks) {
            veto_ = Veto::spread_too_wide; ++veto_count_; return intent;
        }
        // Refuse to enter while the spread is expanding: widening quotes are the
        // L1-visible signature of a venue pulling liquidity, which is exactly
        // when a market entry is most expensive.
        if (signals_.ready(L1Signal::spread_ratio) &&
            signals_.spread_ratio > config_.max_spread_ratio) {
            veto_ = Veto::spread_too_wide; ++veto_count_; return intent;
        }
        if (signals_.volatility < config_.min_volatility ||
            signals_.volatility > config_.max_volatility) {
            veto_ = Veto::volatility_out_of_band; ++veto_count_; return intent;
        }
        if (signals_.ready(L1Signal::quote_rate) &&
            signals_.quote_rate < config_.min_quote_rate) {
            veto_ = Veto::volatility_out_of_band; ++veto_count_; return intent;
        }

        const auto direction = entry_direction();
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

    // `net_minor` lets the strategy honour a consecutive-loss halt without
    // reaching into the broker: the runtime reports the realised result.
    void on_exit_filled(std::uint64_t ts_ns, std::int64_t net_minor) noexcept {
        if (state_ != StrategyState::exit_pending) return;
        position_ = OpenPosition{};
        if (open_positions_ > 0) --open_positions_;
        cooldown_until_ns_ = ts_ns + config_.cooldown_ns;
        if (net_minor < 0) ++consecutive_losses_; else consecutive_losses_ = 0;
        transition(StrategyState::cooldown);
        if (consecutive_losses_ >= config_.max_consecutive_losses) halt();
    }

    void on_exit_rejected() noexcept {
        if (state_ == StrategyState::exit_pending) transition(StrategyState::in_position);
    }

private:
    void transition(StrategyState next) noexcept { if (legal(state_, next)) state_ = next; }

    // Marked against the price the position would actually close at — bid for a
    // long, ask for a short — never the mid.
    [[nodiscard]] ExitReason exit_due(std::int64_t bid, std::int64_t ask,
                                      std::uint64_t ts_ns) const noexcept {
        if (ts_ns >= position_.opened_ns &&
            (ts_ns - position_.opened_ns) >= config_.max_hold_ns)
            return ExitReason::time_stop;
        const auto mark = (position_.side == exec::Side::buy) ? bid : ask;
        const auto move = (position_.side == exec::Side::buy)
            ? (mark - position_.entry_ticks) : (position_.entry_ticks - mark);
        if (move >= config_.take_profit_ticks) return ExitReason::take_profit;
        if (move <= -config_.stop_loss_ticks) return ExitReason::stop_loss;
        return ExitReason::none;
    }

    // +1 long, -1 short, 0 stand aside. Momentum sets direction; acceleration
    // must agree when required, so the strategy joins a move that is still
    // building rather than one already decaying.
    [[nodiscard]] int entry_direction() const noexcept {
        if (!signals_.ready(L1Signal::momentum)) return 0;
        const auto m = signals_.momentum;
        int direction = 0;
        if (m >= config_.entry_momentum) direction = 1;
        else if (m <= -config_.entry_momentum) direction = -1;
        if (direction == 0) return 0;

        if (config_.require_acceleration_agreement) {
            if (!signals_.ready(L1Signal::acceleration)) return 0;
            if (direction > 0 && signals_.acceleration <= 0.0) return 0;
            if (direction < 0 && signals_.acceleration >= 0.0) return 0;
        }
        return direction;
    }

    L1ScalperConfig config_{};
    L1SignalEngine engine_{};
    L1SignalSet signals_{};
    OpenPosition position_{};
    StrategyState state_{StrategyState::cold};
    Veto veto_{Veto::none};
    ExitReason exit_reason_{ExitReason::none};
    std::uint64_t cooldown_until_ns_{0};
    std::uint32_t open_positions_{0};
    std::uint32_t consecutive_losses_{0};
    std::uint64_t entries_{0}, exits_{0}, veto_count_{0};
    bool valid_{false};
};

}  // namespace strategy
