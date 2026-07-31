#pragma once

#include <cstdint>

namespace feed {

enum class HeartbeatState : std::uint8_t { stale, recovering, healthy };

class HeartbeatMonitor final {
public:
    explicit constexpr HeartbeatMonitor(std::uint64_t timeout_ns,
                                        std::uint32_t clean_required = 3) noexcept
        : timeout_ns_(timeout_ns), clean_required_(clean_required == 0 ? 1 : clean_required) {}

    [[nodiscard]] HeartbeatState observe(std::uint64_t receive_ns,
                                         bool connected) noexcept {
        last_receive_ns_ = receive_ns;
        if (!connected) { clean_count_ = 0; state_ = HeartbeatState::stale; return state_; }
        if (clean_count_ < clean_required_) ++clean_count_;
        state_ = clean_count_ >= clean_required_ ? HeartbeatState::healthy
                                                  : HeartbeatState::recovering;
        return state_;
    }

    [[nodiscard]] HeartbeatState poll(std::uint64_t now_ns) noexcept {
        if (last_receive_ns_ == 0 || now_ns < last_receive_ns_ ||
            now_ns - last_receive_ns_ > timeout_ns_) {
            clean_count_ = 0;
            state_ = HeartbeatState::stale;
        }
        return state_;
    }

private:
    std::uint64_t timeout_ns_;
    std::uint64_t last_receive_ns_{0};
    std::uint32_t clean_required_;
    std::uint32_t clean_count_{0};
    HeartbeatState state_{HeartbeatState::stale};
};

}  // namespace feed
