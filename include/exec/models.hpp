#pragma once

#include <cstdint>

#include "exec/exec_types.hpp"
#include "exec/philox_rng.hpp"

// Phase 5 — regime bucketing and the conditional latency / slippage / rejection
// models (Architecture Part 9.3).
//
// ⚠ CALIBRATION STATUS: NOT CALIBRATED.
// Part 9.3 requires these distributions to be "calibrated per bucket from Phase 9
// demo observation". Phase 9 has not run, so no observed data exists. Every
// number below is a DECLARED PLACEHOLDER chosen to be structurally sane, not an
// empirical estimate. The simulator's mechanics are correct; its magnitudes are
// not evidence. Results are inadmissible for promotion until calibration exists.
//
// Why conditional rather than independent draws: broker latency, slippage and
// rejects all spike together during bursts. Sampling them independently
// systematically flatters the backtest on exactly the trades that matter most.

namespace exec {

// Three levels per axis: spread percentile x event rate x realised volatility.
inline constexpr std::uint32_t regime_levels = 3;
inline constexpr std::uint32_t regime_bucket_count = regime_levels * regime_levels * regime_levels;

struct RegimeObservation final {
    std::int64_t spread_ticks{0};
    std::int64_t median_spread_ticks{1};
    double events_per_second{0.0};
    double realised_vol{0.0};
};

struct RegimeBucket final {
    std::uint32_t spread_level{0};
    std::uint32_t rate_level{0};
    std::uint32_t vol_level{0};

    [[nodiscard]] constexpr std::uint32_t index() const noexcept {
        return (spread_level * regime_levels + rate_level) * regime_levels + vol_level;
    }
    // Level 2 on any axis is the stressed end; the adverse bucket is all-2.
    [[nodiscard]] constexpr bool adverse() const noexcept {
        return spread_level == 2 && rate_level == 2 && vol_level == 2;
    }
};

[[nodiscard]] constexpr std::uint32_t level_of(double value, double low, double high) noexcept {
    if (value < low) return 0;
    if (value < high) return 1;
    return 2;
}

[[nodiscard]] inline RegimeBucket classify(const RegimeObservation& observation) noexcept {
    const double median = observation.median_spread_ticks > 0
        ? static_cast<double>(observation.median_spread_ticks) : 1.0;
    const double spread_ratio = static_cast<double>(observation.spread_ticks) / median;
    return RegimeBucket{level_of(spread_ratio, 1.25, 2.0),
                        level_of(observation.events_per_second, 5.0, 50.0),
                        level_of(observation.realised_vol, 1e-5, 5e-5)};
}

// Part 9.4. Pessimistic is the only promotion-grade mode.
struct ModeProfile final {
    std::uint64_t latency_base_ns{0};
    std::uint64_t latency_span_ns{0};
    double slippage_multiplier{0.0};
    double reject_probability{0.0};
    // Queue placement: 0.0 = front of queue, 0.5 = mid, 1.0 = back.
    double queue_placement{0.0};

