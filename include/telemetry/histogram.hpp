#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace telemetry {

template <std::size_t BucketCount = 64>
class Histogram final {
public:
    void record(std::uint64_t value) noexcept {
        buckets_[bucket_for(value)].fetch_add(1, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
        sum_.fetch_add(value, std::memory_order_relaxed);
        update_min(value);
        update_max(value);
    }

    [[nodiscard]] std::uint64_t count() const noexcept {
        return count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t min() const noexcept {
        const auto value = min_.load(std::memory_order_relaxed);
        return value == std::numeric_limits<std::uint64_t>::max() ? 0 : value;
    }
    [[nodiscard]] std::uint64_t max() const noexcept {
        return max_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t percentile(double quantile) const noexcept {
        const auto total = count();
        if (total == 0) return 0;
        if (quantile < 0.0) quantile = 0.0;
        if (quantile > 1.0) quantile = 1.0;
        const auto rank = static_cast<std::uint64_t>(quantile * (total - 1)) + 1;
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < BucketCount; ++i) {
            seen += buckets_[i].load(std::memory_order_relaxed);
            if (seen >= rank) return i == 0 ? 0 : (std::uint64_t{1} << i);
        }
        return max();
    }

private:
    [[nodiscard]] static std::size_t bucket_for(std::uint64_t value) noexcept {
        std::size_t bucket = 0;
        while (value > 1 && bucket + 1 < BucketCount) {
            value >>= 1U;
            ++bucket;
        }
        return bucket;
    }
    void update_min(std::uint64_t value) noexcept {
        auto current = min_.load(std::memory_order_relaxed);
        while (value < current && !min_.compare_exchange_weak(
            current, value, std::memory_order_relaxed)) {}
    }
    void update_max(std::uint64_t value) noexcept {
        auto current = max_.load(std::memory_order_relaxed);
        while (value > current && !max_.compare_exchange_weak(
            current, value, std::memory_order_relaxed)) {}
    }

    std::array<std::atomic<std::uint64_t>, BucketCount> buckets_{};
    std::atomic<std::uint64_t> count_{0};
    std::atomic<std::uint64_t> sum_{0};
    std::atomic<std::uint64_t> min_{std::numeric_limits<std::uint64_t>::max()};
    std::atomic<std::uint64_t> max_{0};
};

}  // namespace telemetry
