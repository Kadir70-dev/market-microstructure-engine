#pragma once

#include <cstdint>

namespace feed {

class ClockOffsetEstimator final {
public:
    void observe(std::uint64_t terminal_utc_ns, std::uint64_t local_utc_ns) noexcept {
        const auto sample = static_cast<std::int64_t>(local_utc_ns - terminal_utc_ns);
        if (!initialized_ || sample < offset_ns_) offset_ns_ = sample;
        initialized_ = true;
    }
    [[nodiscard]] std::uint64_t correct(std::uint64_t terminal_utc_ns) const noexcept {
        if (!initialized_) return terminal_utc_ns;
        if (offset_ns_ >= 0) return terminal_utc_ns + static_cast<std::uint64_t>(offset_ns_);
        return terminal_utc_ns - static_cast<std::uint64_t>(-offset_ns_);
    }
    [[nodiscard]] std::int64_t offset_ns() const noexcept { return offset_ns_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
private:
    std::int64_t offset_ns_{0};
    bool initialized_{false};
};

}  // namespace feed
