#include <iostream>

#include "strategy/baseline_scalper.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}
using namespace strategy;

book::OrderBook quoted(std::int64_t bid, std::int64_t ask, std::int64_t bid_size,
                       std::int64_t ask_size) {
    book::OrderBook b(0);
    (void)b.apply_quote(bid, ask, bid_size, ask_size, 1'000);
    return b;
}

StrategyConfig base_config() {
    StrategyConfig c{};
    c.volume = 1;
    c.entry_queue_imbalance = 0.30;
    c.entry_ofi = 0.0;
    c.require_ofi_agreement = false;   // isolate the imbalance rule
    c.max_spread_ticks = 20;
    c.take_profit_ticks = 10;
    c.stop_loss_ticks = 10;
    c.max_hold_ns = 1'000'000;
    c.cooldown_ns = 500'000;
    c.max_positions = 1;
    c.required_signals = signal_bit(Signal::queue_imbalance) | signal_bit(Signal::spread);
    return c;
}

// Drives one decision. Feature engine is fed alongside so the warm mask evolves
// exactly as it would in replay.
Intent step(BaselineScalper& s, features::FeatureEngine& fe, book::OrderBook& b,
            std::uint64_t ts, book::BookSource src = book::BookSource::dom_aggregated) {
    const auto fv = fe.compute(b, src);
    return s.on_market(0, b, fv, src, ts);
}
}

