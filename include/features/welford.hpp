#pragma once

#include <cmath>
#include <cstdint>

// Phase 3 — Welford online moments.
//
// Part 18 Phase 3 requires stability over 10^8 updates. The naive
// sum-of-squares estimator loses catastrophically there: for values with a
// large mean and small variance it subtracts two nearly equal large numbers and
// the result can go negative. Welford's recurrence updates around the running
// mean instead, so the accumulated quantity stays O(variance) rather than
// O(mean^2 * n).
//
// Non-finite inputs are rejected at the door rather than poisoning the
// accumulator: once a NaN enters mean_ every subsequent reading is NaN, and the
// feature engine's guard downstream could only report the damage, not undo it.

namespace features {

class Welford final {
public:
    void update(double value) noexcept {
        if (!std::isfinite(value)) { ++rejected_; return; }
        ++count_;
        const double delta = value - mean_;
        mean_ += delta / static_cast<double>(count_);
        const double delta2 = value - mean_;   // deliberately re-read after the mean moves
        m2_ += delta * delta2;
    }

    void reset() noexcept { count_ = 0; mean_ = 0.0; m2_ = 0.0; rejected_ = 0; }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }
    [[nodiscard]] double mean() const noexcept { return count_ > 0 ? mean_ : 0.0; }

    // Sample variance (n-1). Undefined for n < 2, reported as 0.
    [[nodiscard]] double variance() const noexcept {
        if (count_ < 2) return 0.0;
        const double value = m2_ / static_cast<double>(count_ - 1);
        return value > 0.0 ? value : 0.0;   // clamp the -0.0 / tiny-negative edge
    }

    [[nodiscard]] double stddev() const noexcept { return std::sqrt(variance()); }

private:
    std::uint64_t count_{0};
    std::uint64_t rejected_{0};
    double mean_{0.0};
    double m2_{0.0};
};

}  // namespace features
