#pragma once

#include <cstddef>
#include <cstdint>

namespace core {

enum class RingId : std::uint8_t { market_data, command, event, wal, journal, telemetry };
enum class BackpressureAction : std::uint8_t {
    none, halt_new_entries, halt_engine, reject_resource_exhausted, drop_telemetry
};

struct BackpressureDecision final {
    BackpressureAction action{BackpressureAction::none};
    std::uint8_t occupancy_pct{0};
};

[[nodiscard]] constexpr BackpressureDecision evaluate_backpressure(
    RingId ring, std::size_t occupancy, std::size_t capacity) noexcept {
    const auto pct = capacity == 0 ? 100U :
        static_cast<unsigned>((occupancy * 100U) / capacity);
    const auto bounded = static_cast<std::uint8_t>(pct > 100U ? 100U : pct);
    if (ring == RingId::market_data) {
        if (occupancy >= capacity) return {BackpressureAction::halt_engine, bounded};
        if (pct >= 75U) return {BackpressureAction::halt_new_entries, bounded};
        return {BackpressureAction::none, bounded};
    }
    if (occupancy < capacity) return {BackpressureAction::none, bounded};
    if (ring == RingId::command)
        return {BackpressureAction::reject_resource_exhausted, bounded};
    if (ring == RingId::telemetry)
        return {BackpressureAction::drop_telemetry, bounded};
    return {BackpressureAction::halt_engine, bounded};
}

}  // namespace core
