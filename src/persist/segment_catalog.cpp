#include "persist/segment_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace persist {
namespace {

constexpr std::size_t segment_index_digits = 20;

// `00000000000000000000.wal` or `00000000000000000000.wal.zst`. Anything else in
// the directory (`.meta`, `.zst.tmp`, logs) is not a segment.
// std::string::ends_with is C++20; this project is pinned to C++17.
bool has_suffix(const std::string& text, const char* suffix) noexcept {
    const std::size_t length = std::char_traits<char>::length(suffix);
    return text.size() >= length &&
           text.compare(text.size() - length, length, suffix) == 0;
}

bool parse_stem(const std::string& stem, std::uint64_t& index) noexcept {
    if (stem.size() != segment_index_digits) return false;
    std::uint64_t value = 0;
    for (const char character : stem) {
        if (std::isdigit(static_cast<unsigned char>(character)) == 0) return false;
        value = value * 10U + static_cast<std::uint64_t>(character - '0');
    }
    index = value;
    return true;
}

}  // namespace

bool SegmentCatalog::parse_index(const std::filesystem::path& path, std::uint64_t& index,
                                 SegmentEncoding& encoding) noexcept {
    const auto filename = path.filename().string();
    if (filename.size() > segment_index_digits && has_suffix(filename, ".wal")) {
        encoding = SegmentEncoding::raw;
        return parse_stem(filename.substr(0, filename.size() - 4), index);
    }
    if (filename.size() > segment_index_digits && has_suffix(filename, ".wal.zst")) {
        encoding = SegmentEncoding::compressed;
        return parse_stem(filename.substr(0, filename.size() - 8), index);
    }
    return false;
}

void SegmentCatalog::clear() noexcept {
    segments_.clear();
    stats_ = CatalogStats{};
}

bool SegmentCatalog::scan(const std::filesystem::path& directory) noexcept {
    clear();
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) return false;

    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) return false;
        if (!entry.is_regular_file(error) || error) continue;

        std::uint64_t index = 0;
        SegmentEncoding encoding = SegmentEncoding::raw;
        if (!parse_index(entry.path(), index, encoding)) continue;

        // Raw wins when both encodings exist: the writer retains the `.wal`
        // after compressing, and it is the seekable form.
        const auto existing = std::find_if(segments_.begin(), segments_.end(),
            [index](const SegmentDescriptor& candidate) { return candidate.index == index; });
        if (existing != segments_.end()) {
            if (existing->encoding == SegmentEncoding::compressed &&
                encoding == SegmentEncoding::raw) {
                existing->path = entry.path();
                existing->encoding = encoding;
                existing->file_size = entry.file_size(error);
                existing->metadata_path = entry.path().string() + ".meta";
                existing->has_metadata =
                    std::filesystem::exists(existing->metadata_path, error);
            }
            continue;
        }

        SegmentDescriptor descriptor{};
        descriptor.index = index;
        descriptor.path = entry.path();
        descriptor.encoding = encoding;
        descriptor.file_size = entry.file_size(error);
        if (encoding == SegmentEncoding::raw) {
            descriptor.metadata_path = entry.path().string() + ".meta";
            descriptor.has_metadata = std::filesystem::exists(descriptor.metadata_path, error);
        }
        segments_.push_back(std::move(descriptor));
    }

    std::sort(segments_.begin(), segments_.end(),
        [](const SegmentDescriptor& lhs, const SegmentDescriptor& rhs) {
            return lhs.index < rhs.index;
        });

    for (const auto& segment : segments_) {
        if (segment.encoding == SegmentEncoding::raw) ++stats_.raw_segments;
        else ++stats_.compressed_segments;
    }
    if (!segments_.empty()) {
        stats_.first_index = segments_.front().index;
        stats_.last_index = segments_.back().index;
        for (std::size_t i = 1; i < segments_.size(); ++i) {
            const auto previous = segments_[i - 1].index;
            const auto current = segments_[i].index;
            if (current != previous + 1) {
                ++stats_.index_gaps;
                stats_.contiguous = false;
            }
        }
    }
    return true;
}

}  // namespace persist
