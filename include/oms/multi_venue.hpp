#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "exec/exec_types.hpp"
#include "oms/rate_limiter.hpp"
#include "oms/recovery_workflow.hpp"  // reuses ConnectionState + ReconciliationOutcome
#include "oms/report_sequencer.hpp"
#include "oms/sharded_oms.hpp"
#include "oms/venue_reconciliation.hpp"
#include "risk/risk_engine.hpp"
#include "risk/risk_gate.hpp"

// Phase F -- deterministic multi-exchange architecture.
//
// Reused unmodified: exec::BrokerOrderRef (Part 8.2, "globally unique per
// run"), oms::ShardedOms (Phase C), oms::ReportSequencer (Phase D, embedded
// one-per-shard inside ShardedOms already), oms::VenueAdapter/OrderReconciler
// (Phase E), oms::ConnectionState/ReconciliationOutcome (Phase E, this file
// only reuses the enum/struct shapes -- RecoveryWorkflow itself, which is
// built against a plain Oms&, is not reused: see VenueConnection below for
// why). risk::RiskEngine is reused via ShardedOms's own shared static engine
// (see sharded_oms.hpp's standing_engine()/halt_globally()), extended this
// phase (not rewritten) so halting it actually blocks order creation.
//
// New in this phase: ShardedOms::OpKind::snapshot + Completion::snapshot_* /
// submit_snapshot() (sharded_oms.hpp) -- the only way to safely read a
// shard's live orders from a thread that is not that shard's own worker.
// Everything in this file is orchestration on top of that plus the existing
// Phase D/E components; no new order-mutation logic is added anywhere.
//
// ---- venue identity (Part 1) -----------------------------------------------
//
// BrokerOrderRef{run_id, logical_order_id} is already "globally unique per
// run" (Part 8.2) and its equality already compares both fields. Assigning
// each venue a distinct run_id is therefore sufficient, by itself, to make
// "same BrokerOrderRef on two venues must never collide" hold: two orders
// with the same logical_order_id but different run_id already compare
// unequal everywhere BrokerOrderRef equality is used (Oms's hash index,
// ReportSequencer's per-order tracking, ShardedOms's shard routing) --
// without changing any of those types. VenueId is simply an alias for the
// run_id space; run_id_for(VenueId) below is the identity function, kept
// named/typed separately only so call sites read as "venue", not "run".
//
// ---- venue isolation (Part 4) ----------------------------------------------
//
// Each venue gets its own ShardedOms instance (VenueConnection::oms_) rather
// than a shared instance partitioned by venue: this gives isolation at the
// object level -- there is no shared mutable state between two venues'
// ShardedOms at all, so a bug or fault confined to one venue's shards
// structurally cannot touch another venue's. The one deliberate exception is
// the RiskEngine kill switch (Part 7's "existing global kill switch must
// remain authoritative"), which is process-wide by design -- see
// sharded_oms.hpp's standing_engine().
//
// ---- what "RECOVERING" means here vs. Phase E -----------------------------
//
// Phase E's RecoveryWorkflow models a process restart: local state was lost,
// so RECOVERING is "replay the WAL through oms::recover() and hope it
// succeeds" before RECONCILING can even start. A venue's ShardedOms here
// lives for the process's entire lifetime -- a venue disconnect/reconnect
// (network-level, the scenario Part 4's required tests actually exercise:
// "Venue A disconnect while Venue B remains healthy", "simultaneous venue
// reconnect") never loses that in-process state, so there is no journal to
// replay at this layer. RECOVERING here is therefore a pass-through: it
// exists so the state machine's shape matches Phase E's exactly (a caller
// watching connection_state() sees the same five states in the same order),
// but begin_recovery() cannot fail the way Phase E's can, because the thing
// it used to guard against (an unrecoverable local journal) is not a
// question this layer answers. A real process restart still goes through
// Phase E's WAL + oms::recover() path unchanged, per venue, at the layer
// below this one -- out of scope for this phase, not reimplemented here.

