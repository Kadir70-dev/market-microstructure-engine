#pragma once
#include <array>
#include <cstdint>
#include <memory>

#include "oms/multi_venue.hpp"
#include "oms/rate_limiter.hpp"
#include "risk/self_trade_prevention.hpp"

// Phase H -- deterministic multi-strategy scheduling and fair arbitration,
// layered on top of Phase F's multi-venue architecture and Phase G's
// per-venue rate limiting/session gates. Nothing in Phase B-G is modified:
// StrategyScheduler and SelfTradeTracker are new, independent types that sit
// strictly *before* VenueConnection::route_*() in the call chain -- they
// decide *which strategy's queued request goes next*, then hand it to the
// existing, unmodified Phase F/G routing path, which still enforces session,
// connection state, per-venue rate limits, and (inside ShardedOms::apply())
// the kill switch, exactly as before. Arbitration order is therefore never
// able to bypass any of those gates -- it only ever decides *order*, never
// *admission*.
//
// ---- why deficit round robin ----------------------------------------------
//
// Requirement: weighted fairness, no starvation, fully deterministic (no
// wall-clock-derived decisions). DRR gives all three from one integer
// mechanism, using unit-cost items (this phase's fairness unit is "one
// request", not order size/notional):
//
//   - A fixed-order round-robin cursor visits registered strategy slots.
//   - On arriving at a slot with a non-empty queue, its quantum (configured
//     weight) is added to a per-slot deficit; the slot is then served
//     (dequeue + route) repeatedly, decrementing the deficit by 1 per
//     request, until either the deficit drops to 0 or the queue empties --
//     whichever happens first -- at which point the cursor unconditionally
//     advances to the next slot. This is what makes quantum a *rate*, not
//     just a visit frequency: a quantum=3 strategy drains up to 3 requests
//     per visit versus 1 for a quantum=1 strategy, so throughput ratio
//     converges to the quantum ratio over many rounds.
//   - No strategy can serve more than its own quantum before every other
//     active strategy gets a turn -- this is the concrete, provable
//     no-starvation bound: worst-case wait for any active strategy is
//     bounded by the sum of every *other* active strategy's quantum
//     (finite, computable from config, independent of wall-clock time).
//   - When a strategy's queue drains mid-visit, any leftover deficit is
//     discarded (reset to 0) rather than carried to its next visit. This is
//     a deliberate, documented variant of classic DRR (which would normally
//     carry it): carrying deficit across idle periods would let a strategy
//     that stops submitting for a while "bank" credit and later burst-drain
//     ahead of everyone once it resumes, which is exactly the kind of
//     starvation-of-others behavior this phase is required to rule out.
//   - The cursor position, per-slot deficit, and per-slot queue contents are
//     the *entire* state next() consults -- no timestamp, real or synthetic,
//     participates in the arbitration decision itself, satisfying "do not
//     use wall-clock ordering for deterministic decisions" by construction,
//     not by discipline.

namespace oms {

using StrategyId = std::uint32_t;

struct StrategyConfig final {
    std::uint32_t quantum{1};  // must be >=1; register_strategy() rejects 0.
};

// What one arbitrated dequeue hands the caller to actually route. Wraps
// rate_limiter.hpp's PendingRequest (kind/ref/symbol/volume/price, reused
// unchanged) with the two things Phase H adds: which strategy it belongs to
// and which venue it targets -- explicit per-request identity, matching
// Phase F's "no implicit venue ambiguity" precedent, now for strategies too.
// `side` is carried separately because exec::Order/Oms::create() do not
// model Side at all (verified: Oms::create()'s signature has no Side
// parameter) -- it exists here solely for SelfTradeTracker's use below, not
// passed through to ShardedOms.
struct ArbitratedRequest final {
    StrategyId strategy{0};
    VenueId venue{0};
    exec::Side side{exec::Side::buy};
    PendingRequest request{};
};

// Not internally synchronized -- one logical owner (whichever thread drives
// arbitration for a given scope), the same discipline Phase F/G's per-venue
// objects use. A single instance is meant to arbitrate across every venue a
// strategy might target: arbitration only decides *order*, so it does not
// need per-venue isolation the way rate limiting does (a saturated venue's
// requests simply come back `rejected`/`deferred` from the unchanged Phase G
// routing call, same as if arbitration didn't exist).
class StrategyScheduler final {
public:
    StrategyScheduler(std::size_t max_strategies, std::size_t queue_capacity_per_strategy)
        : max_strategies_(max_strategies), queue_capacity_(queue_capacity_per_strategy) {
        slots_ = std::make_unique<Slot[]>(max_strategies_);
        for (std::size_t i = 0; i < max_strategies_; ++i)
            slots_[i].queue = std::make_unique<ArbitratedRequest[]>(queue_capacity_);
    }