int main() {
    // ---- FSM legality ------------------------------------------------------
    {
        using S = StrategyState;
        check(legal(S::cold, S::idle), "fsm_cold_to_idle");
        check(!legal(S::cold, S::in_position), "fsm_cold_cannot_jump_to_position");
        check(legal(S::idle, S::entry_pending), "fsm_idle_to_entry");
        check(!legal(S::idle, S::in_position), "fsm_no_position_without_fill");
        check(legal(S::entry_pending, S::in_position), "fsm_entry_fill");
        check(legal(S::entry_pending, S::idle), "fsm_entry_reject");
        check(legal(S::in_position, S::exit_pending), "fsm_position_to_exit");
        check(!legal(S::in_position, S::idle), "fsm_position_cannot_vanish");
        check(legal(S::exit_pending, S::cooldown), "fsm_exit_fill");
        check(legal(S::exit_pending, S::in_position), "fsm_exit_reject_returns");
        check(legal(S::cooldown, S::idle), "fsm_cooldown_expiry");
        check(legal(S::in_position, S::halted), "fsm_halt_always_reachable");
        check(!legal(S::halted, S::idle), "fsm_halt_requires_manual_rearm");
    }

    // ---- invalid config refuses to trade -----------------------------------
    {
        StrategyConfig bad = base_config();
        bad.volume = 0;
        BaselineScalper s(bad);
        check(!s.valid(), "config_invalid_rejected");
        features::FeatureEngine fe(2);
        auto b = quoted(100, 110, 40, 10);
        check(!step(s, fe, b, 1'000).actionable(), "config_invalid_emits_nothing");
    }

    // ---- warm mask blocks entry --------------------------------------------
    {
        BaselineScalper s(base_config());
        features::FeatureEngine fe(2);
        auto b = quoted(100, 110, 0, 0);      // no size -> queue imbalance cold
        const auto intent = step(s, fe, b, 1'000, book::BookSource::l1_only);
        check(!intent.actionable(), "warm_mask_blocks_entry");
        check(s.last_veto() == Veto::cold_signals, "warm_mask_veto_recorded");
        check(s.state() == StrategyState::cold, "warm_mask_stays_cold");
    }

    // ---- entry on imbalance -------------------------------------------------
    {
        BaselineScalper s(base_config());
        features::FeatureEngine fe(2);
        auto b = quoted(100, 110, 40, 10);    // imbalance +0.6 -> long
        const auto intent = step(s, fe, b, 1'000);
        check(intent.actionable() && intent.kind == IntentKind::enter, "entry_emitted");
        check(intent.side == exec::Side::buy, "entry_long_on_bid_heavy");
        check(intent.volume == 1, "entry_volume");
        check(!intent.reduce_only, "entry_not_reduce_only");
        check(s.state() == StrategyState::entry_pending, "entry_state");
    }
    {
        BaselineScalper s(base_config());
        features::FeatureEngine fe(2);
        auto b = quoted(100, 110, 10, 40);    // imbalance -0.6 -> short
        const auto intent = step(s, fe, b, 1'000);
        check(intent.side == exec::Side::sell, "entry_short_on_ask_heavy");
    }

    // ---- below-threshold imbalance stands aside ----------------------------
    {
        BaselineScalper s(base_config());
        features::FeatureEngine fe(2);
        auto b = quoted(100, 110, 26, 24);    // imbalance 0.04
        check(!step(s, fe, b, 1'000).actionable(), "below_threshold_no_entry");
        check(s.last_veto() == Veto::signal_below_threshold, "below_threshold_veto");
    }

    // ---- spread filter ------------------------------------------------------
    {
        auto config = base_config();
        config.max_spread_ticks = 5;
        BaselineScalper s(config);
        features::FeatureEngine fe(2);
        auto b = quoted(100, 130, 40, 10);    // spread 30 > 5
        check(!step(s, fe, b, 1'000).actionable(), "spread_filter_blocks");
        check(s.last_veto() == Veto::spread_too_wide, "spread_filter_veto");
    }

    // ---- OFI agreement ------------------------------------------------------
    {
        auto config = base_config();
        config.require_ofi_agreement = true;
        BaselineScalper s(config);
        features::FeatureEngine fe(2);
        // First snapshot: OFI cold, so no entry regardless of imbalance.
        auto b1 = quoted(100, 110, 40, 10);
        check(!step(s, fe, b1, 1'000).actionable(), "ofi_required_blocks_first_tick");
        // Second snapshot with the bid growing: OFI positive, imbalance positive.
        auto b2 = quoted(100, 110, 60, 10);
        const auto intent = step(s, fe, b2, 2'000);
        check(intent.actionable() && intent.side == exec::Side::buy, "ofi_agreement_permits_entry");
    }
    {
        auto config = base_config();
        config.require_ofi_agreement = true;
        BaselineScalper s(config);
        features::FeatureEngine fe(2);
        auto b1 = quoted(100, 110, 40, 10);
        (void)step(s, fe, b1, 1'000);
        // Bid heavy (long bias) but the bid is shrinking: OFI disagrees.
        auto b2 = quoted(100, 110, 20, 10);
        check(!step(s, fe, b2, 2'000).actionable(), "ofi_disagreement_blocks_entry");
    }

    // ---- take profit, stop loss, time stop ---------------------------------
    {
        BaselineScalper s(base_config());
        features::FeatureEngine fe(2);
        auto b = quoted(100, 110, 40, 10);
        (void)step(s, fe, b, 1'000);
        s.on_entry_filled(exec::Side::buy, 1, 110, 1'000);
        check(s.state() == StrategyState::in_position, "tp_in_position");

        auto profitable = quoted(125, 135, 40, 10);   // bid 125 vs entry 110 = +15
        const auto intent = step(s, fe, profitable, 2'000);
        check(intent.kind == IntentKind::exit, "take_profit_exit");
        check(intent.side == exec::Side::sell, "take_profit_opposite_side");
        check(intent.reduce_only, "exit_is_reduce_only");
        check(s.last_exit_reason() == ExitReason::take_profit, "take_profit_reason");
    }
    {
        BaselineScalper s(base_config());
        features::FeatureEngine fe(2);
        auto b = quoted(100, 110, 40, 10);
        (void)step(s, fe, b, 1'000);
        s.on_entry_filled(exec::Side::buy, 1, 110, 1'000);
        auto losing = quoted(95, 105, 40, 10);        // bid 95 vs entry 110 = -15
        const auto intent = step(s, fe, losing, 2'000);
        check(intent.kind == IntentKind::exit, "stop_loss_exit");
        check(s.last_exit_reason() == ExitReason::stop_loss, "stop_loss_reason");
    }
    {
        BaselineScalper s(base_config());
        features::FeatureEngine fe(2);
        auto b = quoted(100, 110, 40, 10);
        (void)step(s, fe, b, 1'000);
        s.on_entry_filled(exec::Side::buy, 1, 110, 1'000);
        auto flat = quoted(108, 112, 40, 10);         // inside both stops
        const auto intent = step(s, fe, flat, 1'000 + 2'000'000);   // past max_hold_ns
        check(intent.kind == IntentKind::exit, "time_stop_exit");
        check(s.last_exit_reason() == ExitReason::time_stop, "time_stop_reason");
    }

    // ---- stops are honoured even when signals go cold ----------------------
    {
        BaselineScalper s(base_config());
        features::FeatureEngine fe(2);
        auto b = quoted(100, 110, 40, 10);
        (void)step(s, fe, b, 1'000);
        s.on_entry_filled(exec::Side::buy, 1, 110, 1'000);
        // Size disappears: queue imbalance goes cold. The stop must still fire.
        auto cold_but_losing = quoted(95, 105, 0, 0);
        const auto intent = step(s, fe, cold_but_losing, 2'000, book::BookSource::l1_only);
        check(intent.kind == IntentKind::exit, "stop_honoured_with_cold_signals");
        check(s.last_exit_reason() == ExitReason::stop_loss, "stop_honoured_reason");
    }

    // ---- cooldown -----------------------------------------------------------
    {
        auto config = base_config();
        config.cooldown_ns = 1'000'000;
        BaselineScalper s(config);
        features::FeatureEngine fe(2);
        auto b = quoted(100, 110, 40, 10);
        (void)step(s, fe, b, 1'000);
        s.on_entry_filled(exec::Side::buy, 1, 110, 1'000);
        auto exit_book = quoted(125, 135, 40, 10);
        (void)step(s, fe, exit_book, 2'000);
        s.on_exit_filled(2'000);
        check(s.state() == StrategyState::cooldown, "cooldown_entered");

        auto again = quoted(100, 110, 40, 10);
        check(!step(s, fe, again, 500'000).actionable(), "cooldown_blocks_reentry");
        check(s.last_veto() == Veto::cooldown_active, "cooldown_veto");
        const auto after = step(s, fe, again, 2'000 + 1'000'000);
        check(after.actionable(), "cooldown_expires");
    }

    // ---- max positions / no averaging down ---------------------------------
    {
        BaselineScalper s(base_config());
        features::FeatureEngine fe(2);
        auto b = quoted(100, 110, 40, 10);
        (void)step(s, fe, b, 1'000);
        s.on_entry_filled(exec::Side::buy, 1, 110, 1'000);
        // Price falls but stays inside the stop: a strategy that averaged down
        // would add here. This one must not, and must emit nothing.
        auto drifting = quoted(103, 113, 40, 10);
        const auto intent = step(s, fe, drifting, 1'500);
        check(!intent.actionable(), "no_averaging_down_while_in_position");
        check(s.entries_emitted() == 1, "no_averaging_down_single_entry");
        check(!StrategyConfig::allow_averaging_down, "averaging_down_prohibited_by_config");
    }

    // ---- rejection paths ----------------------------------------------------
    {
        BaselineScalper s(base_config());
        features::FeatureEngine fe(2);
        auto b = quoted(100, 110, 40, 10);
        (void)step(s, fe, b, 1'000);
        s.on_entry_rejected();
        check(s.state() == StrategyState::idle, "entry_reject_returns_to_idle");
        check(!s.position().active, "entry_reject_no_position");
    }
    {
        BaselineScalper s(base_config());
        features::FeatureEngine fe(2);
        auto b = quoted(100, 110, 40, 10);
        (void)step(s, fe, b, 1'000);
        s.on_entry_filled(exec::Side::buy, 1, 110, 1'000);
        auto losing = quoted(95, 105, 40, 10);
        (void)step(s, fe, losing, 2'000);
        s.on_exit_rejected();
        check(s.state() == StrategyState::in_position, "exit_reject_keeps_position");
        // The stop must re-fire on the next tick rather than be forgotten.
        const auto retry = step(s, fe, losing, 3'000);
        check(retry.kind == IntentKind::exit, "exit_retried_after_reject");
    }

    // ---- halt ---------------------------------------------------------------
    {
        BaselineScalper s(base_config());
        features::FeatureEngine fe(2);
        s.halt();
        auto b = quoted(100, 110, 40, 10);
        check(!step(s, fe, b, 1'000).actionable(), "halt_blocks_all_intents");
        check(s.state() == StrategyState::halted, "halt_state_sticky");
    }

    return failures == 0 ? 0 : 1;
}
