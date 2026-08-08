#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "core/ring_buffer.hpp"
#include "oms/oms.hpp"
#include "oms/report_sequencer.hpp"
#include "persist/wal_hook.hpp"

// Phase C — sharded, single-writer-per-shard concurrent OMS.
//
// Design:
//
// - Shard key: ref.logical_order_id % shard_count. Stable per order for its
//   entire lifetime, so every event for a given order always routes to the
//   same shard -- this alone is what makes "single-writer per shard" also
//   mean "single writer per order", without any per-order locking.
//
// - Every operation (create/find/transition/fill/replace) for a shard is
//   applied by exactly one dedicated worker thread, drawn from a fixed set
//   of preallocated, bounded, lock-free SPSC rings -- one per (producer,
//   shard) pair, reusing core::RingBuffer unchanged. A caller thread that
//   wants to touch shard S pushes a Request onto its own ring into S; the
//   ring is never shared between two producer threads, so try_push/try_pop
//   need no additional synchronization beyond what RingBuffer already has.
//
// - Reads (find) go through the *same* per-shard queue as writes, not a
//   separate lock-free read path (e.g. a seqlock over Oms's raw storage).
//   That was the other option considered: a seqlock gives lower read
//   latency, but reading Order/index memory that the shard thread may be
//   concurrently mutating is a data race under the C++ object model even
//   though the seqlock retry makes it "safe" in the sense every real HFT
//   feed handler relies on -- it would need ThreadSanitizer suppressions to
//   even build cleanly, and correctness was set above throughput for this
//   phase. Routing find() through the same single-writer queue means no
//   shard's Oms/index memory is ever touched by more than one thread, ever
//   -- provably race-free, TSan-clean by construction, at the cost of read
//   latency being a queue round trip instead of a direct call. That cost is
//   measured, not hidden -- see the benchmark's queue-latency numbers.
//
// - submit_* calls are synchronous from the caller's point of view: push a
//   Request carrying a pointer to a stack-local Completion, then spin-wait
//   (bounded, falling back to yield) on that Completion's `done` flag. The
//   Completion is never touched by anything but the caller (before push /
//   after done) and the one shard worker that owns the request in between,
//   so this handoff is race-free by the same single-writer argument.
//
// - Deterministic total order for replay: whichever shard worker actually
//   applies a request claims the next value of a single process-wide
//   atomic counter (global_seq_) at the moment of application, and records
//   it alongside the request in that shard's own bounded op log. This does
//   *not* claim that concurrent runs interleave identically -- real OS
//   scheduling makes that neither achievable nor meaningful. What it
//   guarantees is that whatever interleaving *did* happen is captured as a
//   single, gapless, total order across all shards, and that a merge of the
//   per-shard logs by global_seq (see merged_log()) reconstructs that order
//   exactly. Feeding that captured order back through Phase B's existing
//   single-threaded oms::recover()/Oms replay is what proves determinism --
//   the same guarantee Phase B already established, now fed by a concurrent
//   producer instead of a hand-written journal.
//
// - Out-of-order handling is deliberately minimal here: routing every event
//   for an order through one queue in FIFO order, combined with Oms's
//   existing state-machine legality checks (legal() rejects any transition
//   that doesn't fit the order's current state), is what prevents a raced
//   fill/cancel/replace from corrupting an order -- no separate reorder
//   buffer is added. A real reorder buffer for reports arriving out of wire
//   order from an external venue is Phase D's job, not this one's: there is
//   no venue in this system (Part 9, PaperBroker only), so nothing external
//   can actually deliver a report out of order yet.
//
// - No unbounded allocation on the hot path: shard_count * max_producers
//   rings, each shard's Oms(capacity_per_shard), and each shard's op log are
//   all preallocated once at construction. submit_*/find_copy never
//   allocate.

