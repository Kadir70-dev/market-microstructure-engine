#include <cmath>
#include <iostream>

#include "strategy/metrics.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}
bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

strategy::TradeRecord trade(std::int64_t pnl, std::int64_t commission = 0,
                            strategy::ExitReason reason = strategy::ExitReason::take_profit) {
    strategy::TradeRecord t{};
    t.volume = 1;
    t.pnl_minor = pnl;
    t.commission_minor = commission;
    t.reason = reason;
    t.opened_ns = 0;
    t.closed_ns = 1'000'000'000;   // 1 s
    return t;
}
}

int main() {
    using namespace strategy;

    // ---- realised PnL arithmetic ------------------------------------------
    check(trade_pnl_minor(exec::Side::buy, 10, 100, 120, 2) == 400, "pnl_long_gain");
    check(trade_pnl_minor(exec::Side::buy, 10, 100, 90, 2) == -200, "pnl_long_loss");
    check(trade_pnl_minor(exec::Side::sell, 10, 100, 90, 2) == 200, "pnl_short_gain");
    check(trade_pnl_minor(exec::Side::sell, 10, 100, 120, 2) == -400, "pnl_short_loss");
    check(trade_pnl_minor(exec::Side::buy, 10, 100, 100, 2) == 0, "pnl_flat");

    // ---- empty statistics --------------------------------------------------
    {
        TradeStatistics s;
        check(s.trades() == 0 && s.net_pnl_minor() == 0, "empty_stats");
        check(near(s.win_rate(), 0.0), "empty_win_rate");
        check(near(s.sharpe_per_trade(), 0.0), "empty_sharpe");
        check(s.max_drawdown_minor() == 0, "empty_drawdown");
        check(near(s.profit_factor(), 0.0), "empty_profit_factor");
    }

    // ---- win rate, gross win/loss, expectancy ------------------------------
    {
        TradeStatistics s;
        (void)s.add(trade(100));
        (void)s.add(trade(-50, 0, ExitReason::stop_loss));
        (void)s.add(trade(200));
        (void)s.add(trade(-50, 0, ExitReason::stop_loss));
        check(s.trades() == 4, "stats_trade_count");
        check(s.wins() == 2 && s.losses() == 2, "stats_win_loss_count");
        check(near(s.win_rate(), 0.5), "stats_win_rate");
        check(s.net_pnl_minor() == 200, "stats_net_pnl");
        check(s.gross_win_minor() == 300 && s.gross_loss_minor() == 100, "stats_gross");
        check(near(s.profit_factor(), 3.0), "stats_profit_factor");
        check(near(s.expectancy_minor(), 50.0), "stats_expectancy");
        check(near(s.average_win_minor(), 150.0), "stats_average_win");
        check(near(s.average_loss_minor(), 50.0), "stats_average_loss");
        check(s.take_profits() == 2 && s.stop_losses() == 2, "stats_exit_reason_counts");
        check(near(s.average_hold_seconds(), 1.0), "stats_average_hold");
    }

    // ---- commission is charged against the trade ---------------------------
    {
        TradeStatistics s;
        (void)s.add(trade(100, 30));
        check(s.net_pnl_minor() == 70, "commission_reduces_net");
        check(s.wins() == 1, "commission_still_a_win");

        TradeStatistics flipped;
        (void)flipped.add(trade(20, 30));   // gross win, net loss
        check(flipped.net_pnl_minor() == -10, "commission_can_flip_sign");
        check(flipped.losses() == 1 && flipped.wins() == 0, "commission_flip_counted_as_loss");
    }

    // ---- max drawdown ------------------------------------------------------
    {
        // Equity path: +100, +150, +50, +250. Peak 150, trough 50 -> drawdown 100.
        TradeStatistics s;
        (void)s.add(trade(100));
        (void)s.add(trade(50));
        (void)s.add(trade(-100, 0, ExitReason::stop_loss));
        (void)s.add(trade(200));
        check(s.peak_equity_minor() == 250, "drawdown_peak");
        check(s.max_drawdown_minor() == 100, "drawdown_max");
        check(s.net_pnl_minor() == 250, "drawdown_final_equity");
    }
    {
        // Monotonic gains have no drawdown.
        TradeStatistics s;
        for (int i = 0; i < 5; ++i) (void)s.add(trade(10));
        check(s.max_drawdown_minor() == 0, "drawdown_none_when_monotonic");
    }
    {
        // Drawdown is measured from the running peak, not from zero.
        TradeStatistics s;
        (void)s.add(trade(-100, 0, ExitReason::stop_loss));
        check(s.max_drawdown_minor() == 100, "drawdown_from_zero_peak");
    }

    // ---- Sharpe ------------------------------------------------------------
    {
        // Constant returns: zero variance, reported as 0 rather than infinity.
        TradeStatistics s;
        for (int i = 0; i < 5; ++i) (void)s.add(trade(10));
        check(near(s.sharpe_per_trade(), 0.0), "sharpe_zero_variance");
    }
    {
        // Hand-computable: {10, -10} -> mean 0 -> Sharpe 0.
        TradeStatistics s;
        (void)s.add(trade(10));
        (void)s.add(trade(-10, 0, ExitReason::stop_loss));
        check(near(s.sharpe_per_trade(), 0.0), "sharpe_zero_mean");
    }
    {
        // {2, 4, 4, 4, 5, 5, 7, 9}: mean 5, sample sd sqrt(32/7).
        TradeStatistics s;
        const std::int64_t data[] = {2, 4, 4, 4, 5, 5, 7, 9};
        for (auto v : data) (void)s.add(trade(v));
        const double expected = 5.0 / std::sqrt(32.0 / 7.0);
        check(near(s.sharpe_per_trade(), expected), "sharpe_hand_computed");
        check(s.sharpe_per_trade() > 0.0, "sharpe_positive_for_profitable");
    }
    {
        // A single trade has no dispersion, so Sharpe is undefined -> 0.
        TradeStatistics s;
        (void)s.add(trade(100));
        check(near(s.sharpe_per_trade(), 0.0), "sharpe_single_trade_undefined");
    }

    // ---- bounded capacity fails closed -------------------------------------
    {
        TradeStatistics s;
        std::size_t accepted = 0;
        for (std::size_t i = 0; i < max_tracked_trades + 16; ++i)
            if (s.add(trade(1))) ++accepted;
        check(accepted == max_tracked_trades, "capacity_bounded");
        check(s.overflowed(), "capacity_overflow_flagged");
    }

    // ---- clear resets everything -------------------------------------------
    {
        TradeStatistics s;
        (void)s.add(trade(100));
        (void)s.add(trade(-40, 0, ExitReason::stop_loss));
        s.clear();
        check(s.trades() == 0 && s.net_pnl_minor() == 0, "clear_resets");
        check(s.max_drawdown_minor() == 0 && s.wins() == 0, "clear_resets_derived");
    }

    return failures == 0 ? 0 : 1;
}