namespace oms {

using VenueId = std::uint64_t;

// ---- per-venue connection/reconciliation state -----------------------------

class VenueConnection final {
public:
    VenueConnection(VenueId id, std::unique_ptr<ShardedOms> venue_oms,
                     std::unique_ptr<VenueAdapter> adapter) noexcept
        : id_(id), oms_(std::move(venue_oms)), adapter_(std::move(adapter)) {}

    VenueConnection(const VenueConnection&) = delete;
    VenueConnection& operator=(const VenueConnection&) = delete;

    [[nodiscard]] VenueId id() const noexcept { return id_; }
    [[nodiscard]] ConnectionState state() const noexcept { return state_; }
    [[nodiscard]] ShardedOms& oms() noexcept { return *oms_; }
    [[nodiscard]] const ShardedOms& oms() const noexcept { return *oms_; }
    [[nodiscard]] const VenueAdapter& adapter() const noexcept { return *adapter_; }

    // Fail-closed gate (Part 4): only READY admits new exposure on this
    // venue. A disconnect on venue A flips only venue A's state -- venue B's
    // VenueConnection is a wholly separate object, untouched.
    [[nodiscard]] bool can_submit_new_exposure() const noexcept { return state_ == ConnectionState::ready; }

    void disconnect() noexcept { state_ = ConnectionState::disconnected; }

    // ---- Phase G: rate limiting + session constraints ---------------------
    //
    // Defaulted to permissive (VenueRateLimitConfig{}'s ~unlimited buckets,
    // SessionConstraints{}'s fully-open session) so a VenueConnection that
    // never calls configure_rate_limit()/set_session() behaves exactly as it
    // did in Phase F -- these are additive gates, opt-in per venue.
    void configure_rate_limit(const VenueRateLimitConfig& config, std::uint64_t now_ns) noexcept {
        limiter_.configure(config, now_ns);
    }
    void set_session(const SessionConstraints& session) noexcept { session_ = session; }
    [[nodiscard]] const SessionConstraints& session() const noexcept { return session_; }
    [[nodiscard]] VenueRateLimiter& rate_limiter() noexcept { return limiter_; }
    [[nodiscard]] const VenueRateLimiter& rate_limiter() const noexcept { return limiter_; }

    // Connection-state gate for *new* exposure only: broader than
    // can_submit_new_exposure() (which is READY-only, Phase E's original
    // post-disconnect-resync meaning, left unchanged for existing callers).
    // A venue that has simply never disconnected sits in CONNECTED forever
    // and must still be able to trade -- only DISCONNECTED/RECOVERING/
    // RECONCILING fail closed here, matching Part 4/5's "no new exposure
    // during recovery" without misclassifying ordinary steady-state
    // operation as blocked.
    [[nodiscard]] bool connection_allows_new_exposure() const noexcept {
        return state_ == ConnectionState::connected || state_ == ConnectionState::ready;
    }

    // The single gated entry point Phase G's tests use for order creation:
    // session -> connection state -> rate limiter -> ShardedOms, in that
    // order, so a request that fails an earlier gate never touches the
    // limiter's token bucket or ShardedOms at all. Kill switch remains
    // authoritative "before execution" exactly as in Phase F: it is still
    // checked last, inside ShardedOms::apply() via standing_approval(), at
    // the moment of actual creation -- this method adds gates in front of
    // that, it does not move or weaken it.
    [[nodiscard]] Decision route_create(std::size_t producer_id, exec::BrokerOrderRef ref, std::uint32_t symbol,
                                        std::int64_t volume, std::uint64_t now_ns, Completion& out) noexcept {
        if (!session_.allows_new_exposure() || !connection_allows_new_exposure()) return Decision::rejected;
        PendingRequest req{};
        req.kind = RateLimitKind::order;
        req.ref = ref;
        req.symbol = symbol;
        req.volume = volume;
        const auto decision = limiter_.admit(req, now_ns);
        if (decision != Decision::admitted) return decision;
        // Phase I: mandatory risk authority, checked after rate-limiting and
        // strictly before the OMS call -- closes the "raw submit_create
        // reachable" gap at its root for any caller of this method, not just
        // ones that go through RiskGatedRouter. No-op (nullptr) for every
        // pre-Phase-I call site, so Phase F/G/H's own tests are unaffected.
        if (risk_gate_ != nullptr && !risk_gate_->admit_create(id_, symbol, volume, ref)) return Decision::rejected;
        const bool ok = oms_->submit_create(producer_id, ref, symbol, volume, out) && out.ok;
        return ok ? Decision::admitted : Decision::rejected;
    }

