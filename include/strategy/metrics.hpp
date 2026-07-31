#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "exec/exec_types.hpp"
#include "strategy/strategy_types.hpp"

// Phase 7 — trade statistics.
//
// PnL is integer minor units throughout, so it is bit-reproducible across runs
// and the Phase 4/5 determinism gates continue to hold. Only the ratio metrics
// (Sharpe, profit factor, win rate) are floating point, and they are derived
// from the integer series rather than accumulated in double.
//
// Sharpe here is PER TRADE: mean(trade PnL) / stddev(trade PnL). It is NOT
// annualised. Annualising requires a trades-per-year figure, and inventing one
// from a 17-minute capture would manufacture a number with no evidence behind
// it. Part 22 asks for Sharpe at the gate; that gate also requires three months
// of data, at which point the scaling factor is measured rather than assumed.

namespace strategy {

inline constexpr std::size_t max_tracked_trades = 4096;

struct TradeRecord final {
    exec::Side side{exec::Side::buy};
    std::int64_t volume{0};
    std::int64_t entry_ticks{0};
    std::int64_t exit_ticks{0};
    std::int64_t pnl_minor{0};
    std::int64_t commission_minor{0};
    std::uint64_t opened_ns{0};
    std::uint64_t closed_ns{0};
    ExitReason reason{ExitReason::none};

    [[nodiscard]] constexpr std::int64_t net_minor() const noexcept {
        return pnl_minor - commission_minor;
    }
    [[nodiscard]] constexpr std::uint64_t hold_ns() const noexcept {
        return closed_ns > opened_ns ? closed_ns - opened_ns : 0;
    }
};

class TradeStatistics final {
public:
    // Returns false when full rather than wrapping: a silently truncated trade
    // log would understate drawdown, which is the one statistic a reader most
    // needs to trust.
    [[nodiscard]] bool add(const TradeRecord& trade) noexcept {
        if (count_ >= max_tracked_trades) { overflowed_ = true; return false; }
        trades_[count_++] = trade;
        const auto net = trade.net_minor();
        cumulative_ += net;
        if (cumulative_ > peak_) peak_ = cumulative_;
        const auto drawdown = peak_ - cumulative_;
        if (drawdown > max_drawdown_) max_drawdown_ = drawdown;
        if (net > 0) { ++wins_; gross_win_ += net; }
        else if (net < 0) { ++losses_; gross_loss_ += -net; }
        else { ++flats_; }
        switch (trade.reason) {
            case ExitReason::take_profit: ++take_profits_; break;
            case ExitReason::stop_loss:   ++stop_losses_;  break;
            case ExitReason::time_stop:   ++time_stops_;   break;
            default: break;
        }
        return true;
    }

    [[nodiscard]] std::size_t trades() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t wins() const noexcept { return wins_; }
    [[nodiscard]] std::uint64_t losses() const noexcept { return losses_; }
    [[nodiscard]] std::uint64_t flats() const noexcept { return flats_; }
    [[nodiscard]] std::uint64_t take_profits() const noexcept { return take_profits_; }
    [[nodiscard]] std::uint64_t stop_losses() const noexcept { return stop_losses_; }
    [[nodiscard]] std::uint64_t time_stops() const noexcept { return time_stops_; }
    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }

    [[nodiscard]] std::int64_t net_pnl_minor() const noexcept { return cumulative_; }
    [[nodiscard]] std::int64_t gross_win_minor() const noexcept { return gross_win_; }
    [[nodiscard]] std::int64_t gross_loss_minor() const noexcept { return gross_loss_; }
    [[nodiscard]] std::int64_t max_drawdown_minor() const noexcept { return max_drawdown_; }
    [[nodiscard]] std::int64_t peak_equity_minor() const noexcept { return peak_; }

    [[nodiscard]] double win_rate() const noexcept {
        const auto decided = wins_ + losses_;
        return decided > 0 ? static_cast<double>(wins_) / static_cast<double>(decided) : 0.0;
    }

    // Undefined with no losses; reported as 0 rather than infinity so a caller
    // cannot accidentally print "inf" as a result.
    [[nodiscard]] double profit_factor() const noexcept {
        return gross_loss_ > 0 ? static_cast<double>(gross_win_) / static_cast<double>(gross_loss_)
                               : 0.0;
    }

    [[nodiscard]] double average_win_minor() const noexcept {
        return wins_ > 0 ? static_cast<double>(gross_win_) / static_cast<double>(wins_) : 0.0;
    }
    [[nodiscard]] double average_loss_minor() const noexcept {
        return losses_ > 0 ? static_cast<double>(gross_loss_) / static_cast<double>(losses_) : 0.0;
    }
    [[nodiscard]] double expectancy_minor() const noexcept {
        return count_ > 0 ? static_cast<double>(cumulative_) / static_cast<double>(count_) : 0.0;
    }

    // Per-trade Sharpe. Two passes over a bounded array: exact, and immune to
    // the cancellation a single-pass sum-of-squares would suffer on a large mean.
    [[nodiscard]] double sharpe_per_trade() const noexcept {
        if (count_ < 2) return 0.0;
        double mean = 0.0;
        for (std::size_t i = 0; i < count_; ++i)
            mean += static_cast<double>(trades_[i].net_minor());
        mean /= static_cast<double>(count_);
        double variance = 0.0;
        for (std::size_t i = 0; i < count_; ++i) {
            const auto d = static_cast<double>(trades_[i].net_minor()) - mean;
            variance += d * d;
        }
        variance /= static_cast<double>(count_ - 1);
        if (!(variance > 0.0)) return 0.0;
        return mean / std::sqrt(variance);
    }

    [[nodiscard]] double average_hold_seconds() const noexcept {
        if (count_ == 0) return 0.0;
        double total = 0.0;
        for (std::size_t i = 0; i < count_; ++i)
            total += static_cast<double>(trades_[i].hold_ns());
        return total / static_cast<double>(count_) / 1e9;
    }

    [[nodiscard]] const TradeRecord& at(std::size_t index) const noexcept { return trades_[index]; }

    void clear() noexcept {
        count_ = 0; wins_ = losses_ = flats_ = 0;
        take_profits_ = stop_losses_ = time_stops_ = 0;
        cumulative_ = peak_ = max_drawdown_ = gross_win_ = gross_loss_ = 0;
        overflowed_ = false;
    }

private:
    std::array<TradeRecord, max_tracked_trades> trades_{};
    std::size_t count_{0};
    std::uint64_t wins_{0}, losses_{0}, flats_{0};
    std::uint64_t take_profits_{0}, stop_losses_{0}, time_stops_{0};
    std::int64_t cumulative_{0}, peak_{0}, max_drawdown_{0};
    std::int64_t gross_win_{0}, gross_loss_{0};
    bool overflowed_{false};
};

// Realised PnL of a round trip, integer minor units.
[[nodiscard]] constexpr std::int64_t trade_pnl_minor(exec::Side side, std::int64_t volume,
                                                     std::int64_t entry_ticks,
                                                     std::int64_t exit_ticks,
                                                     std::int64_t tick_value_minor) noexcept {
    const auto move = (side == exec::Side::buy) ? (exit_ticks - entry_ticks)
                                                : (entry_ticks - exit_ticks);
    return move * volume * tick_value_minor;
}

}  // namespace strategy
