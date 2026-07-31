#pragma once

#include <cstdint>

namespace feed {

enum class SequenceStatus : std::uint8_t { synchronized, in_order, gap, stale_or_duplicate };

struct SequenceResult final {
    SequenceStatus status{SequenceStatus::synchronized};
    std::uint64_t expected{0};
    std::uint64_t received{0};
    std::uint64_t lost{0};
};

class SequenceTracker final {
public:
    [[nodiscard]] SequenceResult observe(std::uint64_t sequence) noexcept {
        if (!initialized_) {
            initialized_ = true;
            last_ = sequence;
            return {SequenceStatus::synchronized, sequence, sequence, 0};
        }
        const auto expected = last_ + 1;
        if (sequence == expected) {
            last_ = sequence;
            return {SequenceStatus::in_order, expected, sequence, 0};
        }
        if (sequence <= last_)
            return {SequenceStatus::stale_or_duplicate, expected, sequence, 0};
        const auto lost = sequence - expected;
        last_ = sequence;
        gaps_ += lost;
        return {SequenceStatus::gap, expected, sequence, lost};
    }

    void reset() noexcept { initialized_ = false; last_ = 0; gaps_ = 0; }
    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

private:
    std::uint64_t last_{0};
    std::uint64_t gaps_{0};
    bool initialized_{false};
};

}  // namespace feed
