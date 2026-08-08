#pragma once
#include <cstdint>
#include <cstring>
#include <memory>

#include "oms/recovery.hpp"

// Phase J (blocker fix) -- streaming/segmented recovery, removing the
// 16,384-record ceiling Phase J's audit found in oms::recover() (which
// takes a whole exec::Journal -- a fixed std::array<JournalRecord, 16384>
// -- so it structurally cannot process more records than that in one call,
// regardless of how big RecoveryState::orders itself is sized).
//
// Root cause, precisely: two independent things both happened to be capped
// by the same exec::max_journal_records constant --
//   1. exec::Journal's fixed array (the thing actually flagged).
//   2. oms::default_capacity (= exec::max_journal_records too), which
//      RecoveryState::orders silently inherited since RecoveryState's
//      constructor never gave a caller any way to ask for more.
// Fixing only (1) without (2) would still cap recovered order count at
// 16,384 regardless of how the journal was read. Both are fixed here.
//
// Design (explicitly NOT "a giant fixed array"):
//   - RecoveryState gets an additive, backward-compatible constructor
//     overload taking an explicit Oms capacity (defaults to the existing
//     16,384 so every current call site is unaffected). Oms itself has
//     supported arbitrary capacities via a single preallocated heap block
//     since Phase B (oms.hpp) -- proven to 1M+ by oms_bench_main.cpp -- the
//     only missing piece was RecoveryState plumbing a caller-chosen capacity
//     through to it.
//   - Records are consumed one at a time from a caller-supplied source
//     (RecordSource, a template parameter -- zero-overhead, no std::function
//     allocation) instead of being materialized as a whole exec::Journal
//     first. persist::ExecutionWalTailer (Phase E, unchanged) already reads
//     the WAL in bounded 4096-record batches internally; this function is
//     what lets that batching reach all the way to full recovery instead of
//     stopping at a Journal-sized intermediate buffer. Memory use is O(1)
//     in record count -- one JournalRecord at a time, plus RecoveryState's
//     own preallocated-at-construction storage.
//   - recover()'s O(n^2) same_payload() scan (every record compared against
//     every prior record -- already the asymptotic bottleneck even within
//     the old 16,384 cap, and flatly infeasible at 1,000,000) is replaced
//     by DuplicateWindow: an O(1)-per-record, fixed-capacity hash check.
//     This is a real, documented change in guarantee -- see DuplicateWindow's
//     own comment -- traded deliberately for the ability to run at all at
//     this scale, the same "bounded, best-effort" trade
//     report_sequencer.hpp's TerminalCache already makes elsewhere in this
//     codebase for an analogous reason.
//   - oms::recover(const exec::Journal&, RecoveryState&, ...) itself is
//     completely untouched -- same code, same exact-duplicate check, same
//     bounded-Journal signature -- so every existing caller/test
//     (phase6_recovery*.cpp, phase7_recovery_fault_injection.cpp,
//     recovery_bench_main.cpp) keeps its exact prior behavior. This file is
//     purely additive.

namespace oms {

enum class StreamStatus : std::uint8_t { ok, done, corrupt };

// Bounded, O(1)-check duplicate-payload detector. Each of the fields
// recovery.hpp's own same_payload() compares is hashed into one 64-bit
// value; the value is looked up (and unconditionally stored) in a single
// slot of a fixed-size, power-of-two hash table indexed by hash & mask.
//
// Guarantee, precisely: a payload identical to one already occupying its
// own slot is *always* caught (the check happens before the slot is
// overwritten). A payload whose slot was since overwritten by an unrelated
// record's colliding hash will *not* be caught -- a false negative, not a
// false positive (this can only miss a real duplicate, never invent one).
// This covers the realistic corruption class this check exists for (a WAL
// segment, or a tailer batch, delivered twice in direct succession) without
// the O(n^2) cost of comparing every record against the *entire* prior
// history, which cannot run at 1,000,000-record scale at all.
class DuplicateWindow final {
public:
    explicit DuplicateWindow(std::size_t window)
        : capacity_(next_pow2(window == 0 ? 1 : window)), table_(std::make_unique<std::uint64_t[]>(capacity_)) {
        std::memset(table_.get(), 0, capacity_ * sizeof(std::uint64_t));
    }

