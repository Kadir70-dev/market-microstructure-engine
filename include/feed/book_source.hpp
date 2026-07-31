#pragma once

#include <cstdint>

namespace feed {

enum class BookSource : std::uint8_t {
    l1_only = 0,
    dom_aggregated = 1,
    dom_synthetic = 2,
    l2_exchange = 3,
    l3_mbo = 4
};

[[nodiscard]] constexpr BookSource classify_book_source(
    bool has_dom, bool exchange_depth, bool market_by_order,
    bool synthetic_depth) noexcept {
    if (!has_dom) return BookSource::l1_only;
    if (market_by_order) return BookSource::l3_mbo;
    if (exchange_depth) return BookSource::l2_exchange;
    if (synthetic_depth) return BookSource::dom_synthetic;
    return BookSource::dom_aggregated;
}

}  // namespace feed