    // Explicit ownership (Part "Explicit StrategyId ownership"): a strategy
    // must be registered before it can enqueue -- no implicit registration
    // on first use. Fails closed (false) on a duplicate id, an out-of-range
    // slot count, or quantum==0 (which would silently starve the strategy
    // forever, exactly the bug class this method exists to prevent by
    // construction).
    [[nodiscard]] bool register_strategy(StrategyId id, StrategyConfig config) noexcept {
        if (config.quantum == 0) return false;
        if (find_slot(id) != nullptr) return false;
        for (std::size_t i = 0; i < max_strategies_; ++i) {
            if (!slots_[i].registered) {
                slots_[i].registered = true;
                slots_[i].id = id;
                slots_[i].quantum = config.quantum;
                slots_[i].deficit = 0;
                slots_[i].head = 0;
                slots_[i].count = 0;
                return true;
            }
        }
        return false;  // no free slot -- fail closed rather than growing max_strategies_
    }

    // Enqueue: fails closed if unregistered or this strategy's own bounded
    // queue is full ("no unbounded allocation" -- the queue never grows
    // past queue_capacity_, and queue-full is distinctly observable via
    // queue_full_rejections(id), same convention as VenueRateLimiter).
    [[nodiscard]] bool enqueue(StrategyId id, VenueId venue, exec::Side side, const PendingRequest& request) noexcept {
        auto* slot = find_slot(id);
        if (slot == nullptr) return false;
        if (slot->count >= queue_capacity_) { ++slot->queue_full_rejections; return false; }
        slot->queue[(slot->head + slot->count) % queue_capacity_] = ArbitratedRequest{id, venue, side, request};
        ++slot->count;
        slot->high_water = std::max(slot->high_water, slot->count);
        return true;
    }

    // Deterministic DRR dequeue -- see the file header for the algorithm.
    // Returns false only when no registered strategy has a non-empty queue.
    [[nodiscard]] bool next(ArbitratedRequest& out) noexcept {
        if (active_count() == 0) return false;
        for (std::size_t scans = 0; scans < max_strategies_; ++scans) {
            auto& slot = slots_[cursor_];
            if (!slot.registered || slot.count == 0) {
                slot.deficit = 0;
                advance();
                continue;
            }
            if (visit_deficit_ <= 0) {
                visit_deficit_ = static_cast<std::int64_t>(slot.quantum);
                slot.deficit = 0;  // fresh visit's credit is tracked in visit_deficit_, not carried state
            }
            out = slot.queue[slot.head];
            slot.head = (slot.head + 1) % queue_capacity_;
            --slot.count;
            --visit_deficit_;
            ++slot.admitted;
            if (slot.count == 0 || visit_deficit_ <= 0) {
                visit_deficit_ = 0;  // drained or visit budget spent: no leftover carried (see file header)
                advance();
            }
            return true;
        }
        return false;
    }

    [[nodiscard]] std::size_t queue_depth(StrategyId id) const noexcept {
        const auto* slot = find_slot(id);
        return slot ? slot->count : 0;
    }
    [[nodiscard]] std::size_t queue_high_water(StrategyId id) const noexcept {
        const auto* slot = find_slot(id);
        return slot ? slot->high_water : 0;
    }
    [[nodiscard]] std::size_t queue_full_rejections(StrategyId id) const noexcept {
        const auto* slot = find_slot(id);
        return slot ? slot->queue_full_rejections : 0;
    }
    [[nodiscard]] std::uint64_t admitted_count(StrategyId id) const noexcept {
        const auto* slot = find_slot(id);
        return slot ? slot->admitted : 0;
    }

private:
    struct Slot final {
        bool registered{false};
        StrategyId id{0};
        std::uint32_t quantum{1};
        std::int64_t deficit{0};  // unused across visits by design (see next()); kept for clarity/diagnostics
        std::unique_ptr<ArbitratedRequest[]> queue;
        std::size_t head{0}, count{0}, high_water{0};
        std::uint64_t admitted{0}, queue_full_rejections{0};
    };