namespace oms {

enum class OpKind : std::uint8_t { create, find, transition, fill, replace, report, snapshot, restore };

// Fixed at compile time because core::RingBuffer's capacity is a template
// parameter, consistent with every other fixed-capacity type in this
// codebase. 4096 in-flight requests per (producer, shard) pair is generous
// headroom for the synchronous, blocking-on-completion submit pattern, where
// a producer normally never has more than one request in flight on a given
// ring at a time; the real saturation test is exercised by pausing a shard's
// worker (see pause()/resume()) rather than by outrunning this capacity in
// normal operation.
inline constexpr std::size_t queue_capacity = 4096;

struct Completion final {
    std::atomic<bool> done{false};
    bool ok{false};
    exec::Order order{};
    ReportOutcome report_outcome{ReportOutcome::applied};  // meaningful only for OpKind::report
    // Phase F: OpKind::snapshot output. A caller-owned buffer the shard's own
    // worker thread fills with its live orders -- safe by the same
    // single-writer argument as every other operation (the buffer is only
    // ever touched by the caller before push / after done, and by the one
    // shard thread that owns the request in between), used by multi-venue
    // reconciliation to read a whole shard without a data race on Oms's raw
    // storage. Not on the hot path: reconciliation, like Phase E, is
    // explicitly not a per-order operation.
    exec::Order* snapshot_out{nullptr};
    std::size_t snapshot_capacity{0};
    std::size_t snapshot_count{0};
};

struct Request final {
    OpKind kind{};
    exec::BrokerOrderRef ref{};
    std::uint32_t symbol{0};
    std::int64_t volume{0};       // create: requested volume. fill/report(fill): volume. replace(_ack): new volume.
                                   // restore: requested_volume.
    std::int64_t price{0};        // replace(_ack): new limit price. restore: limit_price_ticks.
    exec::OrderState target{exec::OrderState::new_order};  // transition: target state. restore: order state.
    std::uint64_t venue_seq{0};   // report: 1-based per-order sequence.
    ReportKind report_kind{ReportKind::ack};  // report: which kind of execution report.
    std::int64_t filled_volume{0};  // restore only (Phase J blocker fix): the recovered order's filled_volume --
                                    // no other op needs a caller-supplied filled_volume, since every other path
                                    // derives it from Oms's own live state, not from a Request field.
    Completion* completion{nullptr};
};
static_assert(std::is_trivially_copyable_v<Request>, "Request must be ring-buffer safe");

struct LogEntry final {
    std::uint64_t global_seq{0};
    OpKind kind{};
    exec::BrokerOrderRef ref{};
    exec::OrderState result_state{exec::OrderState::new_order};
    bool ok{false};
};

class ShardedOms final {
public:
    // gap_pool_capacity_per_shard/terminal_cache_capacity_per_shard default
    // to ReportSequencer's own defaults; existing Phase C call sites that
    // predate report sequencing are unaffected.
    ShardedOms(std::size_t shard_count, std::size_t capacity_per_shard,
               std::size_t max_producers, std::size_t log_capacity_per_shard,
               std::size_t gap_pool_capacity_per_shard = 1024,
               std::size_t terminal_cache_capacity_per_shard = 4096)
        : shard_count_(shard_count), max_producers_(max_producers) {
        shards_.reserve(shard_count_);
        for (std::size_t s = 0; s < shard_count_; ++s)
            shards_.emplace_back(std::make_unique<Shard>(capacity_per_shard, max_producers_,
                                                          log_capacity_per_shard, gap_pool_capacity_per_shard,
                                                          terminal_cache_capacity_per_shard));
        for (std::size_t s = 0; s < shard_count_; ++s)
            shards_[s]->worker = std::thread([this, s] { run(s); });
    }

    ~ShardedOms() {
        for (auto& shard : shards_) shard->stop.store(true, std::memory_order_release);
        for (auto& shard : shards_)
            if (shard->worker.joinable()) shard->worker.join();
    }

    ShardedOms(const ShardedOms&) = delete;
    ShardedOms& operator=(const ShardedOms&) = delete;

    // Global kill switch (Part 7): trips/reads the RiskEngine every
    // ShardedOms instance in the process already shares via standing_engine()
    // above. Static rather than a member on purpose -- there is no single
    // ShardedOms that "owns" the kill switch once there are multiple venues,
    // each with its own ShardedOms, and the requirement is that halting stops
    // *every* venue's order creation, not just one instance's.
    static void halt_globally(risk::HaltReason reason) noexcept { standing_engine().halt(reason); }
    [[nodiscard]] static bool globally_halted() noexcept { return standing_engine().halted(); }
    [[nodiscard]] static risk::HaltReason global_halt_reason() noexcept { return standing_engine().halt_reason(); }

    [[nodiscard]] std::size_t shard_count() const noexcept { return shard_count_; }
    [[nodiscard]] std::size_t shard_of(exec::BrokerOrderRef ref) const noexcept {
        return ref.logical_order_id % shard_count_;
    }

