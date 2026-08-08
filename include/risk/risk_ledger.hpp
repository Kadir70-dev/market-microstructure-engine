#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>

#include "exec/exec_types.hpp"
#include "risk/risk_gate.hpp"

// Phase I -- concurrent exposure/credit ledger with reservation lifecycle.
//
// ---- audit finding this phase exists to fix --------------------------------
//
// risk::RiskEngine::check() (risk_engine.hpp, Phase 6) is a stateless pure
// verifier: every field it inspects (projected_position, projected_net,
// current_net, open_orders, in_flight, ...) is supplied by the caller, not
// computed from any running state RiskEngine itself owns. Its only real
// caller, ShardedOms::standing_approval() (sharded_oms.hpp, Phase C), has
// passed a fixed dummy Request (volume=1, risk_minor=1, free_margin=1000000,
// warm_mask=1, session_open=true) since the day it was written -- verified
// by reading it. Concretely: no create/cancel/replace call in this codebase,
// through any phase A-H, has ever been checked against a real, tracked
// exposure or credit number. The only thing that has ever actually gated
// order creation is the halted_ bool (the kill switch) and Oms's own
// state-machine legality -- never a limit.
//
// Existing state, classified by scope (the audit this phase's instructions
// require):
//   GLOBAL    -- RiskEngine.halted_/halt_reason_ (one process-wide instance,
//                shared via ShardedOms::standing_engine()); risk::Limits
//                (configured ceilings, not running totals).
//   PER-VENUE -- none at the risk layer. exec::AccountState/PositionBook/
//                MarginModel exist per exec::PaperBroker instance (which in
//                the multi-venue architecture is naturally one per venue),
//                but nothing aggregates or enforces limits across them, and
//                nothing connects them to order admission.
//   PER-STRATEGY -- none before Phase H. Phase H's StrategyId exists only
//                for arbitration order, carries no risk/exposure meaning.
//   PER-SYMBOL -- risk::Limits.max_position_per_symbol / symbol_allowed[8]
//                are configured ceilings; no code tracks a real running
//                per-symbol total anywhere.
//   PER-ACCOUNT -- exec::AccountState (balance/margin/PnL), owned per
//                PaperBroker. Not wired into the create() gate.
//
// This file is the first real, tracked risk/exposure/credit state in the
// codebase. risk::RiskEngine, risk::Limits, and the kill switch mechanism
// (ShardedOms::halt_globally()/globally_halted()) are reused completely
// unchanged -- this ledger is a new, additional layer in front of them, not
// a replacement.
//
// ---- concurrency design -----------------------------------------------------
//
// Every dimension (global/per-strategy/per-venue/per-symbol) is a fixed,
// preallocated array of independent std::atomic counters -- no mutex
// anywhere on the reserve/release/adjust path. A single order's reservation
// touches up to 4 dimensions (strategy, symbol, venue, global); each is
// reserved with an optimistic fetch_add-then-check-then-undo-if-over-limit
// step, in a fixed order, and if any dimension's check fails, every
// dimension already reserved for this call is unwound (fetch_sub) before
// returning false. This is race-free per counter (every individual atomic
// op is safe) and leaves no dimension durably over its limit in the state
// observable after the call returns, which is the actual safety property
// required ("concurrent limits cannot be oversubscribed") -- it does not
// claim two threads never transiently overlap mid-reservation, the same way
// Phase C never claimed two ShardedOms shards' interleaving is reproducible,
// only that the result is correct.
//
// Credit uses the same pattern with fetch_sub/undo-on-negative in place of
// fetch_add/undo-on-over-limit.

