#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace persist {

enum class SegmentEncoding : std::uint8_t { raw, compressed };

struct SegmentDescriptor final {
    std::uint64_t index{0};
    std::filesystem::path path{};
    std::filesystem::path metadata_path{};
    SegmentEncoding encoding{SegmentEncoding::raw};
    bool has_metadata{false};
    std::uint64_t file_size{0};
};

struct CatalogStats final {
    std::uint64_t raw_segments{0};
    std::uint64_t compressed_segments{0};
    std::uint64_t index_gaps{0};       // count of missing indices in the run
    std::uint64_t first_index{0};
    std::uint64_t last_index{0};
    bool contiguous{true};
};

// Discovers and orders the segments of one capture run.
//
// Ordering is numeric on the 20-digit index parsed from the filename, not
// lexicographic on the path, so it stays correct if the naming width ever
// changes. A missing index is reported rather than silently skipped: a gap means
// a segment was deleted or never flushed, and a backtest that quietly replays
// across the hole is producing a result nobody can defend.
//
// Memory is O(segments), never O(events). Segment count is small by design
// (256 MiB or one hour per segment).
class SegmentCatalog final {
public:
    // Scans `directory` for `<20-digit>.wal` and `<20-digit>.wal.zst`. When both
    // encodings exist for one index the raw file wins: it is seekable and is the
    // form the writer retains after compression.
    [[nodiscard]] bool scan(const std::filesystem::path& directory) noexcept;

    [[nodiscard]] const std::vector<SegmentDescriptor>& segments() const noexcept {
        return segments_;
    }
    [[nodiscard]] const CatalogStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t size() const noexcept { return segments_.size(); }
    [[nodiscard]] bool empty() const noexcept { return segments_.empty(); }
    void clear() noexcept;

    // Parses the 20-digit stem. Returns false for any name that is not a segment.
    [[nodiscard]] static bool parse_index(const std::filesystem::path& path,
                                          std::uint64_t& index,
                                          SegmentEncoding& encoding) noexcept;

private:
    std::vector<SegmentDescriptor> segments_{};
    CatalogStats stats_{};
};

}  // namespace persist
