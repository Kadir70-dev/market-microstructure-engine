#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace persist {

// Forward-only bounded byte source over one segment. Deliberately not seekable:
// replay is a sequential scan, and forbidding random access here keeps the
// reader honest about never searching forward past a damaged frame.
//
// Implementations must use a fixed buffer so memory is O(1) in segment size.
class ISegmentStream {
public:
    virtual ~ISegmentStream() = default;
    ISegmentStream() = default;
    ISegmentStream(const ISegmentStream&) = delete;
    ISegmentStream& operator=(const ISegmentStream&) = delete;

    [[nodiscard]] virtual bool open(const std::filesystem::path& path) noexcept = 0;

    // Reads exactly `size` bytes, or fewer at end of file. Returns bytes read.
    [[nodiscard]] virtual std::size_t read(void* destination, std::size_t size) noexcept = 0;

    [[nodiscard]] virtual bool skip(std::uint64_t size) noexcept = 0;
    [[nodiscard]] virtual std::uint64_t position() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    [[nodiscard]] virtual bool is_open() const noexcept = 0;
    virtual void close() noexcept = 0;
};

inline constexpr std::size_t segment_stream_buffer_bytes = 64U * 1024U;

// Uncompressed `.wal` access. Opened read-only; the class has no write, resize
// or truncate surface at all, so a reader physically cannot mutate a segment.
class RawFileStream final : public ISegmentStream {
public:
    RawFileStream() noexcept = default;
    ~RawFileStream() noexcept override;

    [[nodiscard]] bool open(const std::filesystem::path& path) noexcept override;
    [[nodiscard]] std::size_t read(void* destination, std::size_t size) noexcept override;
    [[nodiscard]] bool skip(std::uint64_t size) noexcept override;
    [[nodiscard]] std::uint64_t position() const noexcept override { return position_; }
    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }
    void close() noexcept override;

private:
    std::ifstream input_{};
    std::array<char, segment_stream_buffer_bytes> buffer_{};
    std::uint64_t position_{0};
    std::uint64_t size_{0};
    bool open_{false};
};

}  // namespace persist