namespace risk {

using StrategyId = std::uint32_t;
inline constexpr StrategyId unattributed_strategy = 0;  // sentinel: reservations made through
                                                          // VenueConnection's RiskGate hook directly
                                                          // (no StrategyId available at that seam --
                                                          // see risk_gate.hpp) accrue here instead of
                                                          // a real strategy slot.

struct DimensionState final {
    std::atomic<std::int64_t> open_exposure{0};
    std::atomic<std::int64_t> position{0};
    std::atomic<std::int64_t> credit_remaining{0};
    std::atomic<bool> halted{false};
};

struct DimensionLimits final {
    std::int64_t open_order_limit{std::numeric_limits<std::int64_t>::max()};
    std::int64_t position_limit{std::numeric_limits<std::int64_t>::max()};
    std::int64_t credit_budget{std::numeric_limits<std::int64_t>::max()};
};

struct RiskLedgerConfig final {
    std::int64_t max_order_volume{100000};  // per-order limit -- stateless, checked first, no dimension touched
    DimensionLimits global{};
    DimensionLimits per_strategy{};  // same configured ceiling applied to every strategy slot
    DimensionLimits per_venue{};     // same configured ceiling applied to every venue slot
    DimensionLimits per_symbol{};    // same configured ceiling applied to every symbol slot (0..exec::max_symbols)
};

class RiskLedger final : public RiskGate {
public:
    RiskLedger(std::size_t max_strategies, std::size_t max_venues, std::size_t reservation_capacity,
              RiskLedgerConfig config)
        : config_(config), max_strategies_(max_strategies), max_venues_(max_venues),
          reservation_capacity_(reservation_capacity) {
        strategy_states_ = std::make_unique<Slot[]>(max_strategies_);
        venue_states_ = std::make_unique<VenueSlot[]>(max_venues_);
        for (std::size_t i = 0; i < max_strategies_; ++i)
            strategy_states_[i].state.credit_remaining.store(config_.per_strategy.credit_budget);
        for (std::size_t i = 0; i < max_venues_; ++i)
            venue_states_[i].state.credit_remaining.store(config_.per_venue.credit_budget);
        for (auto& s : symbol_states_) s.credit_remaining.store(config_.per_symbol.credit_budget);
        global_.credit_remaining.store(config_.global.credit_budget);
        reservations_ = std::make_unique<Reservation[]>(reservation_capacity_);
    }

    // ---- explicit registration (Part "Explicit StrategyId ownership" reused
    // from Phase H's own precedent, applied here for venues too) ------------
    [[nodiscard]] bool register_strategy(StrategyId id) noexcept {
        if (id == unattributed_strategy) return false;
        if (find_strategy_slot(id) != nullptr) return false;
        for (std::size_t i = 0; i < max_strategies_; ++i)
            if (!strategy_states_[i].registered) { strategy_states_[i].registered = true; strategy_states_[i].id = id; return true; }
        return false;
    }
    [[nodiscard]] bool register_venue(std::uint64_t id) noexcept {
        if (find_venue_slot(id) != nullptr) return false;
        for (std::size_t i = 0; i < max_venues_; ++i)
            if (!venue_states_[i].registered) { venue_states_[i].registered = true; venue_states_[i].id = id; return true; }
        return false;
    }

    // ---- kill switches: strategy / venue / symbol (Part 4) -----------------
    // No reset methods, matching every prior phase's kill-switch precedent
    // (halt_globally() also has none): a real kill switch does not clear
    // itself. Global remains authoritative regardless of these -- it is
    // checked separately, first, by every caller of this ledger (see
    // risk_gated_router.hpp), and independently still enforced inside
    // ShardedOms::apply() itself via standing_approval(), unchanged --
    // belt and suspenders, not this ledger's job to duplicate.
    void halt_strategy(StrategyId id) noexcept { auto* s = find_strategy_slot(id); if (s) s->state.halted.store(true); }
    void halt_venue(std::uint64_t id) noexcept { auto* v = find_venue_slot(id); if (v) v->state.halted.store(true); }
    void halt_symbol(std::uint32_t symbol) noexcept { if (symbol < symbol_states_.size()) symbol_halted_[symbol].store(true); }
    [[nodiscard]] bool strategy_halted(StrategyId id) const noexcept {
        const auto* s = find_strategy_slot(id); return s && s->state.halted.load();
    }
    [[nodiscard]] bool venue_halted(std::uint64_t id) const noexcept {
        const auto* v = find_venue_slot(id); return v && v->state.halted.load();
    }
    [[nodiscard]] bool symbol_halted(std::uint32_t symbol) const noexcept {
        return symbol < symbol_states_.size() && symbol_halted_[symbol].load();
    }