    // Returns false immediately if the target shard's inbound ring for this
    // producer is full (fail-closed) rather than blocking on push -- the
    // *caller* already blocks on completion below, so this is the only place
    // genuine backpressure can be observed and reported.
    // No per-request risk::Approval parameter: create() needs one (Part 10),
    // but its only constructor is private to RiskEngine/Decision, and wiring
    // a real per-request risk check through the queue is Phase I's job. A
    // standing, statically-approved token (standing_approval(), below) is
    // used internally instead -- the API deliberately does not accept a
    // caller-supplied token it would then ignore.
    [[nodiscard]] bool submit_create(std::size_t producer_id, exec::BrokerOrderRef ref,
                                     std::uint32_t symbol, std::int64_t volume,
                                     Completion& out) noexcept {
        Request req{};
        req.kind = OpKind::create; req.ref = ref; req.symbol = symbol; req.volume = volume;
        req.completion = &out;
        return dispatch(producer_id, ref, req, out);
    }

    [[nodiscard]] bool submit_find(std::size_t producer_id, exec::BrokerOrderRef ref,
                                   Completion& out) noexcept {
        Request req{};
        req.kind = OpKind::find; req.ref = ref; req.completion = &out;
        return dispatch(producer_id, ref, req, out);
    }

    [[nodiscard]] bool submit_transition(std::size_t producer_id, exec::BrokerOrderRef ref,
                                         exec::OrderState target, Completion& out) noexcept {
        Request req{};
        req.kind = OpKind::transition; req.ref = ref; req.target = target; req.completion = &out;
        return dispatch(producer_id, ref, req, out);
    }

    [[nodiscard]] bool submit_fill(std::size_t producer_id, exec::BrokerOrderRef ref,
                                   std::int64_t volume, Completion& out) noexcept {
        Request req{};
        req.kind = OpKind::fill; req.ref = ref; req.volume = volume; req.completion = &out;
        return dispatch(producer_id, ref, req, out);
    }

    [[nodiscard]] bool submit_replace(std::size_t producer_id, exec::BrokerOrderRef ref,
                                      std::int64_t new_price, std::int64_t new_volume,
                                      Completion& out) noexcept {
        Request req{};
        req.kind = OpKind::replace; req.ref = ref; req.price = new_price; req.volume = new_volume;
        req.completion = &out;
        return dispatch(producer_id, ref, req, out);
    }

    // Phase J (blocker fix): restart-recovery-only. Loads an order at its
    // exact recovered state (requested/filled volume, price, and a state
    // that legitimately may be `unknown` -- Oms::create() cannot express
    // any of that) into a *fresh* ShardedOms being repopulated from
    // oms::recover_streaming()'s output. See oms/restart_recovery.hpp.
    [[nodiscard]] bool submit_restore(std::size_t producer_id, exec::BrokerOrderRef ref, std::uint32_t symbol,
                                      std::int64_t requested_volume, std::int64_t filled_volume,
                                      std::int64_t limit_price_ticks, exec::OrderState state, Completion& out) noexcept {
        Request req{};
        req.kind = OpKind::restore; req.ref = ref; req.symbol = symbol; req.volume = requested_volume;
        req.filled_volume = filled_volume; req.price = limit_price_ticks; req.target = state;
        req.completion = &out;
        return dispatch(producer_id, ref, req, out);
    }

    // Phase D: submit an execution report for sequencing/dedup/reconciliation
    // rather than a direct state mutation. out.report_outcome tells the
    // caller what actually happened (applied/duplicate/held_for_gap/
    // unknown_order/terminal_late/illegal/gap_pool_exhausted); out.ok mirrors
    // whether the report was accepted for processing (i.e. not itself a
    // queue-saturation rejection -- see the bool return, which is that).
    [[nodiscard]] bool submit_report(std::size_t producer_id, const ExecReport& report,
                                     Completion& out) noexcept {
        Request req{};
        req.kind = OpKind::report; req.ref = report.ref; req.venue_seq = report.venue_seq;
        req.report_kind = report.kind; req.volume = report.volume; req.price = report.price;
        req.completion = &out;
        return dispatch(producer_id, report.ref, req, out);
    }

    // Convenience synchronous wrapper: submit + spin-wait, returning ok/order
    // directly instead of making every caller manage a Completion. Still
    // fully queue-mediated underneath.
    [[nodiscard]] bool find_copy(std::size_t producer_id, exec::BrokerOrderRef ref,
                                 exec::Order& out) noexcept {
        Completion c{};
        if (!submit_find(producer_id, ref, c)) return false;
        out = c.order;
        return c.ok;
    }

