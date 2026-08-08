#include "persist/wal_reader.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

#include "persist/boundary.hpp"

namespace persist {

WalReader::WalReader() noexcept : stream_(new (std::nothrow) RawFileStream{}) {}
WalReader::~WalReader() noexcept { close(); }

bool WalReader::resolve_boundary(const std::filesystem::path& segment,
                                 std::uint64_t physical_data) noexcept {
    boundary_ = physical_data;
    boundary_from_meta_ = false;

    const auto metadata_path = std::filesystem::path(segment.string() + ".meta");
    std::error_code error;
    if (!std::filesystem::exists(metadata_path, error) || error) return true;

    std::ifstream metadata_file(metadata_path, std::ios::binary);
    BoundaryMetadataView metadata{};
    if (!metadata_file) return true;
    metadata_file.read(reinterpret_cast<char*>(&metadata), sizeof(metadata));
    if (metadata_file.gcount() != static_cast<std::streamsize>(sizeof(metadata))) return true;

    std::uint64_t committed = 0;
    if (!select_boundary(metadata, committed)) return true;

    // The sidecar is advisory for a reader: never trust it past what the file
    // physically holds. Preallocated space beyond the boundary is not data.
    boundary_ = std::min(committed, physical_data);
    boundary_from_meta_ = true;
    return true;
}

bool WalReader::open(const std::filesystem::path& segment) noexcept {
    close();
    if (stream_ == nullptr) return false;
    if (!stream_->open(segment)) return false;
    if (stream_->size() < wal_header_size) { stream_->close(); return false; }

    if (stream_->read(&header_, sizeof(header_)) != sizeof(header_)) {
        stream_->close();
        return false;
    }
    if (!valid_header(header_)) { stream_->close(); return false; }

    if (!resolve_boundary(segment, stream_->size() - wal_header_size)) {
        stream_->close();
        return false;
    }
    position_ = 0;
    expected_seq_ = 0;
    have_seq_ = false;
    stats_ = ReadStats{};
    open_ = true;
    return true;
}

ReadStatus WalReader::next(core::FixedEvent& out) noexcept {
    if (!open_ || stream_ == nullptr) return ReadStatus::not_open;

    const auto remaining = boundary_ - std::min(position_, boundary_);
    if (remaining == 0) return ReadStatus::boundary_reached;
    if (remaining < frame_prefix_size) return ReadStatus::incomplete_prefix;

    FramePrefix prefix{};
    if (stream_->read(&prefix, sizeof(prefix)) != sizeof(prefix))
        return ReadStatus::incomplete_prefix;
    position_ += frame_prefix_size;

    // A wholly zero prefix is preallocated space, not a corrupt frame. The
    // writer truncates on clean close, so this only appears after an unclean
    // shutdown. Treated as a clean stop, counted separately from corruption.
    if (prefix.payload_length == 0 && prefix.type == 0 && prefix.flags == 0)
        return ReadStatus::zero_fill;

    if (!valid_payload_length(prefix.payload_length)) return ReadStatus::bad_length;

    const std::uint64_t frame_size =
        frame_prefix_size + prefix.payload_length + frame_crc_size;
    if (frame_size > remaining) return ReadStatus::frame_exceeds_boundary;

    // One bounded frame buffer, sized by the frozen format maximum. No
    // allocation occurs per frame.
    std::array<std::byte, max_frame_size> frame{};
    std::memcpy(frame.data(), &prefix, sizeof(prefix));
    const auto tail = static_cast<std::size_t>(prefix.payload_length) + frame_crc_size;
    if (stream_->read(frame.data() + frame_prefix_size, tail) != tail)
        return ReadStatus::incomplete_payload;
    position_ += tail;

    std::uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, frame.data() + frame_prefix_size + prefix.payload_length,
                sizeof(stored_crc));
    if (crc32(frame.data(), frame_prefix_size + prefix.payload_length) != stored_crc) {
        ++stats_.crc_failures;
        return ReadStatus::crc_mismatch;
    }

    if (prefix.payload_length != sizeof(core::FixedEvent))
        return ReadStatus::unexpected_payload_size;

    std::memcpy(&out, frame.data() + frame_prefix_size, sizeof(out));

    // Sequence continuity within this segment. Cross-segment continuity belongs
    // to the layer that spans segments and is deliberately not inferred here.
    if (have_seq_ && out.header.seq_global != expected_seq_) {
        ++stats_.sequence_gaps;
        if (out.header.seq_global > expected_seq_)
            stats_.lost_sequences += out.header.seq_global - expected_seq_;
    }
    if (!have_seq_) {
        stats_.first_seq = out.header.seq_global;
        stats_.first_ts_local_ns = out.header.ts_local_ns;
        have_seq_ = true;
    }
    expected_seq_ = out.header.seq_global + 1;
    stats_.last_seq = out.header.seq_global;
    stats_.last_ts_local_ns = out.header.ts_local_ns;
    ++stats_.frames_read;
    stats_.bytes_read += frame_size;
    return ReadStatus::ok;
}

ReadStatus WalReader::next_raw(std::byte* out, std::size_t out_capacity,
                               std::uint32_t& out_length) noexcept {
    if (!open_ || stream_ == nullptr) return ReadStatus::not_open;

    const auto remaining = boundary_ - std::min(position_, boundary_);
    if (remaining == 0) return ReadStatus::boundary_reached;
    if (remaining < frame_prefix_size) return ReadStatus::incomplete_prefix;

    FramePrefix prefix{};
    if (stream_->read(&prefix, sizeof(prefix)) != sizeof(prefix))
        return ReadStatus::incomplete_prefix;
    position_ += frame_prefix_size;

    if (prefix.payload_length == 0 && prefix.type == 0 && prefix.flags == 0)
        return ReadStatus::zero_fill;

    if (!valid_payload_length(prefix.payload_length)) return ReadStatus::bad_length;

    const std::uint64_t frame_size = frame_prefix_size + prefix.payload_length + frame_crc_size;
    if (frame_size > remaining) return ReadStatus::frame_exceeds_boundary;

    std::array<std::byte, max_frame_size> frame{};
    std::memcpy(frame.data(), &prefix, sizeof(prefix));
    const auto tail = static_cast<std::size_t>(prefix.payload_length) + frame_crc_size;
    if (stream_->read(frame.data() + frame_prefix_size, tail) != tail)
        return ReadStatus::incomplete_payload;
    position_ += tail;

    std::uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, frame.data() + frame_prefix_size + prefix.payload_length, sizeof(stored_crc));
    if (crc32(frame.data(), frame_prefix_size + prefix.payload_length) != stored_crc) {
        ++stats_.crc_failures;
        return ReadStatus::crc_mismatch;
    }

    if (out == nullptr || prefix.payload_length > out_capacity) return ReadStatus::unexpected_payload_size;

    std::memcpy(out, frame.data() + frame_prefix_size, prefix.payload_length);
    out_length = prefix.payload_length;
    ++stats_.frames_read;
    stats_.bytes_read += frame_size;
    return ReadStatus::ok;
}

void WalReader::close() noexcept {
    if (stream_ != nullptr) stream_->close();
    header_ = WalFileHeader{};
    boundary_ = 0;
    position_ = 0;
    expected_seq_ = 0;
    boundary_from_meta_ = false;
    have_seq_ = false;
    open_ = false;
}

}  // namespace persist