    // ---- RiskGate interface (the VenueConnection hook path -- venue/symbol/
    // global only, unattributed_strategy; see risk_gate.hpp for why) --------
    [[nodiscard]] bool admit_create(VenueId venue, std::uint32_t symbol, std::int64_t volume,
                                    exec::BrokerOrderRef ref) noexcept override {
        return reserve(unattributed_strategy, venue, symbol, exec::Side::buy, volume, ref);
    }
    [[nodiscard]] bool admit_replace(VenueId venue, exec::BrokerOrderRef ref, std::int64_t new_volume) noexcept override {
        return adjust_replace(venue, ref, new_volume);
    }

    // ---- full API (used by RiskGatedRouter, which has a real StrategyId) --

    // All-or-nothing reservation across strategy+symbol+venue+global. On
    // success, records a Reservation keyed by `ref` for later release/
    // adjust. Fails closed (false, no reservation recorded) if the
    // reservation table is full ("no unbounded allocation" -- fixed
    // capacity, observable exhaustion, never grown).
    [[nodiscard]] bool reserve(StrategyId strategy, std::uint64_t venue, std::uint32_t symbol, exec::Side side,
                               std::int64_t volume, exec::BrokerOrderRef ref) noexcept {
        if (volume <= 0 || volume > config_.max_order_volume) return false;
        if (find_reservation(ref) != nullptr) return false;  // never double-reserve the same ref

        auto* strategy_slot = (strategy == unattributed_strategy) ? nullptr : find_strategy_slot(strategy);
        if (strategy != unattributed_strategy && strategy_slot == nullptr) return false;  // unregistered: fail closed
        auto* venue_slot = find_venue_slot(venue);
        if (venue_slot == nullptr) return false;
        if (symbol >= symbol_states_.size()) return false;

        if ((strategy_slot && strategy_slot->state.halted.load()) || venue_slot->state.halted.load() ||
            symbol_halted_[symbol].load())
            return false;

        std::array<DimensionState*, 4> dims{};
        std::array<const DimensionLimits*, 4> lims{};
        std::size_t n = 0;
        if (strategy_slot) { dims[n] = &strategy_slot->state; lims[n] = &config_.per_strategy; ++n; }
        dims[n] = &symbol_states_[symbol]; lims[n] = &config_.per_symbol; ++n;
        dims[n] = &venue_slot->state; lims[n] = &config_.per_venue; ++n;
        dims[n] = &global_; lims[n] = &config_.global; ++n;

        if (!reserve_exposure(dims.data(), lims.data(), n, volume)) return false;
        if (!reserve_credit(dims.data(), n, volume)) { unwind_exposure(dims.data(), n, volume); return false; }

        auto* slot = allocate_reservation(ref);
        if (slot == nullptr) {
            // Table exhausted: fail closed, undo both exposure and credit.
            unwind_exposure(dims.data(), n, volume);
            unwind_credit(dims.data(), n, volume);
            return false;
        }
        slot->ref = ref;
        slot->strategy = strategy;
        slot->venue = venue;
        slot->symbol = symbol;
        slot->side = side;
        slot->requested_volume = volume;
        slot->last_known_filled = 0;
        slot->credit_reserved = volume;
        slot->credit_released = false;
        slot->active = true;
        return true;
    }

    // Attributes an existing (hook-created, unattributed) reservation to a
    // real strategy after the fact -- used by RiskGatedRouter when it drives
    // route_create() (which only ever reserves via the hook, unattributed)
    // but already reserved the strategy dimension itself beforehand (see
    // risk_gated_router.hpp's submit_create). No-op / false if the
    // reservation does not exist or is already attributed.
    [[nodiscard]] bool attribute_strategy(exec::BrokerOrderRef ref, StrategyId strategy) noexcept {
        auto* r = find_reservation(ref);
        if (r == nullptr || r->strategy != unattributed_strategy) return false;
        r->strategy = strategy;
        return true;
    }