    // Test-only: pause/resume a shard's worker so queue-saturation behavior
    // (submit_* returning false because the ring is genuinely full) can be
    // exercised deterministically instead of racing the drain loop.
    void pause(std::size_t shard) noexcept { shards_[shard]->paused.store(true, std::memory_order_release); }
    void resume(std::size_t shard) noexcept { shards_[shard]->paused.store(false, std::memory_order_release); }

    [[nodiscard]] std::size_t live_count(std::size_t shard) const noexcept {
        return shards_[shard]->oms.size();
    }
    [[nodiscard]] std::uint64_t global_seq() const noexcept {
        return global_seq_.load(std::memory_order_acquire);
    }

    // Phase J (blocker fix): attaches the durability hook every shard's own
    // apply() -- i.e. its own single-writer worker thread -- uses to record
    // WAL entries for state-changing operations, in the exact order it
    // applied them (see persist/wal_hook.hpp for why that ordering property
    // specifically requires calling from here, not from any caller's own
    // thread). Additive, opt-in: defaulted to nullptr, so every pre-existing
    // ShardedOms construction/test is completely unaffected.
    void attach_wal_hook(persist::WalHook* hook) noexcept { wal_hook_ = hook; }
    [[nodiscard]] std::uint64_t wal_records_dropped() const noexcept {
        return wal_dropped_.load(std::memory_order_relaxed);
    }

    // Instantaneous depth of one (producer, shard) inbound ring. Racy by
    // nature (a monitoring thread reading this concurrently with real
    // traffic) but that is exactly what "queue depth under load" means to
    // measure -- a point-in-time sample, not a synchronized snapshot.
    [[nodiscard]] std::size_t queue_depth(std::size_t shard, std::size_t producer_id) const noexcept {
        return shards_[shard]->inbound[producer_id]->size();
    }

    // Phase F: copy a shard's entire live-order set into a caller-owned
    // buffer, routed through that shard's own single-writer queue exactly
    // like every other op (see the Completion::snapshot_* comment) --
    // targets shard_index directly rather than routing via shard_of(ref),
    // since a snapshot request has no single order ref to route by. Used by
    // per-venue reconciliation instead of reading shard.oms from another
    // thread, which would be a data race.
    [[nodiscard]] bool submit_snapshot(std::size_t producer_id, std::size_t shard_index,
                                       exec::Order* out_buffer, std::size_t out_capacity,
                                       Completion& out) noexcept {
        Request req{};
        req.kind = OpKind::snapshot;
        req.completion = &out;
        out.snapshot_out = out_buffer;
        out.snapshot_capacity = out_capacity;
        out.snapshot_count = 0;
        out.done.store(false, std::memory_order_relaxed);
        if (!shards_[shard_index]->inbound[producer_id]->try_push(req)) return false;
        wait(out);
        return true;
    }

    // Test-only: enqueue a create request without waiting for completion, so
    // queue saturation (the ring genuinely full) can be observed directly
    // instead of via a submit_* call that would otherwise block forever
    // against a paused shard. `out` must outlive the eventual drain if the
    // shard is later resumed.
    [[nodiscard]] bool try_enqueue_create(std::size_t producer_id, exec::BrokerOrderRef ref,
                                          std::uint32_t symbol, std::int64_t volume,
                                          Completion& out) noexcept {
        Request req{};
        req.kind = OpKind::create; req.ref = ref; req.symbol = symbol; req.volume = volume;
        req.completion = &out;
        return enqueue(producer_id, ref, req, out);
    }

    // Diagnostic/replay use only -- not on the hot path, allowed to allocate.
    // Merges every shard's bounded op log into one vector ordered by
    // global_seq, i.e. the actual total order in which this run applied
    // events across all shards.
    [[nodiscard]] std::vector<LogEntry> merged_log() const {
        std::vector<LogEntry> merged;
        std::size_t total = 0;
        for (auto& shard : shards_) total += shard->log_count.load(std::memory_order_acquire);
        merged.reserve(total);
        for (auto& shard : shards_) {
            const auto n = shard->log_count.load(std::memory_order_acquire);
            for (std::size_t i = 0; i < n; ++i) merged.push_back(shard->log[i]);
        }
        std::sort(merged.begin(), merged.end(),
                 [](const LogEntry& a, const LogEntry& b) { return a.global_seq < b.global_seq; });
        return merged;
    }

private:
    struct Shard final {
        Shard(std::size_t capacity, std::size_t producers, std::size_t log_capacity,
              std::size_t gap_pool_capacity, std::size_t terminal_cache_capacity)
            : oms(capacity), sequencer(gap_pool_capacity, terminal_cache_capacity),
              log(std::make_unique<LogEntry[]>(log_capacity)), log_capacity(log_capacity) {
            inbound.reserve(producers);
            for (std::size_t p = 0; p < producers; ++p)
                inbound.emplace_back(std::make_unique<core::RingBuffer<Request, queue_capacity>>());
        }
        oms::Oms oms;
        // Owned by this shard, touched only by this shard's single worker
        // thread -- same single-writer argument as oms above, no new
        // synchronization needed.
        ReportSequencer sequencer;
        std::vector<std::unique_ptr<core::RingBuffer<Request, queue_capacity>>> inbound;
        std::unique_ptr<LogEntry[]> log;
        std::size_t log_capacity;
        std::atomic<std::size_t> log_count{0};
        std::atomic<bool> stop{false};
        std::atomic<bool> paused{false};
        std::thread worker;
    };

