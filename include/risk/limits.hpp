#pragma once
#include <array>
#include <cstdint>

namespace risk {
struct Limits final {
    std::int64_t max_risk_per_trade{100000};
    std::int64_t max_position_per_symbol{1000000};
    std::int64_t max_gross_exposure{5000000};
    std::int64_t max_net_exposure{3000000};
    std::int64_t max_leverage_bp{100000};
    std::int64_t adverse_shock_bp{200};
    std::uint32_t max_open_orders{256};
    std::uint32_t max_in_flight_per_symbol{32};
    std::uint32_t max_order_requests_per_second{1000};
    std::uint32_t max_orders_per_minute{10000};
    std::uint32_t max_fills_per_minute{10000};
    std::uint32_t max_trades_per_day{100000};
    std::uint64_t min_time_between_orders_ns{0};
    std::int64_t max_spread_ticks{100};
    std::int64_t max_slippage_ticks{100};
    std::int64_t max_order_volume{100000};
    std::int64_t max_daily_loss{1000000};
    std::uint32_t max_drawdown_bp{2000};
    std::uint32_t max_consecutive_losses{10};
    std::uint32_t max_staleness_ms{1000};
    std::uint32_t max_reject_rate_bp{1000};
    std::uint64_t min_free_bytes{1073741824ULL};
    std::uint64_t required_warm_mask{1};
    std::array<bool, 8> symbol_allowed{{true,true,true,true,true,false,false,false}};
    bool allow_cross_strategy_opposition{false};
};

[[nodiscard]] constexpr bool self_test(const Limits& x) noexcept {
    return x.max_risk_per_trade > 0 && x.max_position_per_symbol > 0 &&
        x.max_gross_exposure > 0 && x.max_net_exposure > 0 &&
        x.max_gross_exposure >= x.max_net_exposure && x.max_leverage_bp > 0 &&
        x.adverse_shock_bp > 0 && x.max_open_orders > 0 &&
        x.max_in_flight_per_symbol > 0 &&
        x.max_order_requests_per_second > 0 && x.max_orders_per_minute > 0 &&
        x.max_orders_per_minute >= x.max_order_requests_per_second &&
        x.max_fills_per_minute > 0 && x.max_trades_per_day > 0 &&
        x.max_spread_ticks > 0 && x.max_slippage_ticks > 0 &&
        x.max_order_volume > 0 && x.max_daily_loss > 0 &&
        x.max_drawdown_bp > 0 && x.max_drawdown_bp < 10000 &&
        x.max_consecutive_losses > 0 && x.max_staleness_ms > 0 &&
        x.max_reject_rate_bp > 0 && x.max_reject_rate_bp <= 10000 &&
        x.min_free_bytes > 0 && x.required_warm_mask != 0;
}
}
