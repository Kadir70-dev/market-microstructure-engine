#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/event.hpp"
#include "persist/segment_catalog.hpp"
#include "persist/wal_reader.hpp"

// Follows a WAL directory the recorder is still writing.
//
// Distinct from ReplayAdapter, which consumes a finished run once. A tailer must
// re-read a segment whose committed boundary keeps advancing, resume at exactly
// the frame after the last one delivered, and cross into the next segment only
// once the current one is genuinely exhausted.
//
// The cursor is (segment_index, frames_consumed_in_segment). Frame position is
// counted independently of delivery on every pass, so the resume point is never
// derived from a counter that the same pass is mutating — the defect that made
// the first implementation deliver every other event.
//
// Fail-closed: a CRC failure or a truncated payload is `corrupt` and terminal.
// Preallocated zero-fill and the committed boundary are clean stops, because
// neither is data.

namespace replay {

enum class TailStatus : std::uint8_t {
    ok = 0,          // an event was delivered
    idle,            // nothing new right now; caller should poll again
    end_of_data,     // no further segments and current one exhausted
    corrupt,         // CRC / truncation / malformed frame — terminal
    catalog_error    // directory unreadable, empty, or non-contiguous
};

struct TailCursor final {
    std::uint64_t segment_index{0};
    std::uint64_t frames_consumed{0};

    friend bool operator==(const TailCursor& a, const TailCursor& b) noexcept {
        return a.segment_index == b.segment_index && a.frames_consumed == b.frames_consumed;
    }
};

class WalTailer final {
public:
    [[nodiscard]] bool open(const std::filesystem::path& directory) noexcept {
        directory_ = directory;
        cursor_ = TailCursor{};
        total_ = 0;
        buffered_ = 0;
        buffer_position_ = 0;
        corrupt_ = false;
        return rescan();
    }

    // Resume from a previously persisted cursor. The next event delivered is the
    // one immediately after the last event the cursor accounts for.
    [[nodiscard]] bool resume(const TailCursor& cursor) noexcept {
        cursor_ = cursor;
        buffered_ = 0;
        buffer_position_ = 0;
        return true;
    }

    [[nodiscard]] TailStatus next(core::FixedEvent& out) noexcept {
        if (corrupt_) return TailStatus::corrupt;

        // Serve from the current pass if it still has undelivered frames.
        if (buffer_position_ < buffered_) {
            out = buffer_[buffer_position_++];
            ++cursor_.frames_consumed;
            ++total_;
            return TailStatus::ok;
        }

        if (!rescan()) return TailStatus::catalog_error;
        if (catalog_.empty()) return TailStatus::catalog_error;
        if (cursor_.segment_index >= catalog_.size()) return TailStatus::end_of_data;

        const auto status = fill_from_current_segment();
        if (status == TailStatus::corrupt) { corrupt_ = true; return TailStatus::corrupt; }
        if (status == TailStatus::ok) {
            out = buffer_[buffer_position_++];
            ++cursor_.frames_consumed;
            ++total_;
            return TailStatus::ok;
        }

        // Current segment yielded nothing new. Advance only when a later segment
        // exists, which is what proves the current one is sealed — the recorder
        // rotates before it starts the next index.
        if (cursor_.segment_index + 1 < catalog_.size()) {
            ++cursor_.segment_index;
            cursor_.frames_consumed = 0;
            return next(out);
        }
        return TailStatus::idle;
    }

    [[nodiscard]] const TailCursor& cursor() const noexcept { return cursor_; }
    [[nodiscard]] std::uint64_t total_events() const noexcept { return total_; }
    [[nodiscard]] std::size_t segments() const noexcept { return catalog_.size(); }
    [[nodiscard]] bool corrupt() const noexcept { return corrupt_; }

    // Durable cursor. Text on purpose: a restart cursor that cannot be read by a
    // human during an incident is a liability.
    [[nodiscard]] bool save_cursor(const std::filesystem::path& path) const noexcept {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream file(path, std::ios::trunc);
        if (!file) return false;
        file << cursor_.segment_index << ' ' << cursor_.frames_consumed << '\n';
        return static_cast<bool>(file);
    }

    [[nodiscard]] static bool load_cursor(const std::filesystem::path& path,
                                          TailCursor& out) noexcept {
        std::ifstream file(path);
        if (!file) return false;
        std::uint64_t segment = 0, frames = 0;
        if (!(file >> segment >> frames)) return false;
        out.segment_index = segment;
        out.frames_consumed = frames;
        return true;
    }

private:
    static constexpr std::size_t buffer_capacity = 4096;

    [[nodiscard]] bool rescan() noexcept {
        if (!catalog_.scan(directory_)) return false;
        return !catalog_.empty();
    }

    // Reads the active segment from the beginning, discards frames the cursor
    // already accounts for, and buffers what follows. Frame position is counted
    // on its own variable so the skip decision can never be influenced by how
    // many frames this pass has delivered.
    [[nodiscard]] TailStatus fill_from_current_segment() noexcept {
        const auto& descriptor = catalog_.segments()[cursor_.segment_index];
        if (descriptor.encoding != persist::SegmentEncoding::raw) {
            // A compressed-only segment cannot be tailed. Advancing past it
            // would silently drop its interval, so it is a hard error.
            return TailStatus::corrupt;
        }

        persist::WalReader reader;
        if (!reader.open(descriptor.path)) return TailStatus::idle;

        buffered_ = 0;
        buffer_position_ = 0;

        std::uint64_t position = 0;
        core::FixedEvent event{};
        for (;;) {
            const auto status = reader.next(event);
            if (status == persist::ReadStatus::ok) {
                const auto index = position++;
                if (index < cursor_.frames_consumed) continue;   // already delivered
                buffer_[buffered_++] = event;
                if (buffered_ >= buffer_capacity) break;          // deliver in chunks
                continue;
            }
            // Boundary and zero-fill are clean stops: preallocated space is not
            // data and an unwritten tail is not truncation.
            if (persist::is_clean_stop(status)) break;
            // Anything else is a malformed or torn frame. Never treat a partial
            // record as complete.
            return TailStatus::corrupt;
        }

        return buffered_ > 0 ? TailStatus::ok : TailStatus::idle;
    }

    std::filesystem::path directory_{};
    persist::SegmentCatalog catalog_{};
    TailCursor cursor_{};
    std::array<core::FixedEvent, buffer_capacity> buffer_{};
    std::size_t buffered_{0};
    std::size_t buffer_position_{0};
    std::uint64_t total_{0};
    bool corrupt_{false};
};

}  // namespace replay