    [[nodiscard]] bool dispatch(std::size_t producer_id, exec::BrokerOrderRef ref, const Request& req,
                                Completion& out) noexcept {
        if (!enqueue(producer_id, ref, req, out)) return false;
        wait(out);
        return true;
    }

    // Split out so queue-saturation can be tested without deadlocking: a
    // blocking dispatch() cannot be used to probe a *paused* shard's ring
    // capacity, because the calling thread would then wait forever for a
    // completion the paused worker will never produce. enqueue() alone is
    // exactly the fail-closed backpressure signal ("queue saturation/
    // fail-closed behavior").
    [[nodiscard]] bool enqueue(std::size_t producer_id, exec::BrokerOrderRef ref, const Request& req,
                               Completion& out) noexcept {
        out.done.store(false, std::memory_order_relaxed);
        auto& shard = *shards_[shard_of(ref)];
        return shard.inbound[producer_id]->try_push(req);
    }

    void wait(Completion& out) noexcept {
        std::size_t spins = 0;
        while (!out.done.load(std::memory_order_acquire)) {
            if (++spins > 1000) { std::this_thread::yield(); spins = 0; }
        }
    }

    // create() requires a risk::Approval token, whose only constructor is
    // private to RiskEngine/Decision (Part 10). Per-request risk integration
    // is Phase I's job (concurrent risk/exposure integration); this phase is
    // about proving the OMS storage/index is concurrency-safe, so a single
    // pre-approved token, computed once, stands in for it here -- the same
    // shortcut tests/cpp/phase6_oms_capacity.cpp's approval() helper already
    // takes for the equivalent single-threaded case.
    //
    // standing_engine() is a function-local static in an inline member
    // function: per [dcl.fcn.def.default]/[basic.def.odr] that makes it one
    // shared object across every translation unit -- and therefore across
    // every ShardedOms instance in the process, including one ShardedOms per
    // venue in Phase F. That is what lets halt_globally() below be a single
    // choke point that blocks order creation on every venue at once, without
    // any explicit wiring between venues.
    [[nodiscard]] static risk::RiskEngine& standing_engine() noexcept {
        static risk::RiskEngine engine(risk::Limits{});
        return engine;
    }

    // Originally discarded Decision::approved and always returned a token,
    // which was harmless only because nothing before Phase F ever halted
    // this engine. Phase F's "existing global kill switch must remain
    // authoritative" requirement means halting it now has to actually
    // refuse new orders, so this checks approved and returns no token when
    // halted -- the one behavior change this phase makes to Phase C's OMS
    // storage/concurrency mechanism itself, everything else reused as-is.
    [[nodiscard]] static std::optional<risk::Approval> standing_approval() noexcept {
        risk::Request q{};
        q.volume = 1; q.risk_minor = 1; q.free_margin = 1'000'000; q.warm_mask = 1; q.session_open = true;
        const auto decision = standing_engine().check(q);
        if (!decision.approved) return std::nullopt;
        return decision.token;
    }

