#include <iostream>

#include "replay/sim_replay_engine.hpp"
#include "replay_fixture.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}
}

// Part 8.6 restart recovery: "journal replay -> OMS/PMS rebuild -> broker
// reconcile -> resolve every UNKNOWN -> only then permit new orders."
//
// SCOPE LIMITATION, STATED PLAINLY: the OMS-rebuild-from-journal step is NOT
// implemented in Phase 5. It needs a durable journal reader, which is Phase 6
// work (Part 11.1 puts the journal writer there), and broker reconciliation
// needs a broker, which Phase 5 deliberately does not have.
//
// What IS testable now, and is tested here, is the property that rebuild depends
// on: a restarted run over the same inputs reconstructs byte-identical state.
// If that failed, no journal-driven rebuild could ever be correct.

int main() {
    const auto dir = replay_fixture::make_wal("restart", 4'000, 64 * 1024, 1'000'000);
    if (dir.empty()) { std::cout << "fixture=FAIL\n"; return 1; }

    replay::SimReplayConfig config{};
    config.order_every_n_quotes = 40;

    // A "restart" is a fresh engine and a fresh broker over the same recorded
    // history. Reconstructed state must match the original exactly.
    replay::SimReplayEngine original(config);
    const auto before = original.run(dir);
    check(before.ok, "restart_original_run_ok");
    check(before.fills > 0, "restart_original_had_fills");

    replay::SimReplayEngine restarted(config);
    const auto after = restarted.run(dir);
    check(after.ok, "restart_recovered_run_ok");

    check(after.journal_digest == before.journal_digest, "restart_journal_reconstructed");
    check(after.journal_records == before.journal_records, "restart_journal_record_count");
    check(after.balance_minor == before.balance_minor, "restart_balance_reconstructed");
    check(after.equity_minor == before.equity_minor, "restart_equity_reconstructed");
    check(after.margin_used_minor == before.margin_used_minor, "restart_margin_reconstructed");
    check(after.realized_pnl_minor == before.realized_pnl_minor, "restart_realized_pnl");
    check(after.unrealized_pnl_minor == before.unrealized_pnl_minor, "restart_unrealized_pnl");
    check(after.open_positions == before.open_positions, "restart_positions_reconstructed");
    check(after.fills == before.fills, "restart_fill_count");
    check(after.stopped_out == before.stopped_out, "restart_stop_out_state");

    // A broker constructed fresh starts flat, so recovery is genuinely a
    // reconstruction rather than residual state surviving in the process.
    {
        exec::BrokerConfig cfg{};
        cfg.initial_balance_minor = 500;
        exec::PaperBroker fresh(cfg);
        check(fresh.account().balance_minor == 500, "restart_fresh_broker_balance");
        check(fresh.account().open_positions == 0, "restart_fresh_broker_flat");
        check(fresh.account().open_orders == 0, "restart_fresh_broker_no_orders");
        check(fresh.journal().size() == 0, "restart_fresh_broker_empty_journal");
        check(!fresh.halted(), "restart_fresh_broker_not_halted");
    }

    // UNKNOWN must survive a rebuild as non-terminal holding full exposure
    // (Part 8.3, Invariant 5) — the invariant a resolver would depend on.
    check(!exec::is_terminal(exec::OrderState::unknown), "restart_unknown_not_terminal");
    check(exec::reserves_exposure(exec::OrderState::unknown), "restart_unknown_holds_exposure");
    check(!exec::reserves_exposure(exec::OrderState::filled), "restart_terminal_releases_exposure");

    replay_fixture::remove_all(dir);
    return failures == 0 ? 0 : 1;
}