    // Phase I: attaches the mandatory risk gate this venue's route_create()/
    // route_replace() check. Additive, opt-in -- see risk_gate.hpp.
    void attach_risk_gate(risk::RiskGate* gate) noexcept { risk_gate_ = gate; }

    // Risk-reducing: gated by SessionConstraints::allows_risk_reducing()
    // only (Part 5's "must remain possible where policy allows") -- not by
    // connection_allows_new_exposure(), since a cancel is exactly the action
    // that should still be reachable while a venue is mid-recovery, not
    // blocked alongside new exposure.
    [[nodiscard]] Decision route_cancel(std::size_t producer_id, exec::BrokerOrderRef ref, std::uint64_t now_ns,
                                        Completion& out) noexcept {
        if (!session_.allows_risk_reducing()) return Decision::rejected;
        PendingRequest req{};
        req.kind = RateLimitKind::cancel;
        req.ref = ref;
        const auto decision = limiter_.admit(req, now_ns);
        if (decision != Decision::admitted) return decision;
        const bool ok = oms_->submit_transition(producer_id, ref, exec::OrderState::cancel_pending, out) && out.ok;
        return ok ? Decision::admitted : Decision::rejected;
    }

    // Replace can increase requested size (Oms's own replace legality check
    // in sharded_oms.hpp's apply() already requires new_volume >
    // filled_volume, i.e. it never shrinks below what's already filled, but
    // it can grow), so this is treated as new-exposure-shaped, same gate as
    // route_create.
    [[nodiscard]] Decision route_replace(std::size_t producer_id, exec::BrokerOrderRef ref, std::int64_t new_price,
                                         std::int64_t new_volume, std::uint64_t now_ns, Completion& out) noexcept {
        if (!session_.allows_new_exposure() || !connection_allows_new_exposure()) return Decision::rejected;
        PendingRequest req{};
        req.kind = RateLimitKind::replace;
        req.ref = ref;
        req.volume = new_volume;
        req.price = new_price;
        const auto decision = limiter_.admit(req, now_ns);
        if (decision != Decision::admitted) return decision;
        if (risk_gate_ != nullptr && !risk_gate_->admit_replace(id_, ref, new_volume)) return Decision::rejected;
        const bool ok = oms_->submit_replace(producer_id, ref, new_price, new_volume, out) && out.ok;
        return ok ? Decision::admitted : Decision::rejected;
    }

    // Drains up to `max_n` ready deferred requests (their bucket now has a
    // token) and actually submits each to ShardedOms, in FIFO order. Returns
    // the number submitted. A caller drives this periodically (the
    // benchmark and tests do so explicitly, passing an advancing now_ns) --
    // there is no background thread, consistent with this codebase not
    // spawning timers of its own.
    std::size_t drain_pending(std::size_t producer_id, std::uint64_t now_ns, std::size_t max_n) noexcept {
        std::size_t drained = 0;
        PendingRequest req{};
        while (drained < max_n && limiter_.try_drain_one(now_ns, req)) {
            Completion out{};
            switch (req.kind) {
                case RateLimitKind::order: (void)oms_->submit_create(producer_id, req.ref, req.symbol, req.volume, out); break;
                case RateLimitKind::cancel: (void)oms_->submit_transition(producer_id, req.ref, exec::OrderState::cancel_pending, out); break;
                case RateLimitKind::replace: (void)oms_->submit_replace(producer_id, req.ref, req.price, req.volume, out); break;
            }
            ++drained;
        }
        return drained;
    }