    [[nodiscard]] std::size_t active_count() const noexcept {
        std::size_t n = 0;
        for (std::size_t i = 0; i < max_strategies_; ++i)
            if (slots_[i].registered && slots_[i].count > 0) ++n;
        return n;
    }
    void advance() noexcept {
        cursor_ = (cursor_ + 1) % max_strategies_;
        visit_deficit_ = 0;
    }
    [[nodiscard]] Slot* find_slot(StrategyId id) noexcept {
        for (std::size_t i = 0; i < max_strategies_; ++i)
            if (slots_[i].registered && slots_[i].id == id) return &slots_[i];
        return nullptr;
    }
    [[nodiscard]] const Slot* find_slot(StrategyId id) const noexcept {
        for (std::size_t i = 0; i < max_strategies_; ++i)
            if (slots_[i].registered && slots_[i].id == id) return &slots_[i];
        return nullptr;
    }

    std::size_t max_strategies_;
    std::size_t queue_capacity_;
    std::unique_ptr<Slot[]> slots_;
    std::size_t cursor_{0};
    std::int64_t visit_deficit_{0};
};

// ---- self-trade prevention interaction (Part "self-trade prevention") -----
//
// Oms/ShardedOms's own create() path does not model Side at all (verified:
// its signature carries no Side parameter), and ShardedOms's shared
// standing_approval() (sharded_oms.hpp) calls RiskEngine::check() with a
// fixed dummy Request whose hedging=false makes self_trade_ok() a permanent
// no-op -- self-trade prevention is a real, existing, tested pure function
// (risk::self_trade_ok(), Phase 6, completely unmodified here) that nothing
// in this codebase actually calls with strategy-aware, non-default
// arguments yet. This phase is the first layer with real StrategyId
// identity in front of order submission, so it is the correct place to
// finally exercise it for real -- not by changing RiskEngine or Oms, but by
// calling the existing pure function here, before routing.
//
// Position tracking is intentionally the same kind of *projected* estimate
// RiskEngine::check()'s own current_net/projected_net fields already are
// (Part 5.9-adjacent): updated optimistically at admission time from
// requested volume, not from confirmed fills (this layer has no fill
// feedback loop) -- an approximation, honestly documented, not a new
// portfolio ledger (Pms/Portfolio's job, out of scope, same boundary
// venue_reconciliation.hpp already draws).
class SelfTradeTracker final {
public:
    explicit SelfTradeTracker(bool allow_cross_strategy_opposition, std::size_t capacity = 256) noexcept
        : allow_cross_strategy_opposition_(allow_cross_strategy_opposition),
          entries_(std::make_unique<Entry[]>(capacity)), capacity_(capacity) {}

    // true = OK to admit (and this call already records the position
    // update); false = blocked by self-trade prevention, caller must not
    // route this request and must not call record-on-block itself.
    [[nodiscard]] bool check_and_apply(VenueId venue, std::uint32_t symbol, StrategyId strategy, exec::Side side,
                                       std::int64_t volume) noexcept {
        auto* entry = find_or_create(venue, symbol);
        if (entry == nullptr) return true;  // capacity exhausted: fail open on *tracking*, not on risk --
                                             // documented limitation, see Phase H report.
        const bool ok = risk::self_trade_ok(/*hedging=*/true, allow_cross_strategy_opposition_, entry->owner,
                                            strategy, entry->net_volume, side);
        if (!ok) return false;
        entry->net_volume += (side == exec::Side::buy) ? volume : -volume;
        entry->owner = strategy;
        return true;
    }

private:
    struct Entry final {
        bool used{false};
        VenueId venue{0};
        std::uint32_t symbol{0};
        StrategyId owner{0};
        std::int64_t net_volume{0};
    };

    [[nodiscard]] Entry* find_or_create(VenueId venue, std::uint32_t symbol) noexcept {
        Entry* free_slot = nullptr;
        for (std::size_t i = 0; i < capacity_; ++i) {
            auto& e = entries_[i];
            if (e.used && e.venue == venue && e.symbol == symbol) return &e;
            if (!e.used && free_slot == nullptr) free_slot = &e;
        }
        if (free_slot != nullptr) { free_slot->used = true; free_slot->venue = venue; free_slot->symbol = symbol; }
        return free_slot;
    }

    bool allow_cross_strategy_opposition_;
    std::unique_ptr<Entry[]> entries_;
    std::size_t capacity_;
};

}  // namespace oms
