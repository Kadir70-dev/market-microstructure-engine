#pragma once
#include <cstdint>
#include <memory>

#include "oms/oms.hpp"

// Phase D — execution-report sequencing, deduplication, deterministic
// reconciliation of report arrival order.
//
// Problem: Phase B/C's Oms::fill()/transition() are correct but NOT
// idempotent at the *report* level -- calling fill(ref, 5) twice really does
// apply 10, because Oms has no notion of "I already processed this specific
// report." A duplicate execution report (retransmit, reconnect replay) would
// silently double-count filled quantity/PnL/exposure. This file is the
// minimum correct fix: a per-order monotonic sequence number
// (exec::Order::last_applied_seq, Phase D's one addition to that struct) plus
// a bounded reorder buffer for reports that arrive ahead of a gap, and a
// bounded "recently terminal" cache so a late report for an order that
// already finished can be told apart from a report for an order that never
// existed at all.
//
// Design:
// - Every report carries a venue_seq: 1-based, monotonic, assigned by the
//   (simulated) venue per order. There is no real venue in this system (Part
//   9, PaperBroker only) -- tests and the benchmark construct report streams
//   directly.
// - venue_seq <= order->last_applied_seq: exactly-once guard. Rejected as
//   `duplicate`, whether it's a literal retransmit or a venue replaying its
//   entire report log after a reconnect -- both look identical here, which
//   is correct: both are "I have already applied this."
// - venue_seq == last_applied_seq + 1: in sequence. Applied immediately,
//   Oms's existing legality/volume checks are the actual gate (a fill that
//   would overfill, a transition the state machine forbids, are `illegal`).
// - venue_seq > last_applied_seq + 1: a gap. Held in a bounded, preallocated
//   pool (linear-scan, not a hot path -- gaps are the tail case) keyed by
//   (ref, venue_seq). Whenever a report is applied in sequence, the pool is
//   drained for that ref's newly-expected next seq, cascading through
//   however many held reports now fit. Pool exhaustion is the fail-closed
//   signal (`gap_pool_exhausted`): this system does not silently drop a
//   report or apply out of order to make room.
// - Order not found in Oms (already reclaimed as terminal, or never
//   existed): the terminal cache (a bounded, overwrite-oldest ring of
//   recently-reclaimed refs, populated by this sequencer at the moment of
//   reclaim) distinguishes `terminal_late` (a known order, already done --
//   correctly and silently ignored) from `unknown_order` (a ref this OMS
//   never created -- a genuine orphan, worth flagging in a way "just another
//   late report" is not). The cache is bounded and best-effort: an order
//   that went terminal long enough ago to be evicted is reported as
//   `unknown_order` too, which is the conservative, honestly-labelled
//   failure mode of a bounded history rather than a claim of perfect
//   recall.
//
// This determinism claim is exactly "independent of arrival order where
// venue semantics allow": replaying the same report set in any arrival
// order converges to the same final Oms state, because application order is
// governed by venue_seq (a total order the venue already defined), not by
// wall-clock arrival order. Where venue semantics do *not* allow a single
// answer (e.g. two reports racing at the exact same venue_seq, which cannot
// happen for a well-formed single-venue stream) is out of scope by
// construction: venue_seq is required to be the unique total order.
//
// No unbounded allocation on the hot path: GapPool and TerminalCache
// preallocate once at construction (single make_unique each); process()
// never allocates.