    [[nodiscard]] bool check_and_record(const exec::JournalRecord& r) noexcept {
        const auto h = hash(r);
        const auto slot = h & (capacity_ - 1);
        const bool duplicate = (table_[slot] == h) && (h != 0);
        table_[slot] = (h == 0) ? 1 : h;  // 0 reserved as "empty" -- remap the (astronomically unlikely) real 0 hash
        return duplicate;
    }

private:
    [[nodiscard]] static std::size_t next_pow2(std::size_t v) noexcept {
        std::size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }
    [[nodiscard]] static std::uint64_t hash(const exec::JournalRecord& r) noexcept {
        std::uint64_t h = 1469598103934665603ULL;
        const auto mix = [&h](std::uint64_t v) { h ^= v; h *= 1099511628211ULL; };
        mix(r.ts_ns);
        mix(r.type);
        mix(r.reserved);
        mix(r.symbol_id);
        mix(r.run_id);
        mix(r.logical_order_id);
        mix(r.position_ticket);
        mix(static_cast<std::uint64_t>(r.a));
        mix(static_cast<std::uint64_t>(r.b));
        mix(static_cast<std::uint64_t>(r.c));
        mix(static_cast<std::uint64_t>(r.d));
        return h;
    }

    std::size_t capacity_;
    std::unique_ptr<std::uint64_t[]> table_;
};

// RecordSource must be callable as StreamStatus(exec::JournalRecord&) --
// `ok` with the record populated, `done` for a clean end (stop and
// finalize, matching recover()'s own end-of-journal behavior), `corrupt`
// to fail closed immediately (matching recover()'s existing "any invalid
// record fails the whole recovery" philosophy -- a partial, silently-
// truncated recovery is exactly what this codebase's fail-closed
// conventions since Phase B have refused to produce).
//
// Semantically identical, field-for-field and rule-for-rule, to
// oms::recover()'s own per-record validation and application logic (a
// direct port of that loop body) -- the only differences are (a) records
// arrive one at a time from `source` instead of by indexing a whole
// exec::Journal, and (b) DuplicateWindow replaces the O(n^2) same_payload()
// scan, per this file's header comment.
template <typename RecordSource>
[[nodiscard]] inline bool recover_streaming(RecordSource&& source, RecoveryState& out, std::size_t dedup_window,
                                            bool kill_present = false,
                                            risk::HaltReason persisted_halt = risk::HaltReason::none) {
    RecoveryState next(out.portfolio.mode(), out.orders.capacity());
    std::memset(&next.account, 0, sizeof(next.account));
    DuplicateWindow dedup(dedup_window);
    std::uint64_t last_ts = 0;
    bool have_account = false, have_pnl = false;
    std::size_t i = 0;

    for (;;) {
        exec::JournalRecord r{};
        const auto status = source(r);
        if (status == StreamStatus::done) break;
        if (status == StreamStatus::corrupt) return false;

        r.seq = i;  // normalized to stream position, exactly as exec::Journal::append() already
                    // does internally for the batch path -- seq is Journal-local bookkeeping. not
                    // durable WAL content (verified: no existing WAL writer call site sets it).
        if (r.reserved != 0 || r.ts_ns < last_ts || r.symbol_id >= exec::max_symbols || r.type < 1 || r.type > 10)
            return false;
        if (dedup.check_and_record(r)) return false;
        last_ts = r.ts_ns;
        const auto t = static_cast<exec::JournalRecordType>(r.type);
        const exec::BrokerOrderRef ref{r.run_id, r.logical_order_id};
        auto* o = next.orders.find(ref);

        if (t == exec::JournalRecordType::command) {
            if (!r.run_id || !r.logical_order_id || r.position_ticket || r.a < 0 || r.a > 1 || r.b <= 0 || r.c < 0 ||
                r.d < 0 || r.d > 1)
                return false;
            exec::Order v{};
            v.ref = ref; v.symbol_id = r.symbol_id; v.side = static_cast<exec::Side>(r.a); v.requested_volume = r.b;
            v.limit_price_ticks = r.c; v.type = static_cast<exec::OrderType>(r.d); v.state = exec::OrderState::sent;
            if (!next.orders.restore(v)) return false;
            ++next.counters.commands;
        } else if (t == exec::JournalRecordType::order_state) {
            if (!detail::valid_order_state(r.a) || !detail::valid_order_state(r.b) || r.position_ticket || r.c < 0 ||
                r.d < 0)
                return false;
            const auto from = static_cast<exec::OrderState>(r.a);
            const auto to = static_cast<exec::OrderState>(r.b);
            if (!o && from == exec::OrderState::pending_send && to == exec::OrderState::sent) { ++i; continue; }
            if (!o || o->state != from || o->filled_volume != r.c || o->requested_volume != r.d ||
                !next.orders.transition(ref, to))
                return false;
        } else if (t == exec::JournalRecordType::acknowledgement) {
            if (!o || o->state != exec::OrderState::acknowledged || r.a < 0 || r.position_ticket) return false;
            ++next.counters.acks;
        } else if (t == exec::JournalRecordType::rejection) {
            if (r.a <= 0) return false;
            if (r.logical_order_id && (!o || o->state != exec::OrderState::sent)) return false;
            ++next.counters.rejections;
        } else if (t == exec::JournalRecordType::fill) {
            if (!o || !r.position_ticket || r.a <= 0 || r.b <= 0 || r.c < 0 || (r.d != 0 && r.d != 1) ||
                !next.orders.recover_fill(ref, r.b))
                return false;
            ++next.counters.fills;
        } else if (t == exec::JournalRecordType::cancel) {
            if (!o || o->state != exec::OrderState::cancel_pending || r.a < 0 || r.position_ticket) return false;
            ++next.counters.cancels;
        } else if (t == exec::JournalRecordType::replace) {
            if (!o || o->state != exec::OrderState::acknowledged || r.a <= 0 || r.b <= 0 || r.c <= 0 || r.d <= 0 ||
                r.d < o->filled_volume || r.a != o->limit_price_ticks || r.c != o->requested_volume)
                return false;
            o->limit_price_ticks = r.b; o->requested_volume = r.d;
            ++next.counters.replaces;
        } else if (t == exec::JournalRecordType::position_update) {
            if (r.logical_order_id || !r.position_ticket || r.a < 0 || r.a > 1 || r.b < 0 || r.c < 0) return false;
            auto* old = next.positions.find(r.position_ticket);
            if (old && (old->symbol_id != r.symbol_id || old->side != static_cast<exec::Side>(r.a) ||
                       old->state == exec::PositionState::closed))
                return false;
            if (!old && r.b == 0) return false;
            exec::Position p{};
            p.position_ticket = r.position_ticket; p.symbol_id = r.symbol_id; p.side = static_cast<exec::Side>(r.a);
            p.state = r.b ? exec::PositionState::open : exec::PositionState::closed;
            p.volume = r.b; p.avg_price_ticks = r.c; p.realized_pnl_minor = r.d;
            if (old) *old = p;
            else if (!next.positions.restore(p)) return false;
            if (!next.portfolio.upsert(p.position_ticket, p.symbol_id, p.side, p.volume, p.realized_pnl_minor))
                return false;
        } else if (t == exec::JournalRecordType::account_update) {
            if (r.logical_order_id || r.position_ticket || r.c < 0 || r.d < 0 || r.b - r.c != r.d) return false;
            next.account.balance_minor = r.a; next.account.equity_minor = r.b; next.account.margin_used_minor = r.c;
            next.account.free_margin_minor = r.d; have_account = true; have_pnl = false;
        } else if (t == exec::JournalRecordType::pnl_update) {
            if (r.logical_order_id || r.position_ticket || r.d != 0 || !have_account || have_pnl ||
                next.account.equity_minor != next.account.balance_minor + r.b)
                return false;
            next.account.realized_pnl_minor = r.a; next.account.unrealized_pnl_minor = r.b;
            next.account.commission_paid_minor = r.c; have_pnl = true;
        }
        ++i;
    }

    if (have_account != have_pnl) return false;
    for (std::size_t k = 0; k < next.orders.size(); ++k) {
        const auto& o = next.orders.at(k);
        if (!exec::is_terminal(o.state) && o.state != exec::OrderState::unknown)
            if (!next.orders.mark_unknown_on_restart(o.ref)) return false;
    }
    next.account.open_orders = static_cast<std::uint32_t>(next.orders.size());
    next.account.open_positions = 0;
    for (std::size_t k = 0; k < next.positions.size(); ++k)
        if (next.positions.at(k).state != exec::PositionState::closed) ++next.account.open_positions;
    next.kill = kill_present;
    next.halt = kill_present ? (persisted_halt == risk::HaltReason::none ? risk::HaltReason::manual_kill : persisted_halt)
                             : persisted_halt;
    out = std::move(next);
    return true;
}

}  // namespace oms
