#pragma once
#include <cstdint>
#include "exec/exec_types.hpp"
namespace risk {
[[nodiscard]] constexpr bool self_trade_ok(bool hedging, bool allow_opposition,
    std::uint32_t owner_strategy, std::uint32_t incoming_strategy,
    std::int64_t current_net, exec::Side incoming_side) noexcept {
    if (!hedging || allow_opposition || current_net == 0 || owner_strategy == incoming_strategy)
        return true;
    return (current_net > 0 && incoming_side == exec::Side::buy) ||
           (current_net < 0 && incoming_side == exec::Side::sell);
}
}
