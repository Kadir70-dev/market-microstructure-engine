#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <vector>

#include "exec/philox_rng.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}
using exec::Philox4x32;
using exec::RngStage;

bool equal(const std::array<std::uint32_t, 4>& a, const std::array<std::uint32_t, 4>& b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}
}

int main() {
    // ---- known-answer vectors, Random123 philox4x32-10 ---------------------
    // These are the published reference values. They are what distinguishes a
    // correct Philox from an arbitrary deterministic hash.
    {
        const auto zero = Philox4x32::generate_raw({0, 0, 0, 0}, {0, 0});
        check(equal(zero, {0x6627e8d5U, 0xe169c58dU, 0xbc57ac4cU, 0x9b00dbd8U}),
              "philox_kat_zero_vector");

        const auto ones = Philox4x32::generate_raw(
            {0xffffffffU, 0xffffffffU, 0xffffffffU, 0xffffffffU}, {0xffffffffU, 0xffffffffU});
        check(equal(ones, {0x408f276dU, 0x41c83b0eU, 0xa20bc7c6U, 0x6d5451fdU}),
              "philox_kat_all_ones_vector");

        const auto pi = Philox4x32::generate_raw(
            {0x243f6a88U, 0x85a308d3U, 0x13198a2eU, 0x03707344U}, {0xa4093822U, 0x299f31d0U});
        check(equal(pi, {0xd16cfe09U, 0x94fdccebU, 0x5001e420U, 0x24126ea1U}),
              "philox_kat_pi_vector");
    }

    // ---- purity: same inputs, same output, always --------------------------
    {
        const auto a = Philox4x32::generate(42, 1234, 7);
        const auto b = Philox4x32::generate(42, 1234, 7);
        check(equal(a, b), "philox_pure_function");
    }

    // ---- order independence ------------------------------------------------
    // The property that makes the simulator robust: drawing for corr_id 500
    // first must not change what corr_id 100 receives. A sequential generator
    // fails this, and with it every fill downstream of an added draw.
    {
        std::vector<std::uint64_t> forward, backward;
        for (std::uint64_t i = 0; i < 256; ++i)
            forward.push_back(Philox4x32::next_u64(9, i, RngStage::latency));
        for (std::uint64_t i = 256; i-- > 0;)
            backward.push_back(Philox4x32::next_u64(9, i, RngStage::latency));
        bool same = true;
        for (std::size_t i = 0; i < forward.size(); ++i)
            if (forward[i] != backward[backward.size() - 1 - i]) same = false;
        check(same, "philox_order_independent");
    }

    // ---- key separation ----------------------------------------------------
    {
        const auto latency = Philox4x32::next_u64(1, 1, RngStage::latency);
        const auto slippage = Philox4x32::next_u64(1, 1, RngStage::slippage);
        check(latency != slippage, "philox_stage_separates_streams");
        check(Philox4x32::next_u64(1, 1, RngStage::latency) !=
              Philox4x32::next_u64(2, 1, RngStage::latency), "philox_seed_separates_streams");
        check(Philox4x32::next_u64(1, 1, RngStage::latency) !=
              Philox4x32::next_u64(1, 2, RngStage::latency), "philox_corr_separates_streams");
    }

    // ---- unit interval bounds ----------------------------------------------
    {
        bool in_range = true, saw_low = false, saw_high = false;
        for (std::uint64_t i = 0; i < 100'000; ++i) {
            const auto value = Philox4x32::next_unit(5, i, RngStage::rejection);
            if (!(value >= 0.0 && value < 1.0)) in_range = false;
            if (value < 0.1) saw_low = true;
            if (value > 0.9) saw_high = true;
        }
        check(in_range, "philox_unit_in_half_open_interval");
        check(saw_low && saw_high, "philox_unit_spans_range");
    }

    // ---- range bounds and rough uniformity ---------------------------------
    {
        std::map<std::uint64_t, int> counts;
        bool in_range = true;
        for (std::uint64_t i = 0; i < 100'000; ++i) {
            const auto value = Philox4x32::next_range(3, i, RngStage::partial_fill, 10, 19);
            if (value < 10 || value > 19) in_range = false;
            ++counts[value];
        }
        check(in_range, "philox_range_respects_bounds");
        check(counts.size() == 10, "philox_range_covers_all_buckets");
        bool balanced = true;
        for (const auto& entry : counts)
            if (entry.second < 8'000 || entry.second > 12'000) balanced = false;
        check(balanced, "philox_range_roughly_uniform");
        check(Philox4x32::next_range(3, 1, RngStage::partial_fill, 7, 7) == 7,
              "philox_range_degenerate");
    }

    // ---- bernoulli edges ---------------------------------------------------
    {
        check(!Philox4x32::bernoulli(1, 1, RngStage::requote, 0.0), "philox_bernoulli_zero");
        check(Philox4x32::bernoulli(1, 1, RngStage::requote, 1.0), "philox_bernoulli_one");
        int hits = 0;
        for (std::uint64_t i = 0; i < 100'000; ++i)
            if (Philox4x32::bernoulli(4, i, RngStage::requote, 0.25)) ++hits;
        check(hits > 23'000 && hits < 27'000, "philox_bernoulli_rate");
    }

    return failures == 0 ? 0 : 1;
}