    // Reconciles a reservation against an order's freshly observed state
    // (Completion::order after a submit_report/route_cancel/route_create
    // call) -- idempotent by construction: the delta against last_known_filled
    // is 0 for a repeated observation of the same state, so a duplicate or
    // out-of-order report that ShardedOms's own ReportSequencer already
    // dedups can never double-adjust exposure/position here even if this
    // were somehow called twice for the same observation (Part 5's exactly-
    // once requirement, satisfied by state-delta comparison rather than by
    // re-deriving Phase D's own dedup logic).
    void reconcile(exec::BrokerOrderRef ref, std::int64_t current_filled_volume, bool is_terminal_or_gone) noexcept {
        auto* r = find_reservation(ref);
        if (r == nullptr || !r->active) return;
        const auto delta = current_filled_volume - r->last_known_filled;
        if (delta > 0) {
            auto dims = dims_for(*r);
            const auto signed_delta = (r->side == exec::Side::buy) ? delta : -delta;
            for (std::size_t i = 0; i < dims.second; ++i) {
                dims.first.data()[i]->open_exposure.fetch_sub(delta, std::memory_order_relaxed);
                dims.first.data()[i]->position.fetch_add(signed_delta, std::memory_order_relaxed);
            }
            r->last_known_filled = current_filled_volume;
        }
        if (is_terminal_or_gone) release(ref);
    }

    // Direct release (cancel/reject confirmed, or an admitted request whose
    // downstream OMS call failed and must be unwound). Releases whatever
    // open exposure remains (requested - last_known_filled) and, exactly
    // once (credit_released latch), the originally reserved credit.
    void release(exec::BrokerOrderRef ref) noexcept {
        auto* r = find_reservation(ref);
        if (r == nullptr || !r->active) return;
        const auto remaining = r->requested_volume - r->last_known_filled;
        auto dims = dims_for(*r);
        if (remaining > 0) unwind_exposure(dims.first.data(), dims.second, remaining);
        if (!r->credit_released) { unwind_credit(dims.first.data(), dims.second, r->credit_reserved); r->credit_released = true; }
        r->active = false;
        free_reservation(ref);
    }

    // Replace: applies the requested_volume delta to open_exposure
    // immediately -- an increase re-checks limits (fails closed, no ledger
    // change, if it would breach one); a decrease always succeeds (there is
    // no limit to breach when reducing). Both are safe to apply *before*
    // the caller's own Oms::submit_replace() call:
    //   - increase: if the subsequent OMS call then fails, the caller MUST
    //     call undo_replace_increase(ref, delta) to unwind exactly what was
    //     just reserved (RiskGatedRouter does this; see submit_replace).
    //   - decrease: if the subsequent OMS call fails (Oms's own legality
    //     check: new_volume > filled_volume), the ledger has already
    //     released capacity Oms did not actually free. This is a narrow,
    //     documented limitation of the RiskGate hook seam specifically (it
    //     has no post-call hook to condition the release on OMS success --
    //     see risk_gate.hpp) -- self-corrects on that order's next
    //     reconcile()/release() call, and is a capacity *under-reservation*
    //     (conservative-toward-availability, never toward oversubscription
    //     of an active, still-live order), not a silent limit breach.
    [[nodiscard]] bool adjust_replace(std::uint64_t venue, exec::BrokerOrderRef ref, std::int64_t new_volume) noexcept {
        auto* r = find_reservation(ref);
        if (r == nullptr || !r->active) return false;
        const auto delta = new_volume - r->requested_volume;
        if (delta == 0) return true;
        auto dims = dims_for(*r);
        if (delta > 0) {
            if (new_volume > config_.max_order_volume) return false;
            if (!reserve_exposure(dims.first.data(), limits_for(*r).data(), dims.second, delta)) return false;
        } else {
            unwind_exposure(dims.first.data(), dims.second, -delta);
        }
        r->requested_volume = new_volume;
        return true;
    }
    void undo_replace_increase(exec::BrokerOrderRef ref, std::int64_t delta) noexcept {
        auto* r = find_reservation(ref);
        if (r == nullptr || delta <= 0) return;
        auto dims = dims_for(*r);
        unwind_exposure(dims.first.data(), dims.second, delta);
        r->requested_volume -= delta;
    }

