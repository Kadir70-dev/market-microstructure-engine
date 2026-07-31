#pragma once

#include <cstdint>
#include <filesystem>

#include "core/event.hpp"
#include "core/ring_buffer.hpp"
#include "persist/wal_writer.hpp"

namespace feed {

enum class RecorderState : std::uint8_t { running, halted };
enum class DrainResult : std::uint8_t { empty, committed, rotated, halted };

struct EventTotalOrder final {
    [[nodiscard]] constexpr bool operator()(const core::FixedEvent& lhs,
                                            const core::FixedEvent& rhs) const noexcept {
        if (lhs.header.ts_local_ns != rhs.header.ts_local_ns)
            return lhs.header.ts_local_ns < rhs.header.ts_local_ns;
        if (lhs.header.source_priority != rhs.header.source_priority)
            return lhs.header.source_priority < rhs.header.source_priority;
        return lhs.header.seq_source < rhs.header.seq_source;
    }
};

class Recorder final {
public:
    explicit Recorder(persist::WalWriter& writer) noexcept : writer_(writer) {}

    [[nodiscard]] bool submit(const core::FixedEvent& event) noexcept;
    [[nodiscard]] DrainResult drain_one(core::FixedEvent& committed_event,
        std::uint64_t monotonic_now_ns, std::filesystem::path& closed_segment) noexcept;
    [[nodiscard]] RecorderState state() const noexcept { return state_; }
    [[nodiscard]] std::size_t queued() const noexcept { return ring_.size() + (pending_ ? 1U : 0U); }

private:
    persist::WalWriter& writer_;
    core::RingBuffer<core::FixedEvent, 65536> ring_{};
    core::FixedEvent pending_event_{};
    bool pending_{false};
    RecorderState state_{RecorderState::running};
};

}  // namespace feed