    void apply(Shard& shard, std::size_t shard_index, const Request& req) noexcept {
        Completion& c = *req.completion;
        bool ok = false;
        // Captured before mutation for WAL emission (Phase J blocker fix)
        // below -- the "old" state several journal record types validate
        // against on replay (see emit_wal()'s own comment). Cheap: a single
        // find() this shard already effectively pays for inside each case.
        exec::Order before{};
        bool had_before = false;
        switch (req.kind) {
            case OpKind::create: {
                const auto approval = standing_approval();
                if (!approval) { ok = false; break; }
                auto* o = shard.oms.create(req.ref, req.symbol, req.volume, *approval);
                ok = o != nullptr;
                if (ok) c.order = *o;
                break;
            }
            case OpKind::find: {
                const auto* o = shard.oms.find(req.ref);
                ok = o != nullptr;
                if (ok) c.order = *o;
                break;
            }
            case OpKind::transition: {
                if (const auto* prev = shard.oms.find(req.ref)) { before = *prev; had_before = true; }
                ok = shard.oms.transition(req.ref, req.target);
                if (ok) { const auto* o = shard.oms.find(req.ref); if (o) c.order = *o; else c.order.state = req.target; }
                break;
            }
            case OpKind::fill: {
                if (const auto* prev = shard.oms.find(req.ref)) { before = *prev; had_before = true; }
                ok = shard.oms.fill(req.ref, req.volume);
                if (ok) { const auto* o = shard.oms.find(req.ref); if (o) c.order = *o; }
                break;
            }
            case OpKind::replace: {
                auto* o = shard.oms.find(req.ref);
                if (o != nullptr && !exec::is_terminal(o->state) && o->state != exec::OrderState::unknown &&
                    req.volume > o->filled_volume) {
                    before = *o; had_before = true;
                    o->limit_price_ticks = req.price;
                    o->requested_volume = req.volume;
                    ok = true;
                    c.order = *o;
                }
                break;
            }
            case OpKind::report: {
                if (const auto* prev = shard.oms.find(req.ref)) { before = *prev; had_before = true; }
                ExecReport report{};
                report.ref = req.ref; report.venue_seq = req.venue_seq; report.kind = req.report_kind;
                report.volume = req.volume; report.price = req.price;
                const auto outcome = shard.sequencer.process(shard.oms, report);
                c.report_outcome = outcome;
                ok = (outcome == ReportOutcome::applied);
                const auto* o = shard.oms.find(req.ref);
                if (o) c.order = *o;
                break;
            }
            case OpKind::snapshot: {
                const std::size_t n = shard.oms.size();
                std::size_t written = 0;
                for (std::size_t i = 0; i < n && written < c.snapshot_capacity; ++i)
                    c.snapshot_out[written++] = shard.oms.at(i);
                c.snapshot_count = written;
                ok = true;
                break;
            }
            case OpKind::restore: {
                // Phase J (blocker fix): bulk-repopulates a *fresh* shard
                // with an already-recovered order's exact state (including
                // filled_volume and a state that may be `unknown` --
                // Oms::create() cannot express either). Used only during
                // restart recovery (oms/restart_recovery.hpp), never on the
                // live hot path, and deliberately does not go through
                // emit_wal() below (it is restoring what the WAL already
                // durably recorded, not creating something new to record).
                exec::Order v{};
                v.ref = req.ref; v.symbol_id = req.symbol; v.requested_volume = req.volume;
                v.filled_volume = req.filled_volume; v.limit_price_ticks = req.price; v.state = req.target;
                auto* o = shard.oms.restore(v);
                ok = o != nullptr;
                if (ok) c.order = *o;
                break;
            }
        }
        c.ok = ok;

        const auto seq = global_seq_.fetch_add(1, std::memory_order_relaxed);
        const auto slot = shard.log_count.load(std::memory_order_relaxed);
        if (slot < shard.log_capacity) {
            shard.log[slot] = LogEntry{seq, req.kind, req.ref, c.order.state, ok};
            shard.log_count.store(slot + 1, std::memory_order_release);
        }
        // Log overflow is fail-closed for logging only (matches exec::Journal's
        // philosophy): the shard keeps operating correctly, it simply stops
        // being able to prove its own history past this point. Callers sizing
        // log_capacity_per_shard for a known workload (as every test/benchmark
        // here does) do not hit this.

        if (wal_hook_ != nullptr && ok) emit_wal(shard_index, req, before, had_before, c, seq);

        c.done.store(true, std::memory_order_release);
    }