namespace oms {

// cancel_pending and cancelled are two distinct reports, not one: Oms's
// state machine (order_state.hpp) requires acknowledged/partially_filled ->
// cancel_pending -> cancelled, matching how a real venue actually confirms a
// cancel in two steps ("cancel accepted, working" then "canceled").
enum class ReportKind : std::uint8_t { ack, fill, cancel_pending, cancelled, reject, replace_ack };

struct ExecReport final {
    exec::BrokerOrderRef ref{};
    std::uint64_t venue_seq{0};   // 1-based, monotonic per order
    ReportKind kind{ReportKind::ack};
    std::int64_t volume{0};       // fill: fill qty. replace_ack: new requested volume.
    std::int64_t price{0};        // replace_ack: new limit price.
};

enum class ReportOutcome : std::uint8_t {
    applied,             // applied, in sequence
    duplicate,           // venue_seq already applied (retransmit or reconnect replay)
    held_for_gap,        // venue_seq ahead of a gap; buffered
    unknown_order,       // ref never seen, or evicted from the bounded terminal cache
    terminal_late,       // ref known and already terminal; correctly ignored
    illegal,             // in sequence, but Oms rejected it (state machine / volume)
    gap_pool_exhausted,  // fail-closed: no room to buffer this gap
};

namespace detail {

// Bounded, preallocated pool of reports held while waiting for a gap to
// fill. Linear scan inside hold()/take()/discard_all_for(): gaps are the
// tail case, not the hot path, and a hash index here would cost more than
// it saves at this capacity.
//
// held_count_ is a plain counter, not a scan, and that distinction matters:
// ReportSequencer::drain() runs after *every* successfully applied report,
// including the overwhelming common case where no gap exists at all. An
// O(capacity) held_count() (an earlier version of this file scanned all
// slots) turns that into an O(capacity) tax on every single report, gap or
// not -- measured directly: it took a 1M-report benchmark run from
// low-microseconds-per-report to never finishing. The counter makes the
// no-gap case what it should always have been: O(1).
class GapPool final {
public:
    explicit GapPool(std::size_t capacity)
        : capacity_(capacity), slots_(std::make_unique<Slot[]>(capacity)) {}

    [[nodiscard]] bool hold(const ExecReport& report) noexcept {
        for (std::size_t i = 0; i < capacity_; ++i) {
            if (!slots_[i].occupied) { slots_[i] = {true, report}; ++held_count_; return true; }
        }
        return false;
    }

    [[nodiscard]] bool take(exec::BrokerOrderRef ref, std::uint64_t seq, ExecReport& out) noexcept {
        for (std::size_t i = 0; i < capacity_; ++i) {
            if (slots_[i].occupied && slots_[i].report.ref == ref && slots_[i].report.venue_seq == seq) {
                out = slots_[i].report;
                slots_[i].occupied = false;
                --held_count_;
                return true;
            }
        }
        return false;
    }

    void discard_all_for(exec::BrokerOrderRef ref) noexcept {
        for (std::size_t i = 0; i < capacity_; ++i)
            if (slots_[i].occupied && slots_[i].report.ref == ref) { slots_[i].occupied = false; --held_count_; }
    }

    [[nodiscard]] std::size_t held_count() const noexcept { return held_count_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    struct Slot final { bool occupied{false}; ExecReport report{}; };
    std::size_t capacity_;
    std::unique_ptr<Slot[]> slots_;
    std::size_t held_count_{0};
};

// Bounded, overwrite-oldest ring of recently-terminal order refs.
class TerminalCache final {
public:
    explicit TerminalCache(std::size_t capacity)
        : capacity_(capacity == 0 ? 1 : capacity), ring_(std::make_unique<exec::BrokerOrderRef[]>(capacity_)) {}

    void record(exec::BrokerOrderRef ref) noexcept {
        ring_[write_ % capacity_] = ref;
        ++write_;
        if (count_ < capacity_) ++count_;
    }

    [[nodiscard]] bool contains(exec::BrokerOrderRef ref) const noexcept {
        for (std::size_t i = 0; i < count_; ++i)
            if (ring_[i] == ref) return true;
        return false;
    }

private:
    std::size_t capacity_;
    std::size_t write_{0};
    std::size_t count_{0};
    std::unique_ptr<exec::BrokerOrderRef[]> ring_;
};

}  // namespace detail

class ReportSequencer final {
public:
    explicit ReportSequencer(std::size_t gap_pool_capacity = 1024, std::size_t terminal_cache_capacity = 4096)
        : gap_pool_(gap_pool_capacity), terminal_cache_(terminal_cache_capacity) {}

