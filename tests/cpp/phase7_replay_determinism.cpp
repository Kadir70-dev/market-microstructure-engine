#include <iostream>

#include "replay/strategy_replay_engine.hpp"
#include "replay_fixture.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}

bool same(const replay::StrategyReplayResult& a,
          const replay::StrategyReplayResult& b) noexcept {
    return a.decision_digest == b.decision_digest && a.journal_digest == b.journal_digest &&
           a.events == b.events && a.quotes == b.quotes && a.intents == b.intents &&
           a.entries == b.entries && a.exits == b.exits && a.vetoes == b.vetoes &&
           a.risk_rejects == b.risk_rejects && a.broker_rejects == b.broker_rejects &&
           a.fills == b.fills && a.trades == b.trades &&
           a.net_pnl_minor == b.net_pnl_minor &&
           a.max_drawdown_minor == b.max_drawdown_minor &&
           a.balance_minor == b.balance_minor && a.equity_minor == b.equity_minor;
}

replay::StrategyReplayConfig make_config() {
    replay::StrategyReplayConfig config{};
    config.strategy.volume = 1;
    config.strategy.entry_queue_imbalance = 0.20;
    config.strategy.require_ofi_agreement = false;
    config.strategy.max_spread_ticks = 1'000;
    config.strategy.take_profit_ticks = 5;
    config.strategy.stop_loss_ticks = 5;
    config.strategy.max_hold_ns = 50'000;
    config.strategy.cooldown_ns = 10'000;
    config.strategy.required_signals =
        strategy::signal_bit(strategy::Signal::queue_imbalance) |
        strategy::signal_bit(strategy::Signal::spread);

    // Left at defaults: self_test() requires required_warm_mask != 0, so zeroing
    // it would invalidate the engine and turn every check into a rejection.
    config.limits = risk::Limits{};
    config.mode = exec::SimulationMode::optimistic;   // deterministic, no reject noise
    return config;
}
}

// Phase 7 objective 9: deterministic replay must produce identical trading
// decisions. The decision digest commits to every decision including the
// decisions NOT to act, so two runs that stood aside for different reasons
// cannot hash the same.

int main() {
    const auto dir = replay_fixture::make_wal("phase7", 6'000, 64 * 1024, 1'000'000);
    if (dir.empty()) { std::cout << "fixture=FAIL\n"; return 1; }

    const auto config = make_config();
    replay::StrategyReplayEngine engine(config);

    const auto first = engine.run(dir);
    check(first.ok, "phase7_first_run_ok");
    check(first.events == 6'000, "phase7_all_events_replayed");
    check(first.quotes > 0, "phase7_quotes_seen");

    // The run must actually exercise the strategy, or determinism is vacuous.
    check(first.intents > 0 || first.vetoes > 0, "phase7_strategy_decided");

    std::uint64_t decision_mismatches = 0;
    std::uint64_t state_mismatches = 0;
    for (int run = 1; run < 100; ++run) {
        const auto next = engine.run(dir);
        if (!next.ok) { ++state_mismatches; continue; }
        if (next.decision_digest != first.decision_digest) ++decision_mismatches;
        if (!same(first, next)) ++state_mismatches;
    }
    check(decision_mismatches == 0, "phase7_100_runs_identical_decisions");
    check(state_mismatches == 0, "phase7_100_runs_identical_results");

    // A fresh engine must agree: determinism cannot depend on residual state.
    replay::StrategyReplayEngine independent(config);
    check(same(first, independent.run(dir)), "phase7_fresh_engine_identical");

    // Discriminating: a different entry threshold must change the decisions,
    // otherwise every equality above passes vacuously.
    {
        auto altered = config;
        altered.strategy.entry_queue_imbalance = 0.99;
        replay::StrategyReplayEngine other(altered);
        const auto varied = other.run(dir);
        check(varied.ok, "phase7_alt_threshold_ok");
        check(varied.decision_digest != first.decision_digest,
              "phase7_decisions_depend_on_config");
    }

    // Statistics must be internally consistent with the trades recorded.
    check(first.trades <= first.fills, "phase7_trades_not_exceeding_fills");
    check(first.max_drawdown_minor >= 0, "phase7_drawdown_non_negative");
    check(first.win_rate >= 0.0 && first.win_rate <= 1.0, "phase7_win_rate_in_range");

    std::cout << "phase7_report events=" << first.events
              << " quotes=" << first.quotes
              << " intents=" << first.intents
              << " entries=" << first.entries
              << " exits=" << first.exits
              << " vetoes=" << first.vetoes
              << " risk_rejects=" << first.risk_rejects
              << " broker_rejects=" << first.broker_rejects
              << " fills=" << first.fills
              << " trades=" << first.trades
              << " net_pnl_minor=" << first.net_pnl_minor
              << " max_drawdown_minor=" << first.max_drawdown_minor
              << " win_rate=" << first.win_rate
              << " sharpe_per_trade=" << first.sharpe_per_trade
              << " profit_factor=" << first.profit_factor << '\n';

    replay_fixture::remove_all(dir);
    return failures == 0 ? 0 : 1;
}
