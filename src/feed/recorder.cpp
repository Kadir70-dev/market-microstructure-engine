#include "feed/recorder.hpp"

namespace feed {

bool Recorder::submit(const core::FixedEvent& event) noexcept {
    if (state_ == RecorderState::halted) return false;
    if (!ring_.try_push(event)) {
        state_ = RecorderState::halted;
        return false;
    }
    return true;
}

DrainResult Recorder::drain_one(core::FixedEvent& committed_event,
    std::uint64_t monotonic_now_ns, std::filesystem::path& closed_segment) noexcept {
    if (state_ == RecorderState::halted) return DrainResult::halted;
    if (!pending_ && !ring_.try_pop(pending_event_)) return DrainResult::empty;
    pending_ = true;
    bool rotated = false;
    if (writer_.needs_rotation(monotonic_now_ns, sizeof(pending_event_))) {
        if (!writer_.rotate(monotonic_now_ns, closed_segment)) {
            state_ = RecorderState::halted;
            return DrainResult::halted;
        }
        rotated = true;
    }
    const auto result = writer_.append(pending_event_.header.type,
        pending_event_.header.flags, &pending_event_, sizeof(pending_event_));
    if (result != persist::AppendResult::committed) {
        state_ = RecorderState::halted;
        return DrainResult::halted;
    }
    committed_event = pending_event_;
    pending_ = false;
    return rotated ? DrainResult::rotated : DrainResult::committed;
}

}  // namespace feed