    [[nodiscard]] static constexpr ModeProfile for_mode(SimulationMode mode) noexcept {
        switch (mode) {
            // Optimistic: p10 latency, no slippage, no rejects, front of queue.
            case SimulationMode::optimistic:
                return ModeProfile{200'000ULL, 100'000ULL, 0.0, 0.0, 0.0};
            // Base: p50, expected slippage, observed reject rate, mid queue.
            case SimulationMode::base:
                return ModeProfile{2'000'000ULL, 3'000'000ULL, 1.0, 0.005, 0.5};
            // Pessimistic: p99 latency, 2x slippage, 2x rejects, back of queue.
            case SimulationMode::pessimistic:
                return ModeProfile{30'000'000ULL, 20'000'000ULL, 2.0, 0.010, 1.0};
        }
        return ModeProfile{};
    }
};

// Regime multiplies the mode profile. Adverse conditions stretch latency and
// slippage together, which is the correlation Part 9.3 exists to preserve.
[[nodiscard]] constexpr double regime_stress(const RegimeBucket& bucket) noexcept {
    const auto level_sum = bucket.spread_level + bucket.rate_level + bucket.vol_level;
    return 1.0 + 0.5 * static_cast<double>(level_sum);   // 1.0 .. 4.0
}

class LatencyModel final {
public:
    // Part 9.2: sampled at t_send + lambda, never t_decision. The caller stamps
    // t_send; this returns lambda only, so there is no way to accidentally
    // sample against the decision time.
    [[nodiscard]] static std::uint64_t sample_ns(std::uint64_t run_seed, std::uint64_t corr_id,
                                                 SimulationMode mode,
                                                 const RegimeBucket& bucket) noexcept {
        const auto profile = ModeProfile::for_mode(mode);
        // Pessimistic mode uses the adverse bucket unconditionally (Part 9.3).
        const auto stress = (mode == SimulationMode::pessimistic)
            ? regime_stress(RegimeBucket{2, 2, 2}) : regime_stress(bucket);
        const auto span = static_cast<std::uint64_t>(
            static_cast<double>(profile.latency_span_ns) * stress);
        const auto base = static_cast<std::uint64_t>(
            static_cast<double>(profile.latency_base_ns) * stress);
        if (span == 0) return base;
        return base + Philox4x32::next_range(run_seed, corr_id, RngStage::latency, 0, span);
    }
};

class SlippageModel final {
public:
    // Additional adverse ticks paid by a marketable order. Always signed against
    // the trader: a model that can return favourable slippage would make the
    // pessimistic mode non-conservative and break the Part 9.4 ordering
    // invariant.
    [[nodiscard]] static std::int64_t sample_ticks(std::uint64_t run_seed, std::uint64_t corr_id,
                                                   SimulationMode mode,
                                                   const RegimeBucket& bucket,
                                                   std::int64_t spread_ticks) noexcept {
        const auto profile = ModeProfile::for_mode(mode);
        if (profile.slippage_multiplier <= 0.0) return 0;
        auto stress = (mode == SimulationMode::pessimistic)
            ? regime_stress(RegimeBucket{2, 2, 2}) : regime_stress(bucket);
        // Mode multiplier and regime stress previously multiplied without bound:
        // pessimistic (2x) x adverse stress (4x) = 8x spread of slippage on a
        // 0.01-lot EURUSD order, which is not a plausible fill and made every
        // entry start ~80 ticks underwater — guaranteeing an immediate stop-out
        // regardless of signal.
        //
        // The bound is applied to the REGIME term, not to the product. Capping
        // the product collapsed base and pessimistic onto the same value in the
        // adverse bucket, which inverts the Part 9.4 ordering
        // PnL(opt) >= PnL(base) >= PnL(pess) — the one property that makes the
        // pessimistic mode the promotion-grade one. The mode multiplier must
        // stay the term that carries the ordering; only the regime amplifier is
        // clamped. Still NOT_CALIBRATED, but bounded to 4x spread worst case
        // rather than 8x.
        constexpr double max_regime_stress = 2.0;
        if (stress > max_regime_stress) stress = max_regime_stress;
        const double scale = profile.slippage_multiplier * stress;
        const auto cap = static_cast<std::uint64_t>(
            static_cast<double>(spread_ticks > 0 ? spread_ticks : 1) * scale);
        if (cap == 0) return 0;
        return static_cast<std::int64_t>(
            Philox4x32::next_range(run_seed, corr_id, RngStage::slippage, 0, cap));
    }
};

class RejectionModel final {
public:
    [[nodiscard]] static bool rejects(std::uint64_t run_seed, std::uint64_t corr_id,
                                      SimulationMode mode, const RegimeBucket& bucket) noexcept {
        const auto profile = ModeProfile::for_mode(mode);
        if (profile.reject_probability <= 0.0) return false;
        const auto stress = (mode == SimulationMode::pessimistic)
            ? regime_stress(RegimeBucket{2, 2, 2}) : regime_stress(bucket);
        const double probability = profile.reject_probability * stress;
        return Philox4x32::bernoulli(run_seed, corr_id, RngStage::rejection,
                                     probability > 1.0 ? 1.0 : probability);
    }
};

class CommissionModel final {
public:
    // Integer, round-half-up, so commission is bit-reproducible. Charged on
    // filled volume, per Part 9.2's exact-SymbolMeta requirement.
    [[nodiscard]] static std::int64_t charge_minor(const SymbolSpec& spec,
                                                   std::int64_t filled_volume) noexcept {
        if (filled_volume <= 0 || spec.commission_per_lot_minor == 0) return 0;
        if (spec.volume_step <= 0) return 0;
        const auto numerator = filled_volume * spec.commission_per_lot_minor;
        const auto denominator = spec.contract_size > 0 ? spec.contract_size : 1;
        return (numerator + denominator / 2) / denominator;
    }
};

}  // namespace exec
