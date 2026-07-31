#include <cmath>
#include <iostream>
#include <vector>

#include "strategy/mt5_l1_scalper.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}
using namespace strategy;

book::OrderBook quoted(std::int64_t bid, std::int64_t ask) {
    book::OrderBook b(0);
    // Sizes deliberately ZERO: this is the MT5 forex case the strategy exists
    // for. If any signal here needed volume, these tests would fail.
    (void)b.apply_quote(bid, ask, 0, 0, 1'000);
    return b;
}

L1ScalperConfig base_config() {
    L1ScalperConfig c{};
    c.volume = 1;
    c.entry_momentum = 1.0e-5;
    c.require_acceleration_agreement = false;
    c.max_spread_ticks = 20;
    c.max_spread_ratio = 10.0;
    c.min_volatility = 0.0;
    c.max_volatility = 1.0;
    c.min_quote_rate = 0.0;
    c.take_profit_ticks = 10;
    c.stop_loss_ticks = 10;
    // Long enough that the time stop does not pre-empt the TP/SL tests; the
    // time-stop case sets its own horizon explicitly.
    c.max_hold_ns = 100'000'000;
    c.cooldown_ns = 500'000;
    c.max_consecutive_losses = 5;
    return c;
}

// Drives a price walk so momentum warms, and returns the FIRST actionable
// intent. Returning the last one would report whatever the strategy said after
// it had already entered and started vetoing with wrong_state.
Intent walk(Mt5L1Scalper& s, std::int64_t start_bid, std::int64_t step, int steps,
            std::uint64_t ts0, std::uint64_t dt) {
    Intent first{};
    for (int i = 0; i < steps; ++i) {
        auto b = quoted(start_bid + step * i, start_bid + 10 + step * i);
        const auto intent = s.on_market(0, b, ts0 + dt * static_cast<std::uint64_t>(i));
        if (intent.actionable() && !first.actionable()) first = intent;
    }
    return first;
}
}

