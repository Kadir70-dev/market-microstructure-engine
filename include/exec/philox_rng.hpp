#pragma once

#include <array>
#include <cstdint>

// Phase 5 — Philox-4x32-10 counter-based PRNG.
//
// Architecture Part 9.1: "The simulator's probabilistic models use a
// counter-based PRNG (Philox-4x32-10) keyed on (run_seed, corr_id, stage_id).
// Every draw is a pure function of the event stream — no sequential state,
// order-independent, trivially reproducible."
//
// Counter-based is the load-bearing property, not an implementation detail. A
// sequential generator makes every draw depend on how many draws came before,
// so adding one rejection check anywhere upstream reshuffles every subsequent
// random outcome in the run. Philox indexes instead of iterates: the draw for a
// given (corr_id, stage_id) is the same value no matter what else the simulator
// did, which is what makes a fill reproducible when the surrounding code changes.
//
// Bit-exact reference implementation (Random123). All arithmetic is unsigned
// 32-bit with defined wraparound; no floating point participates in the state.

namespace exec {

// Stage identifiers keep independent models from sharing a draw. Two models
// sampling the same (run_seed, corr_id) must not receive correlated values, so
// the stage is part of the key material rather than a sequence offset.
enum class RngStage : std::uint32_t {
    latency = 1,
    slippage = 2,
    rejection = 3,
    partial_fill = 4,
    queue_share = 5,
    requote = 6
};

class Philox4x32 final {
public:
    static constexpr std::uint32_t rounds = 10;
    static constexpr std::uint32_t multiplier_0 = 0xD2511F53U;
    static constexpr std::uint32_t multiplier_1 = 0xCD9E8D57U;
    static constexpr std::uint32_t weyl_0 = 0x9E3779B9U;   // golden ratio
    static constexpr std::uint32_t weyl_1 = 0xBB67AE85U;   // sqrt(3) - 1

    // Raw block function, exposed so the implementation can be checked against
    // the published Random123 known-answer vectors. A PRNG that is deterministic
    // but not actually Philox would pass every reproducibility test in this
    // phase while silently producing the wrong distribution.
    [[nodiscard]] static std::array<std::uint32_t, 4> generate_raw(
        std::array<std::uint32_t, 4> counter, std::array<std::uint32_t, 2> key) noexcept {
        for (std::uint32_t round = 0; round < rounds; ++round) {
            counter = round_function(counter, key);
            if (round + 1 < rounds) {
                key[0] += weyl_0;
                key[1] += weyl_1;
            }
        }
        return counter;
    }

    // Pure function: identical inputs always yield identical output, with no
    // object state involved anywhere.
    [[nodiscard]] static std::array<std::uint32_t, 4> generate(
        std::uint64_t run_seed, std::uint64_t corr_id, std::uint32_t stage_id) noexcept {
        return generate_raw({static_cast<std::uint32_t>(corr_id & 0xFFFFFFFFU),
                             static_cast<std::uint32_t>(corr_id >> 32),
                             stage_id, 0U},
                            {static_cast<std::uint32_t>(run_seed & 0xFFFFFFFFU),
                             static_cast<std::uint32_t>(run_seed >> 32)});
    }

    // ---- convenience draws ------------------------------------------------

    [[nodiscard]] static std::uint64_t next_u64(std::uint64_t run_seed, std::uint64_t corr_id,
                                                RngStage stage) noexcept {
        const auto words = generate(run_seed, corr_id, static_cast<std::uint32_t>(stage));
        return (static_cast<std::uint64_t>(words[1]) << 32) | words[0];
    }

    // Uniform in [0, 1). Uses 53 bits, the exact mantissa width of a double, so
    // the mapping is lossless and cannot round to 1.0.
    [[nodiscard]] static double next_unit(std::uint64_t run_seed, std::uint64_t corr_id,
                                          RngStage stage) noexcept {
        const auto bits = next_u64(run_seed, corr_id, stage) >> 11;   // keep 53
        return static_cast<double>(bits) * (1.0 / 9007199254740992.0);  // 2^53
    }

    // Uniform integer in [low, high]. Rejection-free by construction: a modulo
    // would bias the tail, and a rejection loop would reintroduce the sequential
    // dependence the whole design exists to avoid.
    [[nodiscard]] static std::uint64_t next_range(std::uint64_t run_seed, std::uint64_t corr_id,
                                                  RngStage stage, std::uint64_t low,
                                                  std::uint64_t high) noexcept {
        if (high <= low) return low;
        const auto span = high - low + 1;
        // 64x64 -> high 64 bits of the product: Lemire's multiply-shift, unbiased
        // enough for model sampling and entirely branch-free.
        const auto draw = next_u64(run_seed, corr_id, stage);
        return low + multiply_high(draw, span);
    }

    [[nodiscard]] static bool bernoulli(std::uint64_t run_seed, std::uint64_t corr_id,
                                        RngStage stage, double probability) noexcept {
        if (probability <= 0.0) return false;
        if (probability >= 1.0) return true;
        return next_unit(run_seed, corr_id, stage) < probability;
    }

private:
    static void mul_hi_lo(std::uint32_t a, std::uint32_t b,
                          std::uint32_t& hi, std::uint32_t& lo) noexcept {
        const auto product = static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b);
        hi = static_cast<std::uint32_t>(product >> 32);
        lo = static_cast<std::uint32_t>(product & 0xFFFFFFFFU);
    }

    [[nodiscard]] static std::array<std::uint32_t, 4> round_function(
        const std::array<std::uint32_t, 4>& counter,
        const std::array<std::uint32_t, 2>& key) noexcept {
        std::uint32_t hi0 = 0, lo0 = 0, hi1 = 0, lo1 = 0;
        mul_hi_lo(multiplier_0, counter[0], hi0, lo0);
        mul_hi_lo(multiplier_1, counter[2], hi1, lo1);
        return {static_cast<std::uint32_t>(hi1 ^ counter[1] ^ key[0]), lo1,
                static_cast<std::uint32_t>(hi0 ^ counter[3] ^ key[1]), lo0};
    }

    [[nodiscard]] static std::uint64_t multiply_high(std::uint64_t a, std::uint64_t b) noexcept {
        const std::uint64_t a_lo = a & 0xFFFFFFFFULL, a_hi = a >> 32;
        const std::uint64_t b_lo = b & 0xFFFFFFFFULL, b_hi = b >> 32;
        const std::uint64_t ll = a_lo * b_lo;
        const std::uint64_t lh = a_lo * b_hi;
        const std::uint64_t hl = a_hi * b_lo;
        const std::uint64_t hh = a_hi * b_hi;
        const std::uint64_t carry = ((ll >> 32) + (lh & 0xFFFFFFFFULL) + (hl & 0xFFFFFFFFULL)) >> 32;
        return hh + (lh >> 32) + (hl >> 32) + carry;
    }
};

}  // namespace exec
