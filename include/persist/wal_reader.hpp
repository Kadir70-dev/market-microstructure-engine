#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include "core/event.hpp"
#include "persist/framing.hpp"
#include "persist/segment_stream.hpp"

namespace persist {

enum class ReadStatus : std::uint8_t {
    ok,                      // an event was decoded into the caller's buffer
    not_open,                // next() called before a successful open()
    boundary_reached,        // clean stop at the committed boundary
    zero_fill,               // preallocated space reached; never data
    incomplete_prefix,       // fewer than 8 bytes remain
    bad_length,              // payload_length outside [1, max_wal_payload_size]
    frame_exceeds_boundary,  // a well-formed frame would run past the boundary
    incomplete_payload,      // payload or CRC truncated
    crc_mismatch,            // integrity failure
    unexpected_payload_size  // valid frame, but not a 128-byte FixedEvent
};

[[nodiscard]] constexpr bool is_clean_stop(ReadStatus status) noexcept {
    return status == ReadStatus::boundary_reached || status == ReadStatus::zero_fill;
}

struct ReadStats final {
    std::uint64_t frames_read{0};
    std::uint64_t bytes_read{0};
    std::uint64_t crc_failures{0};
    std::uint64_t sequence_gaps{0};
    std::uint64_t lost_sequences{0};
    std::uint64_t first_seq{0};
    std::uint64_t last_seq{0};
    std::uint64_t first_ts_local_ns{0};
    std::uint64_t last_ts_local_ns{0};
};

// Sequential, validating, strictly read-only reader for one segment.
//
// Contract, enforced by test:
//   * opens the segment read-only and never writes, truncates, renames or
//     touches the `.meta` sidecar;
//   * never calls WalWriter::recover(), which repairs by mutating;
//   * stops at the first malformed frame and never searches forward, matching
//     recovery's scan semantics without recovery's side effects;
//   * allocates nothing per frame. Steady-state memory is the fixed stream
//     buffer plus one bounded frame buffer.
class WalReader final {
public:
    WalReader() noexcept;
    ~WalReader() noexcept;
    WalReader(const WalReader&) = delete;
    WalReader& operator=(const WalReader&) = delete;

    // Validates the 256-byte header and resolves the committed boundary from the
    // `.meta` sidecar, falling back to physical size when it is absent or
    // unusable. Returns false on a missing file or an invalid header.
    [[nodiscard]] bool open(const std::filesystem::path& segment) noexcept;

    [[nodiscard]] ReadStatus next(core::FixedEvent& out) noexcept;

    [[nodiscard]] const WalFileHeader& header() const noexcept { return header_; }
    [[nodiscard]] const ReadStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t committed_boundary() const noexcept { return boundary_; }
    [[nodiscard]] bool boundary_from_metadata() const noexcept { return boundary_from_meta_; }
    [[nodiscard]] std::uint64_t data_position() const noexcept { return position_; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }
    void close() noexcept;

private:
    [[nodiscard]] bool resolve_boundary(const std::filesystem::path& segment,
                                        std::uint64_t physical_data) noexcept;

    std::unique_ptr<RawFileStream> stream_;
    WalFileHeader header_{};
    ReadStats stats_{};
    std::uint64_t boundary_{0};
    std::uint64_t position_{0};
    std::uint64_t expected_seq_{0};
    bool boundary_from_meta_{false};
    bool have_seq_{false};
    bool open_{false};
};

}  // namespace persist
