#pragma once
#include <cstdint>
#include <limits>
namespace risk {
[[nodiscard]] constexpr std::int64_t projected_free_margin(
    std::int64_t free_margin, std::int64_t order_margin,
    std::int64_t notional, std::int64_t shock_bp) noexcept {
    if (free_margin < 0 || order_margin < 0 || notional < 0 || shock_bp < 0) return -1;
    if (shock_bp != 0 && notional > (std::numeric_limits<std::int64_t>::max() - 9999) / shock_bp)
        return -1;
    const auto shock = (notional * shock_bp + 9999) / 10000;
    if (order_margin > free_margin || shock > free_margin - order_margin) return -1;
    return free_margin - order_margin - shock;
}
}