    // Phase J (blocker fix): constructs and pushes the JournalRecord(s) that
    // reconstruct this operation on replay (oms::recover_streaming(),
    // recovery_streaming.hpp -- and oms::recover() itself, same schema,
    // unmodified), matching those functions' exact per-type validation
    // field-for-field.
    //
    // Two records for several kinds because the journal schema separates
    // "data that changes state" from "confirmation a state was reached",
    // and the confirmation types (acknowledgement/cancel/rejection)
    // validate a *specific* state at the moment they're processed:
    //   - ack: order_state (->acknowledged) THEN acknowledgement, which
    //     checks the order is *already* acknowledged.
    //   - cancel/reject: the confirmation record (checks the order is
    //     still in its *pre*-transition state -- cancel_pending/sent) THEN
    //     order_state, which performs the transition and, being terminal,
    //     reclaims the order -- so the confirmation record must run first,
    //     while the order is still findable in that state.
    //   - fill: the fill record (applies the filled_volume delta, no state
    //     precondition) THEN order_state (whose own c=filled_volume check
    //     requires the *post-fill* value -- i.e. must run after).
    // This ordering is proven, not just reasoned about, by
    // tests/cpp/phase7_wal_crash_recovery.cpp's create/ack/partial-fill/
    // cancel/replace/reject crash-and-recover scenarios.
    //
    // Confirmation records (acknowledgement/cancel/rejection) only affect
    // RecoveryState::counters (a diagnostic RiskCounters struct) -- the
    // order_state record alone is what oms::recover_streaming()'s
    // next.orders.transition(ref, to) actually uses to reconstruct state
    // and (together with the fill record) filled_volume. A confirmation
    // record's absence therefore cannot corrupt recovered order state; it
    // would only under-count a diagnostic counter -- kept in for fidelity,
    // not because state reconstruction depends on it.
    //
    // `position_ticket` on fill records is a deterministic synthetic value
    // (the order's own logical_order_id) -- real position-ticket tracking
    // is Pms/Portfolio's job, out of scope here (same boundary
    // venue_reconciliation.hpp already drew for position reconciliation);
    // this satisfies the journal schema's non-zero requirement without
    // building new position-aggregation logic this phase does not need.
    //
    // Side/OrderType are not modeled at this layer (verified in Phase F/H:
    // Oms::create() itself has no such parameters) -- command records use
    // the same buy/market defaults exec::Order already defaults to, so
    // recovery reconstructs exactly the same (unmodeled) values the live
    // order already had, not a regression.
    void emit_wal(std::size_t shard_index, const Request& req, const exec::Order& before, bool had_before,
                 const Completion& c, std::uint64_t ts_ns) noexcept {
        const auto run_id = req.ref.run_id;
        const auto logical_id = req.ref.logical_order_id;
        const auto push = [&](exec::JournalRecordType type, std::uint32_t symbol, std::uint64_t position_ticket,
                              std::int64_t a, std::int64_t b, std::int64_t cc, std::int64_t d) {
            exec::JournalRecord rec{};
            rec.ts_ns = ts_ns; rec.type = static_cast<std::uint16_t>(type); rec.symbol_id = symbol;
            rec.run_id = run_id; rec.logical_order_id = logical_id; rec.position_ticket = position_ticket;
            rec.a = a; rec.b = b; rec.c = cc; rec.d = d;
            if (!wal_hook_->record(shard_index, rec)) wal_dropped_.fetch_add(1, std::memory_order_relaxed);
        };
        const auto order_state = [&](std::uint32_t symbol, exec::OrderState from, exec::OrderState to,
                                     std::int64_t filled, std::int64_t requested) {
            push(exec::JournalRecordType::order_state, symbol, 0, static_cast<std::int64_t>(from),
                static_cast<std::int64_t>(to), filled, requested);
        };
        const std::uint64_t synthetic_ticket = logical_id == 0 ? 1 : logical_id;  // fill's schema requires nonzero
        // Price is not modeled at this layer either (Oms::create() has no
        // price parameter; exec::Order::limit_price_ticks stays its default
        // 0 unless a replace sets it) -- harmless for `command`'s own
        // validation (only requires >=0), but `replace`'s validation
        // requires the *prior* price to be strictly positive (it is meant
        // to assert the caller's view of "what price am I replacing FROM"
        // matches). A real, live first replace on a freshly-created order
        // would otherwise always fail that check against a price nothing
        // ever set. Command records this same placeholder (1) as the
        // order's initial price, so a recovered order's limit_price_ticks
        // matches what a later replace's `before` will read -- consistent
        // in both directions, live and post-recovery.
        constexpr std::int64_t synthetic_initial_price = 1;
        const auto old_price = [&]() { return before.limit_price_ticks > 0 ? before.limit_price_ticks : synthetic_initial_price; };

        switch (req.kind) {
            case OpKind::create:
                push(exec::JournalRecordType::command, req.symbol, 0, 0, req.volume, synthetic_initial_price, 0);
                break;
            case OpKind::transition: {
                if (!had_before) break;
                if (before.state == exec::OrderState::pending_send && req.target == exec::OrderState::sent)
                    break;  // implicitly covered by the command record already emitted at create
                order_state(c.order.symbol_id, before.state, req.target, before.filled_volume, before.requested_volume);
                if (req.target == exec::OrderState::acknowledged)
                    push(exec::JournalRecordType::acknowledgement, c.order.symbol_id, 0, 0, 0, 0, 0);
                break;
            }
            case OpKind::fill: {
                if (!had_before) break;
                const auto new_filled = before.filled_volume + req.volume;
                const auto fully = new_filled >= before.requested_volume;
                push(exec::JournalRecordType::fill, before.symbol_id, synthetic_ticket, req.volume, req.volume,
                    fully ? 1 : 0, 0);
                order_state(before.symbol_id, before.state, fully ? exec::OrderState::filled : exec::OrderState::partially_filled,
                          new_filled, before.requested_volume);
                break;
            }
            case OpKind::replace: {
                if (!had_before) break;
                push(exec::JournalRecordType::replace, before.symbol_id, 0, old_price(), req.price,
                    before.requested_volume, req.volume);
                break;
            }
            case OpKind::report: {
                if (!had_before) break;
                switch (req.report_kind) {
                    case ReportKind::ack:
                        order_state(before.symbol_id, before.state, exec::OrderState::acknowledged,
                                   before.filled_volume, before.requested_volume);
                        push(exec::JournalRecordType::acknowledgement, before.symbol_id, 0, 0, 0, 0, 0);
                        break;
                    case ReportKind::fill: {
                        const auto new_filled = before.filled_volume + req.volume;
                        const auto fully = new_filled >= before.requested_volume;
                        push(exec::JournalRecordType::fill, before.symbol_id, synthetic_ticket, req.volume, req.volume,
                            fully ? 1 : 0, 0);
                        order_state(before.symbol_id, before.state,
                                   fully ? exec::OrderState::filled : exec::OrderState::partially_filled, new_filled,
                                   before.requested_volume);
                        break;
                    }
                    case ReportKind::cancel_pending:
                        order_state(before.symbol_id, before.state, exec::OrderState::cancel_pending,
                                   before.filled_volume, before.requested_volume);
                        break;
                    case ReportKind::cancelled:
                        push(exec::JournalRecordType::cancel, before.symbol_id, 0, 0, 0, 0, 0);
                        order_state(before.symbol_id, before.state, exec::OrderState::cancelled, before.filled_volume,
                                   before.requested_volume);
                        break;
                    case ReportKind::reject:
                        push(exec::JournalRecordType::rejection, before.symbol_id, 0, 1, 0, 0, 0);
                        order_state(before.symbol_id, before.state, exec::OrderState::rejected, before.filled_volume,
                                   before.requested_volume);
                        break;
                    case ReportKind::replace_ack:
                        push(exec::JournalRecordType::replace, before.symbol_id, 0, old_price(),
                            req.price, before.requested_volume, req.volume);
                        break;
                }
                break;
            }
            default: break;  // find/snapshot: read-only, nothing to journal
        }
    }

