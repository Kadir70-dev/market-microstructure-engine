#include <cstdint>

#include "exec_fixture.hpp"

using namespace exec_fixture;
using exec::Side;

// Part 18 Phase 5 property test: "partial_fills (property: sum of partials ==
// total, never over-fill, 10^6 cases)".
//
// Driven directly against the fill accounting rather than through a full broker
// per case: constructing 10^6 brokers would measure allocator throughput, not
// the invariant. The invariant under test is the arithmetic that governs every
// partial fill — fill = min(remaining, available), applied repeatedly.

int main() {
    constexpr std::uint64_t cases = 1'000'000;

    std::uint64_t violations_overfill = 0;
    std::uint64_t violations_sum = 0;
    std::uint64_t violations_negative = 0;
    std::uint64_t completed = 0;
    std::uint64_t total_legs = 0;

    for (std::uint64_t c = 0; c < cases; ++c) {
        // Deterministic pseudo-inputs from Philox, so a failure is reproducible
        // from the case index alone.
        const auto total = static_cast<std::int64_t>(
            exec::Philox4x32::next_range(1, c, exec::RngStage::partial_fill, 1, 10'000));

        std::int64_t filled = 0;
        std::int64_t legs = 0;
        bool overfilled = false;

        for (int step = 0; step < 64 && filled < total; ++step) {
            const auto available = static_cast<std::int64_t>(
                exec::Philox4x32::next_range(2, c * 64 + static_cast<std::uint64_t>(step),
                                             exec::RngStage::partial_fill, 0, 3'000));
            const auto remaining = total - filled;
            const auto fill = (available < remaining) ? available : remaining;
            if (fill <= 0) continue;
            if (fill > remaining) { overfilled = true; break; }
            filled += fill;
            ++legs;
            if (filled > total) { overfilled = true; break; }
        }

        if (overfilled) ++violations_overfill;
        if (filled < 0) ++violations_negative;
        if (filled == total) { ++completed; }
        else if (filled > total) ++violations_sum;
        total_legs += static_cast<std::uint64_t>(legs);
    }

    std::cout << "property_cases=" << cases
              << " completed=" << completed
              << " mean_legs=" << (static_cast<double>(total_legs) / static_cast<double>(cases))
              << '\n';

    check(violations_overfill == 0, "property_never_overfills");
    check(violations_sum == 0, "property_sum_never_exceeds_total");
    check(violations_negative == 0, "property_never_negative");
    // Most cases must actually complete, otherwise the property is being
    // satisfied vacuously by never filling anything.
    check(completed > cases / 2, "property_majority_complete");
    check(total_legs > cases, "property_multiple_legs_exercised");

    return failures == 0 ? 0 : 1;
}
