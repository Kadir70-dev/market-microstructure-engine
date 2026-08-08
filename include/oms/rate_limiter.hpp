#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>

#include "exec/exec_types.hpp"

// Phase G -- deterministic, low-latency per-exchange rate limiting and
// session constraints, layered on top of Phase F's multi-venue architecture
// (include/oms/multi_venue.hpp). Nothing in Phase B-F is modified by this
// file: VenueRateLimiter and SessionConstraints are new, independent types;
// multi_venue.hpp wires them into VenueConnection as new, defaulted-to-
// permissive members plus new route_*() methods, so every existing Phase F
// call site (VenueRegistry::submit_create, direct ShardedOms access) keeps
// working exactly as before.
//
// ---- token bucket, using double instead of int64 ticks -------------------
//
// exec_types.hpp is explicit that this codebase keeps floating point out of
// *accounting* (Part 5.9) because a PnL built from doubles can't be compared
// bit-for-bit *across different runs/machines*. That is not the determinism
// this phase needs: Part 6 asks for the same captured (event, timestamp)
// sequence, replayed through the same binary, to produce the same decision
// sequence -- and CMakeLists.txt already forces /fp:strict (MSVC) /
// -ffp-contract=off -fno-fast-math (GCC/Clang) project-wide specifically so
// IEEE-754 double arithmetic *is* reproducible for identical instruction
// sequences on identical inputs. Given that guarantee is already paid for at
// the project level, double keeps the refill math simple and correct
// (in particular, correct handling of "huge forward clock jump" is just
// std::min(capacity, ...) instead of a hand-rolled fixed-point overflow
// guard) at no real cost to the determinism this phase actually promises.

namespace oms {

enum class RateLimitKind : std::uint8_t { order, cancel, replace };
enum class Decision : std::uint8_t { admitted, deferred, rejected };
enum class QueuePolicy : std::uint8_t { defer, reject_immediately };

class TokenBucket final {
public:
    void configure(double rate_per_sec, double burst_capacity, std::uint64_t now_ns) noexcept {
        rate_per_sec_ = rate_per_sec;
        capacity_ = burst_capacity;
        tokens_ = burst_capacity;  // starts full: burst is immediately available, matching a
                                   // freshly (re)configured limiter having done no work yet.
        last_refill_ns_ = now_ns;
    }

    // Clock edge cases: now_ns <= the last observed time (a stationary or
    // backwards clock) grants zero tokens rather than a negative/undefined
    // elapsed -- fail-safe, never manufactures free capacity from a clock
    // that didn't advance. A huge forward jump needs no special case: the
    // std::min(capacity_, ...) clamp below saturates it correctly, and
    // double arithmetic here cannot wrap the way subtracting two
    // std::uint64_t timestamps could.
    void refill(std::uint64_t now_ns) noexcept {
        if (now_ns <= last_refill_ns_) return;
        const double elapsed_s = static_cast<double>(now_ns - last_refill_ns_) / 1e9;
        tokens_ = std::min(capacity_, tokens_ + elapsed_s * rate_per_sec_);
        last_refill_ns_ = now_ns;
    }

    [[nodiscard]] bool try_consume(std::uint64_t now_ns) noexcept {
        refill(now_ns);
        if (tokens_ < 1.0) return false;
        tokens_ -= 1.0;
        return true;
    }

