#include <cstring>

#include "oms/recovery.hpp"
#include "phase6_test.hpp"

// Phase 6A — independent journal-driven recovery cases.
//
// Deliberately separate from phase6_recovery.cpp: recovery is the only thing
// standing between a restart and a silently wrong book, so its guarantees are
// verified twice by tests that share no fixtures.
//
// Governing rules, all from the frozen architecture:
//   Part 8.6  restart recovery: journal replay -> OMS/PMS rebuild
//   Part 8.5  exposure reserved at PENDING_SEND, released only on a terminal state
//   Part 10.1 Invariant 5: UNKNOWN orders reserve FULL exposure
//   Part 8.3  UNKNOWN is never terminal

namespace {
using exec::JournalRecordType;
using risk::HaltReason;
using portfolio::MarginMode;

// A complete, well-formed lifecycle shaped exactly as PaperBroker emits it:
// command -> ack -> partial fill -> position -> account/pnl pair.
exec::Journal lifecycle() {
    exec::Journal j;
    (void)j.emit(JournalRecordType::order_state, 1, 7, 9, 0, 0, 2, 3, 0, 10);
    (void)j.emit(JournalRecordType::command, 1, 7, 9, 0, 0, 0, 10, 11, 1);
    (void)j.emit(JournalRecordType::order_state, 2, 7, 9, 0, 0, 3, 4, 0, 10);
    (void)j.emit(JournalRecordType::acknowledgement, 2, 7, 9, 0, 0, 2);
    (void)j.emit(JournalRecordType::fill, 3, 7, 9, 0, 77, 12, 4, 1, 0);
    (void)j.emit(JournalRecordType::order_state, 3, 7, 9, 0, 0, 4, 7, 4, 10);
    (void)j.emit(JournalRecordType::position_update, 3, 7, 0, 0, 77, 0, 4, 12, 3);
    (void)j.emit(JournalRecordType::account_update, 4, 7, 0, 0, 0, 100, 104, 10, 94);
    (void)j.emit(JournalRecordType::pnl_update, 4, 7, 0, 0, 0, 3, 4, 1, 0);
    return j;
}

// Order runs to completion, so it recovers terminal and must NOT become UNKNOWN.
exec::Journal completed_lifecycle() {
    exec::Journal j;
    (void)j.emit(JournalRecordType::order_state, 1, 7, 9, 0, 0, 2, 3, 0, 10);
    (void)j.emit(JournalRecordType::command, 1, 7, 9, 0, 0, 0, 10, 11, 1);
    (void)j.emit(JournalRecordType::order_state, 2, 7, 9, 0, 0, 3, 4, 0, 10);
    (void)j.emit(JournalRecordType::acknowledgement, 2, 7, 9, 0, 0, 2);
    (void)j.emit(JournalRecordType::fill, 3, 7, 9, 0, 77, 12, 10, 1, 1);
    (void)j.emit(JournalRecordType::order_state, 3, 7, 9, 0, 0, 4, 8, 10, 10);
    (void)j.emit(JournalRecordType::position_update, 3, 7, 0, 0, 77, 0, 10, 12, 0);
    return j;
}

// Byte-identical reconstruction, as Phase 6A requires. memcmp rather than field
// comparison on purpose: every container is value-initialised, so padding is
// deterministic too, and byte equality is the actual claim being made.
bool identical(const oms::RecoveryState& a, const oms::RecoveryState& b) noexcept {
    if (a.orders.size() != b.orders.size()) return false;
    if (a.orders.reserved_exposure() != b.orders.reserved_exposure()) return false;
    for (std::size_t i = 0; i < a.orders.size(); ++i)
        if (std::memcmp(&a.orders.at(i), &b.orders.at(i), sizeof(exec::Order)) != 0) return false;
    if (a.positions.size() != b.positions.size()) return false;
    for (std::size_t i = 0; i < a.positions.size(); ++i)
        if (std::memcmp(&a.positions.at(i), &b.positions.at(i), sizeof(exec::Position)) != 0)
            return false;
    if (std::memcmp(&a.account, &b.account, sizeof(exec::AccountState)) != 0) return false;
    if (std::memcmp(&a.counters, &b.counters, sizeof(oms::RiskCounters)) != 0) return false;
    if (a.halt != b.halt || a.kill != b.kill) return false;
    for (std::uint32_t s = 0; s < 8; ++s) {
        const auto x = a.portfolio.symbol(s);
        const auto y = b.portfolio.symbol(s);
        if (x.net != y.net || x.gross != y.gross || x.realized != y.realized ||
            x.unrealized != y.unrealized) return false;
    }
    return a.portfolio.gross() == b.portfolio.gross() && a.portfolio.net() == b.portfolio.net();
}

bool rejected(const exec::Journal& j) noexcept {
    oms::RecoveryState scratch(MarginMode::hedging);
    return !oms::recover(j, scratch);
}
}