    // DISCONNECTED -> RECOVERING -> RECONCILING. See the file-level comment
    // for why this cannot fail the way Phase E's begin_recovery() can.
    bool begin_recovery() noexcept {
        if (state_ != ConnectionState::disconnected) return false;
        state_ = ConnectionState::recovering;
        state_ = ConnectionState::reconciling;
        return true;
    }

    // RECONCILING: drain missed venue reports through this venue's own
    // ShardedOms (each report routes to its owning shard's ReportSequencer
    // via submit_report -- exactly-once, order-independent by venue_seq,
    // the same Phase D guarantee, now per-venue instead of per-process), then
    // compare a full snapshot of this venue's live orders against the
    // venue's own reported open orders. producer_id must be a slot this
    // caller thread owns on every shard of this venue's ShardedOms.
    //
    // snapshot_scratch is reused across shards sequentially (one
    // submit_snapshot call fully completes -- synchronous, blocking -- before
    // its contents are copied into local_scratch and the next shard is
    // queried), so a single caller-sized buffer is sufficient; it does not
    // need to hold every shard's orders at once.
    [[nodiscard]] ReconciliationOutcome run_reconciliation(
            std::size_t producer_id, ExecReport* report_scratch, std::size_t report_capacity,
            exec::Order* snapshot_scratch, std::size_t snapshot_capacity, OrderSnapshot* local_scratch,
            std::size_t local_capacity, OrderSnapshot* venue_scratch, std::size_t venue_capacity,
            OrderReconcileResult* result_out, std::size_t result_capacity) noexcept {
        ReconciliationOutcome outcome{};
        if (state_ != ConnectionState::reconciling) return outcome;

        const auto report_count = adapter_->fetch_recent_reports(report_scratch, report_capacity);
        for (std::size_t i = 0; i < report_count; ++i) {
            Completion c{};
            if (!oms_->submit_report(producer_id, report_scratch[i], c)) { ++outcome.reports_illegal; continue; }
            switch (c.report_outcome) {
                case ReportOutcome::applied: ++outcome.reports_applied; break;
                case ReportOutcome::duplicate: ++outcome.reports_duplicate; break;
                case ReportOutcome::held_for_gap: ++outcome.reports_held_for_gap; break;
                case ReportOutcome::illegal: ++outcome.reports_illegal; break;
                case ReportOutcome::unknown_order: ++outcome.reports_unknown_order; break;
                case ReportOutcome::terminal_late: break;  // correctly a no-op
                case ReportOutcome::gap_pool_exhausted: ++outcome.reports_illegal; break;
            }
        }

        std::size_t local_n = 0;
        for (std::size_t s = 0; s < oms_->shard_count(); ++s) {
            Completion c{};
            if (!oms_->submit_snapshot(producer_id, s, snapshot_scratch, snapshot_capacity, c)) continue;
            for (std::size_t i = 0; i < c.snapshot_count && local_n < local_capacity; ++i) {
                const auto& o = snapshot_scratch[i];
                if (exec::is_terminal(o.state)) continue;
                local_scratch[local_n++] = OrderSnapshot{o.ref, o.state, o.requested_volume, o.filled_volume};
                if (o.state == exec::OrderState::unknown) ++outcome.unresolved_unknown;
            }
        }
        const auto venue_n = adapter_->fetch_open_orders(venue_scratch, venue_capacity);

        OrderReconciler reconciler;
        const auto result_n =
            reconciler.compare(local_scratch, local_n, venue_scratch, venue_n, result_out, result_capacity);
        for (std::size_t i = 0; i < result_n; ++i) {
            switch (result_out[i].status) {
                case OrderReconcileStatus::local_only: ++outcome.local_only; break;
                case OrderReconcileStatus::venue_only: ++outcome.venue_only; break;
                case OrderReconcileStatus::state_mismatch: ++outcome.state_mismatch; break;
                case OrderReconcileStatus::volume_mismatch: ++outcome.volume_mismatch; break;
                case OrderReconcileStatus::matched: break;
            }
        }

        last_outcome_ = outcome;
        if (outcome.clean()) state_ = ConnectionState::ready;
        return outcome;
    }

