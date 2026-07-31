#include <cmath>
#include <cstdint>
#include <iostream>

#include "core/event.hpp"
#include "core/fixed_point.hpp"

int main() {
    int failures = 0;
    const double tick_sizes[] = {1.0, 0.1, 0.01, 0.001, 0.0001, 0.00001};
    for (double tick : tick_sizes) {
        for (std::int64_t ticks : {0LL, 1LL, -1LL, 123456LL, -987654LL}) {
            const double price = static_cast<double>(ticks) * tick;
            const auto fixed = core::FixedPoint::from_price(price, tick);
            if (fixed.ticks() != ticks ||
                std::abs(fixed.to_price(tick) - price) > tick * 1e-9) ++failures;
        }
    }
    if (sizeof(core::EventHeader) != 56 || sizeof(core::FixedEvent) != 128) ++failures;
    std::cout << "fixed_point/layout failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