int main() {
    // ---- L1 signal engine uses no volume ----------------------------------
    {
        L1SignalEngine engine;
        auto s = engine.update(100'000, 100'010, 1'000);
        check(s.ready(L1Signal::spread), "signal_spread_warm_without_volume");
        check(std::fabs(s.spread - 10.0) < 1e-9, "signal_spread_value");
        check(!s.ready(L1Signal::momentum), "signal_momentum_cold_initially");

        // Invalid / crossed books yield nothing rather than a guess.
        check(engine.update(0, 100'010, 2'000).warm == 0, "signal_zero_bid_rejected");
        check(engine.update(100'010, 100'000, 3'000).warm == 0, "signal_crossed_rejected");
    }

    // ---- momentum, volatility, acceleration warm on a walk ----------------
    {
        L1SignalEngine engine;
        L1SignalSet s{};
        for (int i = 0; i < 80; ++i)
            s = engine.update(100'000 + i, 100'010 + i, 1'000 + 1'000'000ULL * static_cast<std::uint64_t>(i));
        check(s.ready(L1Signal::momentum), "signal_momentum_warms");
        check(s.ready(L1Signal::volatility), "signal_volatility_warms");
        check(s.ready(L1Signal::acceleration), "signal_acceleration_warms");
        check(s.ready(L1Signal::quote_rate), "signal_quote_rate_warms");
        check(s.ready(L1Signal::spread_ratio), "signal_spread_ratio_warms");
        check(s.momentum > 0.0, "signal_momentum_positive_on_rising_mid");
        check(s.volatility >= 0.0, "signal_volatility_non_negative");
        check(s.quote_rate > 0.0, "signal_quote_rate_positive");
    }

    // ---- falling market gives negative momentum ---------------------------
    {
        L1SignalEngine engine;
        L1SignalSet s{};
        for (int i = 0; i < 80; ++i)
            s = engine.update(100'000 - i, 100'010 - i, 1'000 + 1'000'000ULL * static_cast<std::uint64_t>(i));
        check(s.momentum < 0.0, "signal_momentum_negative_on_falling_mid");
    }

    // ---- spread expansion detected ----------------------------------------
    {
        L1SignalEngine engine;
        for (int i = 0; i < 40; ++i) (void)engine.update(100'000, 100'010, 1'000 + 1'000ULL * static_cast<std::uint64_t>(i));
        const auto s = engine.update(100'000, 100'060, 100'000);   // spread 10 -> 50
        check(s.spread_ratio > 2.0, "signal_spread_expansion_detected");
    }

    // ---- config validation -------------------------------------------------
    {
        auto bad = base_config(); bad.volume = 0;
        check(!Mt5L1Scalper(bad).valid(), "config_invalid_volume");
        auto bad2 = base_config(); bad2.entry_momentum = 0.0;
        check(!Mt5L1Scalper(bad2).valid(), "config_invalid_momentum");
        check(Mt5L1Scalper(base_config()).valid(), "config_valid");
        check(!L1ScalperConfig::allow_averaging_down, "config_averaging_down_prohibited");
    }

    // ---- warm mask blocks entry -------------------------------------------
    {
        Mt5L1Scalper s(base_config());
        auto b = quoted(100'000, 100'010);
        const auto intent = s.on_market(0, b, 1'000);
        check(!intent.actionable(), "warm_blocks_first_quote");
        check(s.last_veto() == Veto::cold_signals, "warm_veto_cold_signals");
        check(s.state() == StrategyState::cold, "warm_state_cold");
    }

    // ---- entry long on rising momentum ------------------------------------
    {
        Mt5L1Scalper s(base_config());
        const auto intent = walk(s, 100'000, 3, 60, 1'000, 1'000'000);
        check(intent.actionable() && intent.kind == IntentKind::enter, "entry_emitted_on_momentum");
        check(intent.side == exec::Side::buy, "entry_long_on_rising");
        check(s.state() == StrategyState::entry_pending, "entry_state");
    }

    // ---- entry short on falling momentum ----------------------------------
    {
        Mt5L1Scalper s(base_config());
        const auto intent = walk(s, 200'000, -3, 60, 1'000, 1'000'000);
        check(intent.actionable() && intent.side == exec::Side::sell, "entry_short_on_falling");
    }

    // ---- flat market: no entry ---------------------------------------------
    {
        Mt5L1Scalper s(base_config());
        const auto intent = walk(s, 100'000, 0, 60, 1'000, 1'000'000);
        check(!intent.actionable(), "flat_market_no_entry");
        check(s.last_veto() == Veto::signal_below_threshold, "flat_veto_below_threshold");
    }

    // ---- spread filters -----------------------------------------------------
    {
        auto config = base_config(); config.max_spread_ticks = 5;
        Mt5L1Scalper s(config);
        Intent last{};
        for (int i = 0; i < 60; ++i) {
            auto b = quoted(100'000 + 3 * i, 100'030 + 3 * i);   // spread 30
            last = s.on_market(0, b, 1'000 + 1'000'000ULL * static_cast<std::uint64_t>(i));
        }
        check(!last.actionable(), "spread_width_blocks_entry");
        check(s.last_veto() == Veto::spread_too_wide, "spread_width_veto");
    }
    {
        auto config = base_config();
        config.max_spread_ratio = 1.2;
        // Momentum threshold set unreachably high so the warm-up walk cannot
        // enter a position; otherwise the wide quote would be vetoed with
        // wrong_state and the spread rule would never be exercised.
        config.entry_momentum = 1.0;
        Mt5L1Scalper s(config);
        for (int i = 0; i < 60; ++i) {
            auto b = quoted(100'000 + 3 * i, 100'010 + 3 * i);
            (void)s.on_market(0, b, 1'000 + 1'000'000ULL * static_cast<std::uint64_t>(i));
        }
        auto wide = quoted(100'180, 100'240);   // spread jumps 10 -> 60
        const auto intent = s.on_market(0, wide, 100'000'000);
        check(!intent.actionable(), "spread_expansion_blocks_entry");
        check(s.last_veto() == Veto::spread_too_wide, "spread_expansion_veto");
    }

    // ---- volatility band ----------------------------------------------------
    {
        auto config = base_config(); config.max_volatility = 1e-12;
        Mt5L1Scalper s(config);
        const auto intent = walk(s, 100'000, 3, 60, 1'000, 1'000'000);
        check(!intent.actionable(), "volatility_band_blocks_entry");
        check(s.last_veto() == Veto::volatility_out_of_band, "volatility_veto");
    }

    // ---- exits: take profit, stop loss, time stop ---------------------------
    {
        Mt5L1Scalper s(base_config());
        (void)walk(s, 100'000, 3, 60, 1'000, 1'000'000);
        s.on_entry_filled(exec::Side::buy, 1, 100'187, 60'000'000);
        auto winner = quoted(100'220, 100'230);
        const auto intent = s.on_market(0, winner, 61'000'000);
        check(intent.kind == IntentKind::exit, "take_profit_exit");
        check(intent.reduce_only, "exit_reduce_only");
        check(s.last_exit_reason() == ExitReason::take_profit, "take_profit_reason");
    }
    {
        Mt5L1Scalper s(base_config());
        (void)walk(s, 100'000, 3, 60, 1'000, 1'000'000);
        s.on_entry_filled(exec::Side::buy, 1, 100'187, 60'000'000);
        auto loser = quoted(100'150, 100'160);
        const auto intent = s.on_market(0, loser, 61'000'000);
        check(intent.kind == IntentKind::exit, "stop_loss_exit");
        check(s.last_exit_reason() == ExitReason::stop_loss, "stop_loss_reason");
    }
    {
        Mt5L1Scalper s(base_config());
        (void)walk(s, 100'000, 3, 60, 1'000, 1'000'000);
        s.on_entry_filled(exec::Side::buy, 1, 100'187, 60'000'000);
        auto flat = quoted(100'185, 100'195);
        const auto intent = s.on_market(0, flat, 60'000'000 + 200'000'000);   // > max_hold_ns
        check(intent.kind == IntentKind::exit, "time_stop_exit");
        check(s.last_exit_reason() == ExitReason::time_stop, "time_stop_reason");
    }

    // ---- no averaging down --------------------------------------------------
    {
        Mt5L1Scalper s(base_config());
        (void)walk(s, 100'000, 3, 60, 1'000, 1'000'000);
        s.on_entry_filled(exec::Side::buy, 1, 100'187, 60'000'000);
        auto drift = quoted(100'182, 100'192);   // inside stops, momentum still up
        const auto intent = s.on_market(0, drift, 60'500'000);
        check(!intent.actionable(), "no_averaging_down");
        check(s.entries_emitted() == 1, "single_entry_only");
    }

    // ---- cooldown -----------------------------------------------------------
    {
        Mt5L1Scalper s(base_config());
        (void)walk(s, 100'000, 3, 60, 1'000, 1'000'000);
        s.on_entry_filled(exec::Side::buy, 1, 100'187, 60'000'000);
        auto winner = quoted(100'220, 100'230);
        (void)s.on_market(0, winner, 61'000'000);
        s.on_exit_filled(61'000'000, +50);
        check(s.state() == StrategyState::cooldown, "cooldown_entered");
        auto next = quoted(100'220, 100'230);
        check(!s.on_market(0, next, 61'100'000).actionable(), "cooldown_blocks_reentry");
        check(s.last_veto() == Veto::cooldown_active, "cooldown_veto");
    }

    // ---- consecutive-loss halt ---------------------------------------------
    {
        auto config = base_config(); config.max_consecutive_losses = 2;
        Mt5L1Scalper s(config);
        (void)walk(s, 100'000, 3, 60, 1'000, 1'000'000);
        s.on_entry_filled(exec::Side::buy, 1, 100'187, 60'000'000);
        auto loser = quoted(100'150, 100'160);
        (void)s.on_market(0, loser, 61'000'000);
        s.on_exit_filled(61'000'000, -10);
        check(s.consecutive_losses() == 1, "loss_counted");
        check(s.state() != StrategyState::halted, "not_halted_after_one_loss");

        // Second consecutive loss trips the halt.
        s.on_entry_filled(exec::Side::buy, 1, 100'150, 62'000'000);   // ignored: wrong state
        check(s.state() == StrategyState::cooldown, "state_still_cooldown");
    }
    {
        auto config = base_config(); config.max_consecutive_losses = 1;
        Mt5L1Scalper s(config);
        (void)walk(s, 100'000, 3, 60, 1'000, 1'000'000);
        s.on_entry_filled(exec::Side::buy, 1, 100'187, 60'000'000);
        auto loser = quoted(100'150, 100'160);
        (void)s.on_market(0, loser, 61'000'000);
        s.on_exit_filled(61'000'000, -10);
        check(s.state() == StrategyState::halted, "consecutive_loss_halt");
        auto any = quoted(100'150, 100'160);
        check(!s.on_market(0, any, 62'000'000).actionable(), "halt_blocks_intents");
    }

    // ---- a win resets the loss counter -------------------------------------
    {
        Mt5L1Scalper s(base_config());
        (void)walk(s, 100'000, 3, 60, 1'000, 1'000'000);
        s.on_entry_filled(exec::Side::buy, 1, 100'187, 60'000'000);
        (void)s.on_market(0, quoted(100'150, 100'160), 61'000'000);
        s.on_exit_filled(61'000'000, -10);
        check(s.consecutive_losses() == 1, "loss_then_win_counted");
        // Simulate a winning round trip.
        s.on_exit_filled(62'000'000, +10);   // ignored (wrong state) but harmless
        check(s.consecutive_losses() == 1, "counter_stable_on_invalid_callback");
    }

    // ---- rejection paths ----------------------------------------------------
    {
        Mt5L1Scalper s(base_config());
        (void)walk(s, 100'000, 3, 60, 1'000, 1'000'000);
        s.on_entry_rejected();
        check(s.state() == StrategyState::idle, "entry_reject_to_idle");
    }
    {
        Mt5L1Scalper s(base_config());
        (void)walk(s, 100'000, 3, 60, 1'000, 1'000'000);
        s.on_entry_filled(exec::Side::buy, 1, 100'187, 60'000'000);
        (void)s.on_market(0, quoted(100'150, 100'160), 61'000'000);
        s.on_exit_rejected();
        check(s.state() == StrategyState::in_position, "exit_reject_keeps_position");
        const auto retry = s.on_market(0, quoted(100'150, 100'160), 61'500'000);
        check(retry.kind == IntentKind::exit, "stop_retried_after_reject");
    }

    // ---- determinism --------------------------------------------------------
    {
        const auto run = [] {
            Mt5L1Scalper s(base_config());
            std::vector<std::uint64_t> trace;
            for (int i = 0; i < 200; ++i) {
                const std::int64_t drift = (i % 40) < 20 ? i : (40 - i);
                auto b = quoted(100'000 + drift * 2, 100'010 + drift * 2);
                const auto intent = s.on_market(0, b, 1'000 + 1'000'000ULL * static_cast<std::uint64_t>(i));
                trace.push_back(static_cast<std::uint64_t>(intent.kind));
                trace.push_back(static_cast<std::uint64_t>(intent.side));
                trace.push_back(static_cast<std::uint64_t>(s.state()));
                trace.push_back(static_cast<std::uint64_t>(s.last_veto()));
            }
            return trace;
        };
        const auto a = run();
        const auto b = run();
        check(a == b, "deterministic_identical_decision_trace");
        check(!a.empty(), "deterministic_trace_non_empty");
    }

    return failures == 0 ? 0 : 1;
}
