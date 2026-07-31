#pragma once
#include <cstdint>
namespace risk {
enum class HaltReason : std::uint8_t { none, daily_loss, drawdown, consecutive_losses,
 feed_stale, sequence_gap, backpressure, latency, reject_rate, disconnected,
 reconciliation, instance_lock, stale_epoch, disk, manual_kill, control_kill };
struct HaltSignals final {
 std::int64_t pnl{0}, equity{0}, peak_equity{0}; std::uint32_t consecutive_losses{0};
 std::uint64_t feed_age_ms{0}, disk_free_bytes{~0ULL}; std::uint32_t ring_percent{0};
 std::uint32_t reject_rate_bp{0}; bool gap{false}, latency_breach{false}, connected{true};
 bool reconciliation_break{false}, lock_held{true}, stale_epoch{false};
 bool manual_kill{false}, control_kill{false};
};
}