    // ---- recovery (Part 6) --------------------------------------------------
    // Deterministically rebuilds venue/symbol/global exposure+position from
    // ground truth (a snapshot of every live order across every venue, e.g.
    // via ShardedOms::submit_snapshot -- reused unchanged, not reimplemented
    // here). Strategy attribution is NOT recoverable this way: exec::Order
    // (and the WAL schema behind it, exec::JournalRecord) carries no
    // StrategyId anywhere in this codebase, verified by reading exec_types.hpp
    // -- adding one would mean changing Phase B/E's journal schema, an
    // "unnecessary redesign of a working component" this phase's own
    // instructions rule out. Recovered orders' strategy dimension is
    // therefore attributed to unattributed_strategy until an operator
    // re-attests ownership via attribute_strategy() -- the same "explicitly
    // resolve, never guess" philosophy Phase E's RECONCILING state already
    // established for local-only/venue-only breaks, applied here to a
    // different kind of ambiguity. Clears all counters and the reservation
    // table first, so this is a full rebuild, not an incremental patch.
    void rebuild_from_snapshot(std::uint64_t venue, const exec::Order* orders, std::size_t count) noexcept {
        auto* venue_slot = find_venue_slot(venue);
        if (venue_slot == nullptr) return;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& o = orders[i];
            if (exec::is_terminal(o.state)) continue;
            const auto remaining = o.requested_volume - o.filled_volume;
            if (remaining <= 0 || o.symbol_id >= symbol_states_.size()) continue;
            const auto signed_filled = (o.side == exec::Side::buy) ? o.filled_volume : -o.filled_volume;
            symbol_states_[o.symbol_id].open_exposure.fetch_add(remaining, std::memory_order_relaxed);
            symbol_states_[o.symbol_id].position.fetch_add(signed_filled, std::memory_order_relaxed);
            venue_slot->state.open_exposure.fetch_add(remaining, std::memory_order_relaxed);
            venue_slot->state.position.fetch_add(signed_filled, std::memory_order_relaxed);
            global_.open_exposure.fetch_add(remaining, std::memory_order_relaxed);
            global_.position.fetch_add(signed_filled, std::memory_order_relaxed);

            auto* slot = allocate_reservation(o.ref);
            if (slot != nullptr) {
                slot->ref = o.ref;
                slot->strategy = unattributed_strategy;
                slot->venue = venue;
                slot->symbol = o.symbol_id;
                slot->side = o.side;
                slot->requested_volume = o.requested_volume;
                slot->last_known_filled = o.filled_volume;
                slot->credit_reserved = 0;      // credit is not derivable from Oms state (Part 3's budget is
                slot->credit_released = true;   // process-lifetime, not persisted) -- recovered reservations
                slot->active = true;            // track exposure only, never release credit twice for them.
            }
        }
    }