    [[nodiscard]] double tokens(std::uint64_t now_ns) noexcept {
        refill(now_ns);
        return tokens_;
    }

private:
    double rate_per_sec_{0.0};
    double capacity_{0.0};
    double tokens_{0.0};
    std::uint64_t last_refill_ns_{0};
};

// ---- session constraints (Part 5) ------------------------------------------

enum class SessionState : std::uint8_t { open, closed };

// Reconnect/reconciliation state is deliberately NOT duplicated here: it
// already exists per-venue as VenueConnection::state() (Phase F's
// ConnectionState). Session constraints are the *market/operator* dimension
// (is the exchange's session open, has an operator halted trading, has
// order-entry been explicitly disabled); VenueConnection::route_*() (in
// multi_venue.hpp) combines both this struct's verdict and its own
// ConnectionState before ever reaching the rate limiter, so "no new
// exposure outside an allowed session" and "no new exposure during
// recovery/reconciliation" are both enforced, from two independent sources
// of truth, at the same single choke point.
struct SessionConstraints final {
    SessionState session{SessionState::open};
    bool trading_halted{false};
    bool order_entry_enabled{true};
    // Policy switch for Part 5's "risk-reducing cancel/close behavior must
    // remain possible where policy allows": true (default) means a closed
    // session or a halt does not by itself block a cancel/reduce -- only
    // order_entry_enabled=false (an explicit, total stop) does. Set false to
    // make a halt block cancels too, for venues/policies that want that.
    bool allow_risk_reducing_during_halt{true};

    [[nodiscard]] bool allows_new_exposure() const noexcept {
        return order_entry_enabled && !trading_halted && session == SessionState::open;
    }
    [[nodiscard]] bool allows_risk_reducing() const noexcept {
        if (!order_entry_enabled) return false;
        if (allow_risk_reducing_during_halt) return true;
        return !trading_halted && session == SessionState::open;
    }
};

// ---- per-venue rate limiter (Part 1-4) -------------------------------------

// What a deferred request needs to be replayed later, for whichever of the
// three ShardedOms ops (create/cancel-via-transition/replace) it represents.
// Bounded, preallocated, trivially copyable -- ring-buffer safe, same
// convention as sharded_oms.hpp's own Request.
struct PendingRequest final {
    RateLimitKind kind{RateLimitKind::order};
    exec::BrokerOrderRef ref{};
    std::uint32_t symbol{0};   // order
    std::int64_t volume{0};    // order / replace
    std::int64_t price{0};     // replace
};
static_assert(std::is_trivially_copyable_v<PendingRequest>, "PendingRequest must be ring-buffer safe");

struct VenueRateLimitConfig final {
    double orders_per_sec{1.0e18};     // effectively unlimited by default, so a
    double burst_capacity{1.0e18};     // freshly-constructed VenueConnection (no
    double cancels_per_sec{1.0e18};    // configure_rate_limit() call yet) behaves
    double replaces_per_sec{1.0e18};   // exactly like Phase F's ungated submit_*.
    double cancel_burst_capacity{0.0};   // 0 => defaults to cancels_per_sec (1s worth)
    double replace_burst_capacity{0.0};  // 0 => defaults to replaces_per_sec (1s worth)
    QueuePolicy policy{QueuePolicy::reject_immediately};
    std::size_t pending_queue_capacity{0};
};

// Not internally synchronized: exactly one logical owner per venue is the
// same discipline ShardedOms uses per-shard (Part 4's "separate ShardedOms
// per venue" already gives every venue's routing its own single owner in
// every test/benchmark in this phase) -- "Venue A hitting its limit must not
// throttle Venue B" therefore holds structurally, with no shared state and
// no synchronization primitive between two venues' limiters at all.
class VenueRateLimiter final {
public:
    VenueRateLimiter() noexcept { configure(VenueRateLimitConfig{}, 0); }

    void configure(const VenueRateLimitConfig& config, std::uint64_t now_ns) noexcept {
        config_ = config;
        order_bucket_.configure(config.orders_per_sec, config.burst_capacity, now_ns);
        cancel_bucket_.configure(config.cancels_per_sec,
                                 config.cancel_burst_capacity > 0.0 ? config.cancel_burst_capacity
                                                                    : config.cancels_per_sec,
                                 now_ns);
        replace_bucket_.configure(config.replaces_per_sec,
                                  config.replace_burst_capacity > 0.0 ? config.replace_burst_capacity
                                                                      : config.replaces_per_sec,
                                  now_ns);
        pending_capacity_ = config.pending_queue_capacity;
        pending_ = pending_capacity_ > 0 ? std::make_unique<PendingRequest[]>(pending_capacity_) : nullptr;
        pending_head_ = 0;
        pending_count_ = 0;
        high_water_ = 0;
    }

