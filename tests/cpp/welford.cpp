#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

#include "features/welford.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}

bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }
}

int main() {
    {
        features::Welford w;
        check(w.count() == 0 && w.mean() == 0.0 && w.variance() == 0.0, "welford_empty");
        w.update(5.0);
        check(w.count() == 1 && near(w.mean(), 5.0, 1e-12), "welford_single_mean");
        check(w.variance() == 0.0, "welford_single_variance_undefined_reported_zero");
    }

    {
        // Hand-computed: {2,4,4,4,5,5,7,9} -> mean 5, sample variance 32/7.
        features::Welford w;
        const double data[] = {2, 4, 4, 4, 5, 5, 7, 9};
        for (double value : data) w.update(value);
        check(near(w.mean(), 5.0, 1e-12), "welford_mean_vs_hand_computed");
        check(near(w.variance(), 32.0 / 7.0, 1e-12), "welford_variance_vs_hand_computed");
        check(near(w.stddev(), std::sqrt(32.0 / 7.0), 1e-12), "welford_stddev");
    }

    {
        // Non-finite input must be rejected at the door. One NaN reaching mean_
        // would make every subsequent reading NaN, and no downstream guard can
        // undo that — it can only report it.
        features::Welford w;
        w.update(1.0);
        w.update(std::nan(""));
        w.update(std::numeric_limits<double>::infinity());
        w.update(-std::numeric_limits<double>::infinity());
        w.update(3.0);
        check(w.count() == 2, "welford_rejects_non_finite");
        check(w.rejected() == 3, "welford_counts_rejected");
        check(near(w.mean(), 2.0, 1e-12), "welford_mean_unpoisoned");
        check(std::isfinite(w.variance()), "welford_variance_finite");
    }

    {
        // Part 18 Phase 3: stability over 10^8 updates. A large mean with a
        // small deviation is exactly where the naive sum-of-squares estimator
        // collapses — it subtracts two nearly equal ~10^18 quantities and can
        // return a negative variance. Welford must stay exact here.
        features::Welford w;
        const double base = 1'000'000'000.0;
        constexpr std::uint64_t iterations = 100'000'000ULL;
        for (std::uint64_t i = 0; i < iterations; ++i)
            w.update((i % 2 == 0) ? base + 1.0 : base - 1.0);
        check(w.count() == iterations, "welford_1e8_count");
        check(near(w.mean(), base, 1e-3), "welford_1e8_mean_stable");
        // Alternating +/-1 about the mean: population variance 1, sample
        // variance n/(n-1) -> 1 within float tolerance at this scale.
        check(near(w.variance(), 1.0, 1e-6), "welford_1e8_variance_stable");
        check(w.variance() > 0.0, "welford_1e8_variance_not_negative");
    }

    return failures == 0 ? 0 : 1;
}
