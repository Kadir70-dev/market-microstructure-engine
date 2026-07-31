#pragma once

#include <cstdint>
#include <filesystem>

#include "core/clock.hpp"
#include "core/event.hpp"
#include "core/event_order.hpp"
#include "feed/feed_adapter.hpp"
#include "persist/segment_catalog.hpp"
#include "persist/wal_reader.hpp"

// Phase 4 — Replay feed adapter.
//
// Satisfies IFeedAdapter over a recorded WAL run, so the Book Engine and Feature
// Engine consume replay through the same seam as live MT5 ingest. Time is a
// VirtualClock driven entirely by the recorded ts_local_ns: no wall-clock read
// occurs anywhere on this path, which is what makes a 1x run and a 1000x run
// produce identical output (Part 11.3).
//
// Fail-closed by construction. Every condition that could yield a silently
// partial or reordered history is terminal:
//   * a non-contiguous segment catalog (a deleted or never-flushed segment)
//   * any WalReader status that is not `ok` or a clean stop
//   * an event that sorts before its predecessor under the total order
// Objective 9 requires exactly this, and Part 21 classifies a gap as "halt; mark
// interval unusable" rather than something to replay across.

namespace feed {

enum class ReplayError : std::uint8_t {
    none = 0,
    not_open,
    catalog_scan_failed,
    catalog_empty,
    catalog_not_contiguous,
    segment_open_failed,
    corrupt_frame,
    order_regression
};

struct ReplayStats final {
    std::uint64_t events{0};
    std::uint64_t segments_opened{0};
    std::uint64_t first_ts_local_ns{0};
    std::uint64_t last_ts_local_ns{0};
    persist::ReadStatus last_status{persist::ReadStatus::ok};
};

class ReplayAdapter final : public IFeedAdapter {
public:
    // Scans and validates the run up front. Returning false here rather than
    // mid-stream means a caller can never begin a replay it cannot finish.
    [[nodiscard]] bool open(const std::filesystem::path& directory) noexcept {
        close();
        if (!catalog_.scan(directory)) { error_ = ReplayError::catalog_scan_failed; return false; }
        if (catalog_.empty()) { error_ = ReplayError::catalog_empty; return false; }
        if (!catalog_.stats().contiguous) {
            error_ = ReplayError::catalog_not_contiguous;
            return false;
        }
        segment_ = 0;
        if (!open_current_segment()) return false;
        open_ = true;
        error_ = ReplayError::none;
        return true;
    }

    [[nodiscard]] PollResult poll(core::FixedEvent& out) noexcept override {
        if (!open_) { error_ = ReplayError::not_open; return PollResult::error; }

        for (;;) {
            const auto status = reader_.next(out);
            stats_.last_status = status;

            if (status == persist::ReadStatus::ok) {
                if (have_previous_ && core::event_order_regression(previous_, out.header)) {
                    error_ = ReplayError::order_regression;
                    return PollResult::error;
                }
                // Virtual time never runs backwards. Equal timestamps are
                // legitimate (simultaneous events) and are ordered by the
                // tie-break, not by the clock.
                if (out.header.ts_local_ns > clock_.now_ns())
                    (void)clock_.advance_to(out.header.ts_local_ns);

                if (!have_previous_) stats_.first_ts_local_ns = out.header.ts_local_ns;
                stats_.last_ts_local_ns = out.header.ts_local_ns;
                previous_ = out.header;
                have_previous_ = true;
                ++stats_.events;
                return PollResult::event;
            }

            if (persist::is_clean_stop(status)) {
                if (segment_ + 1 >= catalog_.size()) return PollResult::end_of_stream;
                ++segment_;
                if (!open_current_segment()) return PollResult::error;
                continue;   // resume in the next segment
            }

            // Anything else is a corrupt or truncated frame.
            error_ = ReplayError::corrupt_frame;
            return PollResult::error;
        }
    }

    [[nodiscard]] core::IClock& clock() noexcept override { return clock_; }
    [[nodiscard]] core::VirtualClock& virtual_clock() noexcept { return clock_; }

    [[nodiscard]] ReplayError error() const noexcept { return error_; }
    [[nodiscard]] const ReplayStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const persist::SegmentCatalog& catalog() const noexcept { return catalog_; }

    void close() noexcept {
        reader_.close();
        catalog_.clear();
        clock_.reset(0);
        stats_ = ReplayStats{};
        previous_ = core::EventHeader{};
        have_previous_ = false;
        segment_ = 0;
        open_ = false;
        error_ = ReplayError::none;
    }

private:
    [[nodiscard]] bool open_current_segment() noexcept {
        reader_.close();
        const auto& descriptor = catalog_.segments()[segment_];
        // Compressed segments are not seekable and the reader is raw-only. A run
        // whose raw segment has been reaped cannot be replayed faithfully, so it
        // fails rather than silently skipping the interval.
        if (descriptor.encoding != persist::SegmentEncoding::raw ||
            !reader_.open(descriptor.path)) {
            error_ = ReplayError::segment_open_failed;
            return false;
        }
        ++stats_.segments_opened;
        return true;
    }

    persist::SegmentCatalog catalog_{};
    persist::WalReader reader_{};
    core::VirtualClock clock_{};
    core::EventHeader previous_{};
    ReplayStats stats_{};
    std::size_t segment_{0};
    bool have_previous_{false};
    bool open_{false};
    ReplayError error_{ReplayError::none};
};

}  // namespace feed