    // Immediate admission decision for `request` (kind + identity already
    // filled in by the caller). Consumes a token on admission. On rate
    // limit: enqueues (policy == defer, room available -> deferred) or fails
    // closed (policy == reject_immediately, or the queue is full ->
    // rejected). Queue-full is distinguished from "just rate limited" via
    // queue_full_rejections() below -- Part 4's "queue-full must fail closed
    // and be observable".
    [[nodiscard]] Decision admit(const PendingRequest& request, std::uint64_t now_ns) noexcept {
        auto& bucket = bucket_for(request.kind);
        auto& counters = counters_for(request.kind);
        if (bucket.try_consume(now_ns)) { ++counters.admitted; return Decision::admitted; }

        if (config_.policy == QueuePolicy::defer && pending_capacity_ > 0) {
            if (pending_count_ < pending_capacity_) {
                pending_[(pending_head_ + pending_count_) % pending_capacity_] = request;
                ++pending_count_;
                high_water_ = std::max(high_water_, pending_count_);
                ++counters.deferred;
                return Decision::deferred;
            }
            ++queue_full_rejections_;
        }
        ++counters.rejected;
        return Decision::rejected;
    }

    // Pops and returns the queue's front entry if its bucket has a token
    // *right now*. Strict FIFO: does not skip past a front entry that still
    // can't get a token, so create/cancel/replace ordering for one venue is
    // never reshuffled relative to arrival order.
    [[nodiscard]] bool try_drain_one(std::uint64_t now_ns, PendingRequest& out) noexcept {
        if (pending_count_ == 0) return false;
        auto& front = pending_[pending_head_];
        if (!bucket_for(front.kind).try_consume(now_ns)) return false;
        out = front;
        pending_head_ = (pending_head_ + 1) % pending_capacity_;
        --pending_count_;
        ++counters_for(front.kind).admitted;
        return true;
    }

    [[nodiscard]] std::size_t pending_depth() const noexcept { return pending_count_; }
    [[nodiscard]] std::size_t pending_high_water() const noexcept { return high_water_; }
    [[nodiscard]] std::size_t queue_full_rejections() const noexcept { return queue_full_rejections_; }

    struct Counters final { std::uint64_t admitted{0}, deferred{0}, rejected{0}; };
    [[nodiscard]] const Counters& counters(RateLimitKind kind) const noexcept {
        return const_cast<VenueRateLimiter*>(this)->counters_for(kind);
    }

    // Diagnostic only (not on the hot admit()/try_drain_one() path): current
    // token count for `kind`, refilled as of now_ns.
    [[nodiscard]] double tokens(RateLimitKind kind, std::uint64_t now_ns) noexcept {
        return bucket_for(kind).tokens(now_ns);
    }

private:
    [[nodiscard]] TokenBucket& bucket_for(RateLimitKind kind) noexcept {
        switch (kind) {
            case RateLimitKind::order: return order_bucket_;
            case RateLimitKind::cancel: return cancel_bucket_;
            default: return replace_bucket_;
        }
    }
    [[nodiscard]] Counters& counters_for(RateLimitKind kind) noexcept {
        switch (kind) {
            case RateLimitKind::order: return order_counters_;
            case RateLimitKind::cancel: return cancel_counters_;
            default: return replace_counters_;
        }
    }

    VenueRateLimitConfig config_{};
    TokenBucket order_bucket_{}, cancel_bucket_{}, replace_bucket_{};
    Counters order_counters_{}, cancel_counters_{}, replace_counters_{};

    std::unique_ptr<PendingRequest[]> pending_{};
    std::size_t pending_capacity_{0};
    std::size_t pending_head_{0};
    std::size_t pending_count_{0};
    std::size_t high_water_{0};
    std::size_t queue_full_rejections_{0};
};

}  // namespace oms