    // `o` is not owned: the caller (single-threaded test, or a ShardedOms
    // shard's own worker thread) supplies the Oms it already owns. This
    // keeps the sequencer usable standalone (fast, no threading, for
    // permutation/determinism tests) and embeddable per-shard (co-located
    // with that shard's Oms, mutated only by the shard's single owner
    // thread -- no new synchronization required).
    [[nodiscard]] ReportOutcome process(Oms& o, const ExecReport& report) noexcept {
        auto* order = o.find(report.ref);
        if (order == nullptr)
            return terminal_cache_.contains(report.ref) ? ReportOutcome::terminal_late
                                                          : ReportOutcome::unknown_order;

        if (report.venue_seq <= order->last_applied_seq) return ReportOutcome::duplicate;

        if (report.venue_seq > order->last_applied_seq + 1)
            return gap_pool_.hold(report) ? ReportOutcome::held_for_gap : ReportOutcome::gap_pool_exhausted;

        const auto outcome = apply_one(o, report);
        if (outcome == ReportOutcome::applied) drain(o, report.ref);
        return outcome;
    }

    [[nodiscard]] std::size_t gap_pool_held() const noexcept { return gap_pool_.held_count(); }
    [[nodiscard]] std::size_t gap_pool_capacity() const noexcept { return gap_pool_.capacity(); }

private:
    [[nodiscard]] ReportOutcome apply_one(Oms& o, const ExecReport& report) noexcept {
        bool ok = false;
        switch (report.kind) {
            case ReportKind::ack:
                ok = o.transition(report.ref, exec::OrderState::acknowledged);
                break;
            case ReportKind::fill:
                ok = o.fill(report.ref, report.volume);
                break;
            case ReportKind::cancel_pending:
                ok = o.transition(report.ref, exec::OrderState::cancel_pending);
                break;
            case ReportKind::cancelled:
                ok = o.transition(report.ref, exec::OrderState::cancelled);
                break;
            case ReportKind::reject:
                ok = o.transition(report.ref, exec::OrderState::rejected);
                break;
            case ReportKind::replace_ack: {
                auto* order = o.find(report.ref);
                if (order != nullptr && !exec::is_terminal(order->state) &&
                    order->state != exec::OrderState::unknown && report.volume > order->filled_volume) {
                    order->limit_price_ticks = report.price;
                    order->requested_volume = report.volume;
                    ok = true;
                }
                break;
            }
        }
        if (!ok) return ReportOutcome::illegal;

        // Re-find rather than reuse any pointer obtained above: a terminal
        // transition reclaims the slot (Phase B swap-removal), which
        // invalidates it. last_applied_seq only matters on a still-live
        // order; a reclaimed one is recorded into the terminal cache instead,
        // which is what makes every future report for it `terminal_late`.
        auto* after = o.find(report.ref);
        if (after == nullptr) terminal_cache_.record(report.ref);
        else after->last_applied_seq = report.venue_seq;
        return ReportOutcome::applied;
    }

    void drain(Oms& o, exec::BrokerOrderRef ref) noexcept {
        // O(1) fast path: called after every successful apply, including
        // the overwhelmingly common case where the pool is empty.
        if (gap_pool_.held_count() == 0) return;
        for (;;) {
            const auto* order = o.find(ref);
            if (order == nullptr) { gap_pool_.discard_all_for(ref); return; }
            ExecReport next{};
            if (!gap_pool_.take(ref, order->last_applied_seq + 1, next)) return;
            if (apply_one(o, next) != ReportOutcome::applied) return;
        }
    }

    detail::GapPool gap_pool_;
    detail::TerminalCache terminal_cache_;
};

}  // namespace oms