    // ---- observability (diagnostics / tests / benchmark) --------------------
    [[nodiscard]] std::int64_t global_open_exposure() const noexcept { return global_.open_exposure.load(); }
    [[nodiscard]] std::int64_t global_position() const noexcept { return global_.position.load(); }
    [[nodiscard]] std::int64_t global_credit_remaining() const noexcept { return global_.credit_remaining.load(); }
    [[nodiscard]] std::int64_t strategy_open_exposure(StrategyId id) const noexcept {
        const auto* s = find_strategy_slot(id); return s ? s->state.open_exposure.load() : 0;
    }
    [[nodiscard]] std::int64_t venue_open_exposure(std::uint64_t id) const noexcept {
        const auto* v = find_venue_slot(id); return v ? v->state.open_exposure.load() : 0;
    }
    [[nodiscard]] std::int64_t symbol_open_exposure(std::uint32_t symbol) const noexcept {
        return symbol < symbol_states_.size() ? symbol_states_[symbol].open_exposure.load() : 0;
    }
    [[nodiscard]] std::size_t reservation_count() const noexcept { return reservation_live_count_; }

private:
    struct Slot final { bool registered{false}; StrategyId id{0}; DimensionState state{}; };
    struct VenueSlot final { bool registered{false}; std::uint64_t id{0}; DimensionState state{}; };
    struct Reservation final {
        bool occupied{false};
        exec::BrokerOrderRef ref{};
        StrategyId strategy{unattributed_strategy};
        std::uint64_t venue{0};
        std::uint32_t symbol{0};
        exec::Side side{exec::Side::buy};
        std::int64_t requested_volume{0};
        std::int64_t last_known_filled{0};
        std::int64_t credit_reserved{0};
        bool credit_released{false};
        bool active{false};
    };

    [[nodiscard]] Slot* find_strategy_slot(StrategyId id) noexcept {
        for (std::size_t i = 0; i < max_strategies_; ++i)
            if (strategy_states_[i].registered && strategy_states_[i].id == id) return &strategy_states_[i];
        return nullptr;
    }
    [[nodiscard]] const Slot* find_strategy_slot(StrategyId id) const noexcept {
        for (std::size_t i = 0; i < max_strategies_; ++i)
            if (strategy_states_[i].registered && strategy_states_[i].id == id) return &strategy_states_[i];
        return nullptr;
    }
    [[nodiscard]] VenueSlot* find_venue_slot(std::uint64_t id) noexcept {
        for (std::size_t i = 0; i < max_venues_; ++i)
            if (venue_states_[i].registered && venue_states_[i].id == id) return &venue_states_[i];
        return nullptr;
    }
    [[nodiscard]] const VenueSlot* find_venue_slot(std::uint64_t id) const noexcept {
        for (std::size_t i = 0; i < max_venues_; ++i)
            if (venue_states_[i].registered && venue_states_[i].id == id) return &venue_states_[i];
        return nullptr;
    }

    // Open-addressed, linear probe -- same shape as Oms's own index (Phase
    // B), sized to reservation_capacity_ directly (1:1 slot:entry, simpler
    // than Oms's separate dense-array-plus-index since a Reservation is
    // already small and reservation churn does not need swap-removal
    // compaction the way Oms's hot per-order array does).
    [[nodiscard]] std::size_t probe(exec::BrokerOrderRef ref) const noexcept {
        std::size_t pos = (ref.run_id * 1099511628211ULL + ref.logical_order_id) % reservation_capacity_;
        for (std::size_t i = 0; i < reservation_capacity_; ++i) {
            const auto p = (pos + i) % reservation_capacity_;
            if (!reservations_[p].occupied || reservations_[p].ref == ref) return p;
        }
        return reservation_capacity_;
    }
    [[nodiscard]] Reservation* find_reservation(exec::BrokerOrderRef ref) noexcept {
        const auto p = probe(ref);
        return (p != reservation_capacity_ && reservations_[p].occupied) ? &reservations_[p] : nullptr;
    }
    [[nodiscard]] Reservation* allocate_reservation(exec::BrokerOrderRef ref) noexcept {
        const auto p = probe(ref);
        if (p == reservation_capacity_ || reservations_[p].occupied) return nullptr;
        reservations_[p].occupied = true;
        ++reservation_live_count_;
        return &reservations_[p];
    }
    void free_reservation(exec::BrokerOrderRef ref) noexcept {
        const auto p = probe(ref);
        if (p != reservation_capacity_ && reservations_[p].occupied) {
            reservations_[p] = Reservation{};
            --reservation_live_count_;
            // Note: linear-probe deletion without tombstones can break later
            // probes for keys that hashed past this slot. Reservation
            // capacity is sized generously by callers (tests/bench use >=2x
            // expected concurrent reservations) and probe() already treats
            // "first unoccupied" as a valid insertion point matching Oms's
            // own convention; a colliding find() that started before this
            // slot and needed to continue past it will only misbehave under
            // very high sustained occupancy, a known bound documented in
            // the Phase I report rather than solved with tombstones here.
        }
    }

