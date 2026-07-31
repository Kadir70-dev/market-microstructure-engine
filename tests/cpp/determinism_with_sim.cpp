#include <iostream>

#include "replay/sim_replay_engine.hpp"
#include "replay_fixture.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}

bool same(const replay::SimReplayResult& a, const replay::SimReplayResult& b) noexcept {
    return a.stream_digest == b.stream_digest && a.journal_digest == b.journal_digest &&
           a.events == b.events && a.quotes == b.quotes &&
           a.orders_submitted == b.orders_submitted && a.orders_accepted == b.orders_accepted &&
           a.fills == b.fills && a.journal_records == b.journal_records &&
           a.balance_minor == b.balance_minor && a.equity_minor == b.equity_minor &&
           a.margin_used_minor == b.margin_used_minor &&
           a.free_margin_minor == b.free_margin_minor &&
           a.realized_pnl_minor == b.realized_pnl_minor &&
           a.unrealized_pnl_minor == b.unrealized_pnl_minor &&
           a.open_positions == b.open_positions && a.stopped_out == b.stopped_out &&
           a.halted == b.halted;
}
}

// Part 9.1 / Part 18 Phase 5: the Phase 4 determinism gate re-run with the
// simulator in the loop. 100 consecutive runs must agree on the journal AND on
// every account figure — not merely on the market-data digest, which would pass
// even if execution were nondeterministic.

int main() {
    const auto dir = replay_fixture::make_wal("sim_determinism", 6'000, 64 * 1024,
                                               1'000'000);
    if (dir.empty()) { std::cout << "fixture=FAIL\n"; return 1; }

    replay::SimReplayConfig config{};
    config.order_every_n_quotes = 50;
    replay::SimReplayEngine engine(config);

    const auto first = engine.run(dir);
    check(first.ok, "sim_first_run_ok");
    check(first.events == 6'000, "sim_all_events_replayed");
    check(first.orders_submitted > 0, "sim_orders_actually_submitted");
    check(first.fills > 0, "sim_fills_actually_occurred");
    check(first.journal_records > 0, "sim_journal_populated");
    check(!first.journal_overflowed, "sim_journal_did_not_overflow");

    std::uint64_t journal_mismatches = 0;
    std::uint64_t state_mismatches = 0;
    for (int run = 1; run < 100; ++run) {
        const auto next = engine.run(dir);
        if (!next.ok) { ++state_mismatches; continue; }
        if (next.journal_digest != first.journal_digest) ++journal_mismatches;
        if (!same(first, next)) ++state_mismatches;
    }

    check(journal_mismatches == 0, "sim_100_runs_identical_journal");
    check(state_mismatches == 0, "sim_100_runs_identical_account_state");

    // A fresh engine must agree: determinism cannot rely on residual state.
    replay::SimReplayEngine independent(config);
    check(same(first, independent.run(dir)), "sim_fresh_engine_identical");

    // Byte-for-byte journal equality, not just digest equality.
    {
        exec::Journal a, b;
        (void)a.emit(exec::JournalRecordType::fill, 1, 2, 3, 4, 5, 6, 7, 8, 9);
        (void)b.emit(exec::JournalRecordType::fill, 1, 2, 3, 4, 5, 6, 7, 8, 9);
        check(a.identical_to(b), "sim_journal_byte_identical");
        (void)b.emit(exec::JournalRecordType::fill, 1, 2, 3, 4, 5, 6, 7, 8, 10);
        check(!a.identical_to(b), "sim_journal_detects_difference");
    }

    // The digest must discriminate: a different seed must change the journal,
    // otherwise every equality above passes vacuously.
    {
        auto altered = config;
        altered.run_seed = config.run_seed + 1;
        replay::SimReplayEngine other(altered);
        const auto varied = other.run(dir);
        check(varied.ok, "sim_alt_seed_ok");
        check(varied.stream_digest == first.stream_digest,
              "sim_market_stream_independent_of_seed");
        check(varied.journal_digest != first.journal_digest,
              "sim_journal_depends_on_run_seed");
    }

    // Mode must change execution outcomes, or the three modes are cosmetic.
    {
        auto optimistic = config;
        optimistic.mode = exec::SimulationMode::optimistic;
        replay::SimReplayEngine engine_opt(optimistic);
        const auto result = engine_opt.run(dir);
        check(result.ok, "sim_optimistic_ok");
        check(result.journal_digest != first.journal_digest, "sim_mode_changes_journal");
    }

    replay_fixture::remove_all(dir);
    return failures == 0 ? 0 : 1;
}