    [[nodiscard]] const ReconciliationOutcome& last_outcome() const noexcept { return last_outcome_; }

private:
    VenueId id_;
    std::unique_ptr<ShardedOms> oms_;
    std::unique_ptr<VenueAdapter> adapter_;
    ConnectionState state_{ConnectionState::connected};
    ReconciliationOutcome last_outcome_{};
    VenueRateLimiter limiter_{};
    SessionConstraints session_{};
    risk::RiskGate* risk_gate_{nullptr};
};

// ---- registry: VenueId -> VenueConnection, no implicit default ------------
//
// Order routing (Part 5): there is deliberately no submit_create()-style
// method here that could pick "the first venue" or otherwise default --
// every routing call site in this file and in callers takes a VenueId
// explicitly. find() returning nullptr for an unknown id is itself the
// fail-closed behavior for a routing mistake, not a silent fallback.
class VenueRegistry final {
public:
    VenueConnection& add(VenueId id, std::unique_ptr<ShardedOms> venue_oms,
                         std::unique_ptr<VenueAdapter> adapter) {
        venues_.push_back(std::make_unique<VenueConnection>(id, std::move(venue_oms), std::move(adapter)));
        return *venues_.back();
    }

    [[nodiscard]] VenueConnection* find(VenueId id) noexcept {
        for (auto& v : venues_)
            if (v->id() == id) return v.get();
        return nullptr;
    }
    [[nodiscard]] const VenueConnection* find(VenueId id) const noexcept {
        for (auto& v : venues_)
            if (v->id() == id) return v.get();
        return nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept { return venues_.size(); }
    [[nodiscard]] VenueConnection& at(std::size_t i) noexcept { return *venues_[i]; }
    [[nodiscard]] const VenueConnection& at(std::size_t i) const noexcept { return *venues_[i]; }

    // Explicit routing helper: VenueId is always the first argument and
    // unknown venues fail closed (false), rather than this class offering
    // any path that submits without one.
    [[nodiscard]] bool submit_create(VenueId venue, std::size_t producer_id, exec::BrokerOrderRef ref,
                                     std::uint32_t symbol, std::int64_t volume, Completion& out) noexcept {
        auto* v = find(venue);
        if (v == nullptr) return false;
        return v->oms().submit_create(producer_id, ref, symbol, volume, out);
    }

private:
    std::vector<std::unique_ptr<VenueConnection>> venues_;
};

// ---- cross-venue exposure (Part 7) -----------------------------------------
//
// Oms only ever holds *live* orders -- a terminal order is reclaimed
// (swap-removed, Phase B) the instant it transitions, so scanning shard
// snapshots can only ever produce *open-order* exposure (unfilled remaining
// size of currently-resting orders), never a realized position ledger. A
// true position/PnL ledger is Pms/Portfolio's job, deliberately untouched by
// venue_reconciliation.hpp's own compare_positions() (Phase E) and equally
// out of scope here. What is implemented is honest about that: "exposure"
// below means resting, unfilled order size, signed by side, aggregated per
// symbol and per venue and summed across every registered venue.
struct SymbolExposure final {
    std::int64_t open_volume{0};  // signed: buy=+, sell=-
};

struct ExposureReport final {
    std::int64_t aggregate_open_volume{0};
    std::array<SymbolExposure, exec::max_symbols> by_symbol{};
};

// Computes one venue's exposure by snapshotting every shard (queue-routed,
// same mechanism reconciliation uses above) and folding requested-minus-
// filled volume, signed by side, into `out`. scratch must be sized for the
// largest single shard's live-order count. Symbols >= exec::max_symbols are
// silently dropped from the per-symbol breakdown (still counted in
// aggregate_open_volume) -- the same bounded-array convention
// compare_positions() already uses.
inline void accumulate_venue_exposure(ShardedOms& venue_oms, std::size_t producer_id, exec::Order* scratch,
                                       std::size_t scratch_capacity, ExposureReport& out) noexcept {
    for (std::size_t s = 0; s < venue_oms.shard_count(); ++s) {
        Completion c{};
        if (!venue_oms.submit_snapshot(producer_id, s, scratch, scratch_capacity, c)) continue;
        for (std::size_t i = 0; i < c.snapshot_count; ++i) {
            const auto& o = scratch[i];
            if (exec::is_terminal(o.state)) continue;
            const auto open = o.requested_volume - o.filled_volume;
            const auto signed_open = (o.side == exec::Side::buy) ? open : -open;
            out.aggregate_open_volume += signed_open;
            if (o.symbol_id < out.by_symbol.size()) out.by_symbol[o.symbol_id].open_volume += signed_open;
        }
    }
}

// Aggregate across every registered venue. Each venue's contribution is
// computed independently (accumulate_venue_exposure above), so one venue
// being disconnected/paused never blocks or skews another's contribution --
// consistent with Part 4's isolation requirement.
[[nodiscard]] inline ExposureReport aggregate_exposure(VenueRegistry& registry, std::size_t producer_id,
                                                        exec::Order* scratch, std::size_t scratch_capacity) noexcept {
    ExposureReport out{};
    for (std::size_t i = 0; i < registry.size(); ++i)
        accumulate_venue_exposure(registry.at(i).oms(), producer_id, scratch, scratch_capacity, out);
    return out;
}

// ---- deterministic multi-venue replay digest (Part 8) ---------------------
//
// Phase C's own digest proof (phase6_oms_concurrent.cpp) replays a captured
// merged_log() through a fresh single-threaded Oms and requires the result to
// be byte-identical across repeated replays of the *same* captured log --
// it does not claim two differently-scheduled runs converge to the same log,
// only that a given capture always replays the same way. This extends that
// exact claim to N venues by replaying each venue's own log independently
// (through an Oms constructed with that venue's identity) and folding the
// per-venue results together in ascending VenueId order -- a static,
// registration-time order, never derived from runtime thread interleaving --
// so the combined digest is deterministic for the same reason each per-venue
// digest already is.
//
// Limitation inherited unchanged from Phase C: LogEntry (sharded_oms.hpp)
// captures (global_seq, kind, ref, result_state, ok) but not the original
// request's symbol/volume, so replay here -- like Phase C's own -- only
// reconstructs `create` operations faithfully (matching the create-only
// workload both Phase C's and this phase's determinism tests actually use);
// other op kinds are intentionally skipped rather than replayed incorrectly.
[[nodiscard]] inline std::uint64_t venue_replay_digest(VenueId venue_id, const std::vector<LogEntry>& log,
                                                        std::size_t oms_capacity) {
    Oms replay(oms_capacity);
    risk::RiskEngine engine(risk::Limits{});
    risk::Request q{};
    q.volume = 1; q.risk_minor = 1; q.free_margin = 1'000'000; q.warm_mask = 1; q.session_open = true;
    const auto token = engine.check(q).token;
    for (const auto& entry : log) {
        if (entry.kind == OpKind::create) (void)replay.create(entry.ref, 0, 1, token);
    }

    std::uint64_t hash = 1469598103934665603ULL ^ venue_id;
    const auto mix = [&hash](std::uint64_t v) {
        hash ^= v;
        hash *= 1099511628211ULL;
    };
    mix(replay.size());
    for (std::size_t i = 0; i < replay.size(); ++i) {
        const auto& o = replay.at(i);
        mix(o.ref.run_id);
        mix(o.ref.logical_order_id);
        mix(static_cast<std::uint64_t>(o.state));
    }
    return hash;
}

[[nodiscard]] inline std::uint64_t multi_venue_digest(const VenueRegistry& registry, std::size_t oms_capacity) {
    std::vector<VenueId> ids;
    ids.reserve(registry.size());
    for (std::size_t i = 0; i < registry.size(); ++i) ids.push_back(registry.at(i).id());
    std::sort(ids.begin(), ids.end());

    std::uint64_t combined = 1469598103934665603ULL;
    for (const auto id : ids) {
        const auto* v = registry.find(id);
        if (v == nullptr) continue;
        const auto digest = venue_replay_digest(id, v->oms().merged_log(), oms_capacity);
        combined ^= digest;
        combined *= 1099511628211ULL;
    }
    return combined;
}

}  // namespace oms