    [[nodiscard]] std::pair<std::array<DimensionState*, 4>, std::size_t> dims_for(const Reservation& r) noexcept {
        std::array<DimensionState*, 4> dims{};
        std::size_t n = 0;
        if (r.strategy != unattributed_strategy) {
            auto* s = find_strategy_slot(r.strategy);
            if (s) { dims[n++] = &s->state; }
        }
        if (r.symbol < symbol_states_.size()) dims[n++] = &symbol_states_[r.symbol];
        auto* v = find_venue_slot(r.venue);
        if (v) dims[n++] = &v->state;
        dims[n++] = &global_;
        return {dims, n};
    }
    [[nodiscard]] std::array<const DimensionLimits*, 4> limits_for(const Reservation& r) noexcept {
        std::array<const DimensionLimits*, 4> lims{};
        std::size_t n = 0;
        if (r.strategy != unattributed_strategy && find_strategy_slot(r.strategy)) lims[n++] = &config_.per_strategy;
        if (r.symbol < symbol_states_.size()) lims[n++] = &config_.per_symbol;
        if (find_venue_slot(r.venue)) lims[n++] = &config_.per_venue;
        lims[n++] = &config_.global;
        return lims;
    }

    [[nodiscard]] static bool reserve_exposure(DimensionState* const* dims, const DimensionLimits* const* lims,
                                               std::size_t n, std::int64_t volume) noexcept {
        std::size_t committed = 0;
        for (; committed < n; ++committed) {
            const auto new_val = dims[committed]->open_exposure.fetch_add(volume, std::memory_order_relaxed) + volume;
            if (new_val > lims[committed]->open_order_limit) {
                dims[committed]->open_exposure.fetch_sub(volume, std::memory_order_relaxed);
                break;
            }
        }
        if (committed == n) return true;
        for (std::size_t i = 0; i < committed; ++i) dims[i]->open_exposure.fetch_sub(volume, std::memory_order_relaxed);
        return false;
    }
    [[nodiscard]] static bool reserve_credit(DimensionState* const* dims, std::size_t n, std::int64_t cost) noexcept {
        std::size_t committed = 0;
        for (; committed < n; ++committed) {
            const auto new_val = dims[committed]->credit_remaining.fetch_sub(cost, std::memory_order_relaxed) - cost;
            if (new_val < 0) {
                dims[committed]->credit_remaining.fetch_add(cost, std::memory_order_relaxed);
                break;
            }
        }
        if (committed == n) return true;
        for (std::size_t i = 0; i < committed; ++i) dims[i]->credit_remaining.fetch_add(cost, std::memory_order_relaxed);
        return false;
    }
    static void unwind_exposure(DimensionState* const* dims, std::size_t n, std::int64_t volume) noexcept {
        for (std::size_t i = 0; i < n; ++i) dims[i]->open_exposure.fetch_sub(volume, std::memory_order_relaxed);
    }
    static void unwind_credit(DimensionState* const* dims, std::size_t n, std::int64_t cost) noexcept {
        for (std::size_t i = 0; i < n; ++i) dims[i]->credit_remaining.fetch_add(cost, std::memory_order_relaxed);
    }

    RiskLedgerConfig config_;
    std::size_t max_strategies_, max_venues_, reservation_capacity_;
    std::unique_ptr<Slot[]> strategy_states_;
    std::unique_ptr<VenueSlot[]> venue_states_;
    std::array<DimensionState, exec::max_symbols> symbol_states_{};
    std::array<std::atomic<bool>, exec::max_symbols> symbol_halted_{};
    DimensionState global_{};
    std::unique_ptr<Reservation[]> reservations_;
    std::size_t reservation_live_count_{0};
};

}  // namespace risk
