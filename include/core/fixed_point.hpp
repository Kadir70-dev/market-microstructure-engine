#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace core {

class FixedPoint final {
public:
    using rep = std::int64_t;

    constexpr FixedPoint() noexcept = default;
    explicit constexpr FixedPoint(rep ticks) noexcept : ticks_(ticks) {}

    [[nodiscard]] static FixedPoint from_price(double price,
                                                double tick_size) noexcept {
        if (!std::isfinite(price) || !std::isfinite(tick_size) || tick_size <= 0.0) {
            return FixedPoint{};
        }
        const double scaled = price / tick_size;
        if (scaled > static_cast<double>(std::numeric_limits<rep>::max()) ||
            scaled < static_cast<double>(std::numeric_limits<rep>::min())) {
            return FixedPoint{};
        }
        return FixedPoint{static_cast<rep>(std::llround(scaled))};
    }

    [[nodiscard]] constexpr rep ticks() const noexcept { return ticks_; }
    [[nodiscard]] double to_price(double tick_size) const noexcept {
        return static_cast<double>(ticks_) * tick_size;
    }

    friend constexpr bool operator==(FixedPoint lhs, FixedPoint rhs) noexcept {
        return lhs.ticks_ == rhs.ticks_;
    }
    friend constexpr bool operator!=(FixedPoint lhs, FixedPoint rhs) noexcept {
        return !(lhs == rhs);
    }
    friend constexpr bool operator<(FixedPoint lhs, FixedPoint rhs) noexcept {
        return lhs.ticks_ < rhs.ticks_;
    }

private:
    rep ticks_{0};
};

}  // namespace core