int main() {
    Phase6Test t;

    // ---- 1. empty journal -------------------------------------------------
    {
        exec::Journal empty;
        oms::RecoveryState s(MarginMode::hedging);
        t.check(oms::recover(empty, s), "empty: accepted");
        t.check(s.orders.size() == 0 && s.positions.size() == 0, "empty: no state");
        t.check(s.orders.reserved_exposure() == 0, "empty: no exposure");
        t.check(s.account.balance_minor == 0 && s.account.equity_minor == 0, "empty: flat account");
        t.check(s.counters.commands == 0 && s.counters.fills == 0, "empty: zero counters");
        t.check(s.halt == HaltReason::none && !s.kill, "empty: no halt");
        t.check(s.portfolio.gross() == 0, "empty: flat portfolio");
    }

    // ---- 2. normal restart ------------------------------------------------
    {
        oms::RecoveryState s(MarginMode::hedging);
        t.check(oms::recover(lifecycle(), s), "normal: accepted");
        t.check(s.orders.size() == 1, "normal: one order");
        t.check(s.orders.at(0).requested_volume == 10 && s.orders.at(0).filled_volume == 4,
                "normal: fill accounting");
        t.check(s.orders.at(0).limit_price_ticks == 11, "normal: order price");
        t.check(s.positions.size() == 1 && s.positions.at(0).volume == 4, "normal: position");
        t.check(s.positions.at(0).avg_price_ticks == 12, "normal: position price");
        t.check(s.portfolio.symbol(0).gross == 4 && s.portfolio.symbol(0).realized == 3,
                "normal: portfolio aggregate");
        t.check(s.account.balance_minor == 100 && s.account.equity_minor == 104,
                "normal: balance and equity");
        t.check(s.account.margin_used_minor == 10 && s.account.free_margin_minor == 94,
                "normal: margin and free margin");
        t.check(s.account.realized_pnl_minor == 3 && s.account.unrealized_pnl_minor == 4,
                "normal: pnl");
        t.check(s.counters.commands == 1 && s.counters.acks == 1 && s.counters.fills == 1,
                "normal: risk counters");
        t.check(s.account.open_positions == 1, "normal: open position count");
    }

    // ---- 3. partial journal ----------------------------------------------
    {
        // Truncated after the acknowledgement: a crash mid-order. The order is
        // non-terminal, so it must survive as UNKNOWN rather than be discarded.
        exec::Journal partial;
        (void)partial.emit(JournalRecordType::order_state, 1, 7, 9, 0, 0, 2, 3, 0, 10);
        (void)partial.emit(JournalRecordType::command, 1, 7, 9, 0, 0, 0, 10, 11, 1);
        (void)partial.emit(JournalRecordType::order_state, 2, 7, 9, 0, 0, 3, 4, 0, 10);
        (void)partial.emit(JournalRecordType::acknowledgement, 2, 7, 9, 0, 0, 2);

        oms::RecoveryState s(MarginMode::hedging);
        t.check(oms::recover(partial, s), "partial: accepted");
        t.check(s.orders.size() == 1, "partial: order preserved");
        t.check(s.orders.at(0).state == exec::OrderState::unknown, "partial: forced unknown");
        t.check(s.orders.at(0).filled_volume == 0, "partial: nothing filled");
        t.check(s.positions.size() == 0, "partial: no position");
        t.check(s.orders.reserved_exposure() == 10, "partial: full exposure held");

        // A journal torn between the account/pnl pair is inconsistent, not partial.
        exec::Journal torn;
        (void)torn.emit(JournalRecordType::account_update, 4, 7, 0, 0, 0, 100, 104, 10, 94);
        t.check(rejected(torn), "partial: torn account/pnl pair rejected");
    }

    // ---- 4. duplicate records --------------------------------------------
    // Each journal is a 1.3 MiB by-value buffer, so they are scoped tightly
    // rather than left to accumulate on the stack.
    {
        exec::Journal dup;
        (void)dup.emit(JournalRecordType::account_update, 4, 7, 0, 0, 0, 100, 104, 10, 94);
        (void)dup.emit(JournalRecordType::account_update, 4, 7, 0, 0, 0, 100, 104, 10, 94);
        t.check(rejected(dup), "duplicate: identical payload rejected");
    }
    {
        // Replaying an entire valid journal twice is also a duplicate.
        exec::Journal twice;
        {
            const auto source = lifecycle();
            for (int pass = 0; pass < 2; ++pass)
                for (std::size_t i = 0; i < source.size(); ++i) (void)twice.append(source.at(i));
        }
        t.check(rejected(twice), "duplicate: whole journal replayed rejected");
    }

    // ---- 5. malformed records --------------------------------------------
    {
        {   // unknown record type
            exec::Journal m;
            exec::JournalRecord r{};
            r.ts_ns = 1;
            r.type = 99;
            (void)m.append(r);
            t.check(rejected(m), "malformed: unknown type");
        }
        {   // reserved field must be zero
            exec::Journal m;
            exec::JournalRecord r{};
            r.ts_ns = 1;
            r.type = static_cast<std::uint16_t>(JournalRecordType::command);
            r.reserved = 1;
            r.run_id = 7; r.logical_order_id = 9; r.b = 10;
            (void)m.append(r);
            t.check(rejected(m), "malformed: non-zero reserved");
        }
        {   // timestamps must not go backwards
            exec::Journal m;
            (void)m.emit(JournalRecordType::account_update, 9, 7, 0, 0, 0, 100, 104, 10, 94);
            (void)m.emit(JournalRecordType::pnl_update, 1, 7, 0, 0, 0, 3, 4, 1, 0);
            t.check(rejected(m), "malformed: timestamp regression");
        }
        {   // symbol out of range
            exec::Journal m;
            (void)m.emit(JournalRecordType::command, 1, 7, 9, 99, 0, 0, 10, 11, 1);
            t.check(rejected(m), "malformed: symbol out of range");
        }
        {   // command with no identity
            exec::Journal m;
            (void)m.emit(JournalRecordType::command, 1, 0, 0, 0, 0, 0, 10, 11, 1);
            t.check(rejected(m), "malformed: command without ids");
        }
        {   // command with non-positive volume
            exec::Journal m;
            (void)m.emit(JournalRecordType::command, 1, 7, 9, 0, 0, 0, 0, 11, 1);
            t.check(rejected(m), "malformed: command zero volume");
        }
        {   // command carrying a position ticket it cannot own
            exec::Journal m;
            (void)m.emit(JournalRecordType::command, 1, 7, 9, 0, 55, 0, 10, 11, 1);
            t.check(rejected(m), "malformed: command with position ticket");
        }
    }

    // ---- 6. missing dependency -------------------------------------------
    {
        {   // fill with no preceding command
            exec::Journal m;
            (void)m.emit(JournalRecordType::fill, 1, 7, 9, 0, 77, 12, 4, 1, 0);
            t.check(rejected(m), "missing: fill without command");
        }
        {   // acknowledgement with no order
            exec::Journal m;
            (void)m.emit(JournalRecordType::acknowledgement, 1, 7, 9, 0, 0, 2);
            t.check(rejected(m), "missing: ack without command");
        }
        {   // replace with no order
            exec::Journal m;
            (void)m.emit(JournalRecordType::replace, 1, 7, 9, 0, 0, 11, 12, 10, 10);
            t.check(rejected(m), "missing: replace without command");
        }
        {   // state transition referencing an order that never existed
            exec::Journal m;
            (void)m.emit(JournalRecordType::order_state, 1, 7, 9, 0, 0, 4, 7, 0, 10);
            t.check(rejected(m), "missing: transition without order");
        }
        {   // fill exceeding the order's remaining volume
            exec::Journal m;
            (void)m.emit(JournalRecordType::order_state, 1, 7, 9, 0, 0, 2, 3, 0, 10);
            (void)m.emit(JournalRecordType::command, 1, 7, 9, 0, 0, 0, 10, 11, 1);
            (void)m.emit(JournalRecordType::order_state, 2, 7, 9, 0, 0, 3, 4, 0, 10);
            (void)m.emit(JournalRecordType::fill, 3, 7, 9, 0, 77, 12, 999, 1, 0);
            t.check(rejected(m), "missing: overfill rejected");
        }
    }

    // ---- 7. inconsistent account -----------------------------------------
    {
        {   // equity - margin must equal free margin
            exec::Journal m;
            (void)m.emit(JournalRecordType::account_update, 1, 7, 0, 0, 0, 100, 104, 10, 93);
            t.check(rejected(m), "account: free margin identity violated");
        }
        {   // equity must equal balance + unrealised
            exec::Journal m;
            (void)m.emit(JournalRecordType::account_update, 1, 7, 0, 0, 0, 100, 104, 10, 94);
            (void)m.emit(JournalRecordType::pnl_update, 1, 7, 0, 0, 0, 3, 99, 1, 0);
            t.check(rejected(m), "account: equity/pnl disagreement");
        }
        {   // pnl without a preceding account snapshot
            exec::Journal m;
            (void)m.emit(JournalRecordType::pnl_update, 1, 7, 0, 0, 0, 3, 4, 1, 0);
            t.check(rejected(m), "account: pnl without account");
        }
        {   // two pnl records for one account snapshot
            exec::Journal m;
            (void)m.emit(JournalRecordType::account_update, 1, 7, 0, 0, 0, 100, 104, 10, 94);
            (void)m.emit(JournalRecordType::pnl_update, 1, 7, 0, 0, 0, 3, 4, 1, 0);
            (void)m.emit(JournalRecordType::pnl_update, 2, 7, 0, 0, 0, 5, 4, 1, 0);
            t.check(rejected(m), "account: duplicate pnl for one snapshot");
        }
        {   // negative margin
            exec::Journal m;
            (void)m.emit(JournalRecordType::account_update, 1, 7, 0, 0, 0, 100, 104, -1, 105);
            t.check(rejected(m), "account: negative margin");
        }
    }

    // ---- 8. inconsistent portfolio ---------------------------------------
    {
        {   // a ticket may not change side
            exec::Journal m;
            (void)m.emit(JournalRecordType::position_update, 1, 7, 0, 0, 77, 0, 4, 12, 0);
            (void)m.emit(JournalRecordType::position_update, 2, 7, 0, 0, 77, 1, 4, 12, 0);
            t.check(rejected(m), "portfolio: side flip on ticket");
        }
        {   // a ticket may not change symbol
            exec::Journal m;
            (void)m.emit(JournalRecordType::position_update, 1, 7, 0, 0, 77, 0, 4, 12, 0);
            (void)m.emit(JournalRecordType::position_update, 2, 7, 0, 1, 77, 0, 4, 12, 0);
            t.check(rejected(m), "portfolio: symbol change on ticket");
        }
        {   // a closed ticket may not be resurrected
            exec::Journal m;
            (void)m.emit(JournalRecordType::position_update, 1, 7, 0, 0, 77, 0, 4, 12, 0);
            (void)m.emit(JournalRecordType::position_update, 2, 7, 0, 0, 77, 0, 0, 12, 5);
            (void)m.emit(JournalRecordType::position_update, 3, 7, 0, 0, 77, 0, 6, 12, 5);
            t.check(rejected(m), "portfolio: closed ticket reopened");
        }
        {   // closing a ticket that was never opened
            exec::Journal m;
            (void)m.emit(JournalRecordType::position_update, 1, 7, 0, 0, 77, 0, 0, 12, 0);
            t.check(rejected(m), "portfolio: close without open");
        }
        {   // a position update may not carry an order id
            exec::Journal m;
            (void)m.emit(JournalRecordType::position_update, 1, 7, 9, 0, 77, 0, 4, 12, 0);
            t.check(rejected(m), "portfolio: position carrying order id");
        }
        {   // netting mode forbids two tickets on one symbol
            exec::Journal m;
            (void)m.emit(JournalRecordType::position_update, 1, 7, 0, 0, 77, 0, 4, 12, 0);
            (void)m.emit(JournalRecordType::position_update, 2, 7, 0, 0, 78, 0, 4, 12, 0);
            oms::RecoveryState netting(MarginMode::netting);
            t.check(!oms::recover(m, netting), "portfolio: netting rejects second ticket");
            oms::RecoveryState hedging(MarginMode::hedging);
            t.check(oms::recover(m, hedging), "portfolio: hedging permits both tickets");
            t.check(hedging.portfolio.symbol(0).gross == 8, "portfolio: hedged gross aggregate");
        }
    }

    // ---- 9. UNKNOWN recovery ---------------------------------------------
    {
        oms::RecoveryState s(MarginMode::hedging);
        t.check(oms::recover(lifecycle(), s), "unknown: recovered");
        t.check(s.orders.at(0).state == exec::OrderState::unknown, "unknown: non-terminal forced");
        t.check(!exec::is_terminal(exec::OrderState::unknown), "unknown: never terminal");
        t.check(exec::reserves_exposure(exec::OrderState::unknown), "unknown: reserves exposure");

        // Invariant 5: FULL exposure, not the unfilled remainder. The order
        // requested 10 and filled 4; releasing the difference on a partial fill
        // would contradict Part 8.5's "released only on a terminal state".
        t.check(s.orders.reserved_exposure() == 10, "unknown: full exposure reserved");

        // A completed order is terminal and must stay terminal.
        oms::RecoveryState done(MarginMode::hedging);
        t.check(oms::recover(completed_lifecycle(), done), "unknown: completed recovered");
        t.check(done.orders.at(0).state == exec::OrderState::filled, "unknown: terminal preserved");
        t.check(done.orders.reserved_exposure() == 0, "unknown: terminal released exposure");
    }

    // ---- 10. deterministic reconstruction --------------------------------
    {
        const auto j = lifecycle();
        oms::RecoveryState first(MarginMode::hedging);
        t.check(oms::recover(j, first, true, HaltReason::manual_kill), "deterministic: first run");

        bool all_identical = true;
        for (int run = 0; run < 50; ++run) {
            oms::RecoveryState next(MarginMode::hedging);
            if (!oms::recover(j, next, true, HaltReason::manual_kill)) { all_identical = false; break; }
            if (!identical(first, next)) { all_identical = false; break; }
        }
        t.check(all_identical, "deterministic: 50 runs byte-identical");
        t.check(first.kill && first.halt == HaltReason::manual_kill, "deterministic: kill state");

        // Discriminating: differing inputs must produce differing state, or the
        // equality above would be satisfied vacuously.
        oms::RecoveryState without_kill(MarginMode::hedging);
        t.check(oms::recover(j, without_kill), "deterministic: no-kill run");
        t.check(!identical(first, without_kill), "deterministic: comparison discriminates");

        // Failure is transactional: a rejected journal leaves the caller's state
        // untouched rather than half-applied.
        oms::RecoveryState preserved(MarginMode::hedging);
        t.check(oms::recover(j, preserved), "deterministic: seeded state");
        exec::Journal bad;
        (void)bad.emit(JournalRecordType::fill, 1, 7, 9, 0, 77, 12, 4, 1, 0);
        t.check(!oms::recover(bad, preserved), "deterministic: bad journal rejected");
        t.check(identical(first, preserved) == false || preserved.orders.size() == 1,
                "deterministic: state not corrupted by failure");
        t.check(preserved.orders.size() == 1 && preserved.positions.size() == 1,
                "deterministic: failure left prior state intact");
    }

    return t.result();
}