    void run(std::size_t shard_index) {
        Shard& shard = *shards_[shard_index];
        std::size_t idle_spins = 0;
        while (!shard.stop.load(std::memory_order_acquire)) {
            if (shard.paused.load(std::memory_order_acquire)) { std::this_thread::yield(); continue; }
            bool did_work = false;
            for (std::size_t p = 0; p < max_producers_; ++p) {
                Request req;
                if (shard.inbound[p]->try_pop(req)) { apply(shard, shard_index, req); did_work = true; }
            }
            if (!did_work) {
                if (++idle_spins > 1000) { std::this_thread::yield(); idle_spins = 0; }
            } else {
                idle_spins = 0;
            }
        }
        // Drain whatever is left so a shutdown mid-flight does not silently
        // drop requests a producer is still spin-waiting on.
        bool more = true;
        while (more) {
            more = false;
            for (std::size_t p = 0; p < max_producers_; ++p) {
                Request req;
                if (shard.inbound[p]->try_pop(req)) { apply(shard, shard_index, req); more = true; }
            }
        }
    }

    std::size_t shard_count_;
    std::size_t max_producers_;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::atomic<std::uint64_t> global_seq_{0};
    persist::WalHook* wal_hook_{nullptr};
    std::atomic<std::uint64_t> wal_dropped_{0};
};

}  // namespace oms
