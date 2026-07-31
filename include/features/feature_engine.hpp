#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "book/order_book.hpp"
#include "features/welford.hpp"

// Phase 3 — Feature engine.
//
// Three rules, all from Part 18 Phase 3 and Part 10.2:
//
//  1. Warm mask. A feature that has not seen enough history is not "zero", it is
//     *unavailable*. Part 10.2 lists "feature warm-mask satisfied" as a
//     pre-trade check, so an un-warm feature must be structurally impossible to
//     trade on rather than merely discouraged.
//  2. NaN / Inf guard. Any non-finite result clears its own warm bit and zeroes
//     its slot. A poisoned feature never reaches a strategy.
//  3. BookSource validity. Depth-derived features are meaningless on an
//     L1_ONLY feed. Validity is a property of (feature, source) and is published
//     as a matrix rather than assumed.

namespace features {

inline constexpr std::size_t feature_count = 7;

enum class Feature : std::size_t {
    mid_price = 0,
    spread_ticks = 1,
    microprice = 2,
    l1_imbalance = 3,
    depth_imbalance = 4,
    return_mean = 5,
    return_stddev = 6
};

[[nodiscard]] constexpr std::uint32_t bit(Feature f) noexcept {
    return 1U << static_cast<std::uint32_t>(f);
}

// The feature x BookSource validity matrix. Depth features need a source that
// actually publishes depth; everything else is available from top of book.
[[nodiscard]] constexpr bool is_supported(Feature f, book::BookSource source) noexcept {
    if (f == Feature::depth_imbalance)
        return static_cast<std::uint8_t>(source) >=
               static_cast<std::uint8_t>(book::BookSource::dom_aggregated);
    return true;
}

struct FeatureVector final {
    std::array<double, feature_count> values{};
    std::uint32_t warm_mask{0};

    [[nodiscard]] bool warm(Feature f) const noexcept { return (warm_mask & bit(f)) != 0; }
    [[nodiscard]] double value(Feature f) const noexcept {
        return values[static_cast<std::size_t>(f)];
    }
};

class FeatureEngine final {
public:
    FeatureEngine() noexcept = default;
    explicit FeatureEngine(std::uint64_t min_return_samples) noexcept
        : min_return_samples_(min_return_samples) {}

    [[nodiscard]] FeatureVector compute(const book::OrderBook& book,
                                        book::BookSource source) noexcept {
        FeatureVector out{};
        if (!book.has_both_sides()) return out;   // nothing is warm on an empty book

        const auto best_bid = book.best(book::Side::bid);
        const auto best_ask = book.best(book::Side::ask);

        const double bid_price = static_cast<double>(best_bid.price_ticks);
        const double ask_price = static_cast<double>(best_ask.price_ticks);
        const double bid_size = static_cast<double>(best_bid.size);
        const double ask_size = static_cast<double>(best_ask.size);

        set(out, Feature::mid_price, (bid_price + ask_price) * 0.5, source);
        set(out, Feature::spread_ticks, ask_price - bid_price, source);

        // Size-weighted top of book. Undefined when the venue publishes no size
        // at L1 — which is the normal case for MT5 forex — so it is guarded
        // rather than silently degenerating to the mid.
        const double size_total = bid_size + ask_size;
        if (size_total > 0.0) {
            set(out, Feature::microprice,
                (bid_price * ask_size + ask_price * bid_size) / size_total, source);
            set(out, Feature::l1_imbalance, (bid_size - ask_size) / size_total, source);
        }

        if (is_supported(Feature::depth_imbalance, source)) {
            double bid_depth_total = 0.0;
            double ask_depth_total = 0.0;
            for (std::size_t i = 0; i < book.depth(book::Side::bid); ++i)
                bid_depth_total += static_cast<double>(book.level(book::Side::bid, i).size);
            for (std::size_t i = 0; i < book.depth(book::Side::ask); ++i)
                ask_depth_total += static_cast<double>(book.level(book::Side::ask, i).size);
            const double depth_total = bid_depth_total + ask_depth_total;
            if (depth_total > 0.0)
                set(out, Feature::depth_imbalance,
                    (bid_depth_total - ask_depth_total) / depth_total, source);
        }

        // Log return of the mid, accumulated in Welford. Only fed once a prior
        // mid exists, so the first observation cannot manufacture a return.
        const double mid = out.value(Feature::mid_price);
        if (out.warm(Feature::mid_price) && mid > 0.0) {
            if (has_previous_mid_ && previous_mid_ > 0.0) {
                const double ratio = mid / previous_mid_;
                if (ratio > 0.0) returns_.update(std::log(ratio));
            }
            previous_mid_ = mid;
            has_previous_mid_ = true;
        }

        if (returns_.count() >= min_return_samples_) {
            set(out, Feature::return_mean, returns_.mean(), source);
            set(out, Feature::return_stddev, returns_.stddev(), source);
        }
        return out;
    }

    void reset() noexcept {
        returns_.reset();
        previous_mid_ = 0.0;
        has_previous_mid_ = false;
    }

    [[nodiscard]] std::uint64_t return_samples() const noexcept { return returns_.count(); }
    [[nodiscard]] std::uint64_t min_return_samples() const noexcept { return min_return_samples_; }

private:
    // Single write path for every feature, so the NaN guard and the source
    // matrix cannot be bypassed by a future feature that forgets to check.
    static void set(FeatureVector& out, Feature f, double value,
                    book::BookSource source) noexcept {
        if (!is_supported(f, source)) return;
        if (!std::isfinite(value)) return;
        out.values[static_cast<std::size_t>(f)] = value;
        out.warm_mask |= bit(f);
    }

    Welford returns_{};
    double previous_mid_{0.0};
    bool has_previous_mid_{false};
    std::uint64_t min_return_samples_{32};
};

}  // namespace features
