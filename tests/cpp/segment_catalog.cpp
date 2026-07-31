#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "persist/segment_catalog.hpp"

namespace {

std::filesystem::path unique_dir(const char* label) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string("mme_cat_") + label + "_" + std::to_string(stamp));
}

void touch(const std::filesystem::path& path, std::size_t bytes = 8) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    const std::string filler(bytes, '\0');
    output.write(filler.data(), static_cast<std::streamsize>(filler.size()));
}

std::string name_for(std::uint64_t index, const char* suffix) {
    std::string digits = std::to_string(index);
    return std::string(20 - digits.size(), '0') + digits + suffix;
}

}  // namespace

int main() {
    std::error_code error;

    // ---- filename parsing ---------------------------------------------------
    {
        std::uint64_t index = 0;
        persist::SegmentEncoding encoding = persist::SegmentEncoding::compressed;
        if (!persist::SegmentCatalog::parse_index("00000000000000000007.wal", index, encoding))
            return 1;
        if (index != 7 || encoding != persist::SegmentEncoding::raw) return 1;

        if (!persist::SegmentCatalog::parse_index("00000000000000000042.wal.zst", index, encoding))
            return 1;
        if (index != 42 || encoding != persist::SegmentEncoding::compressed) return 1;

        // Non-segments must be ignored, including the sidecar and temporaries.
        for (const char* rejected : {"00000000000000000007.wal.meta", "00000000000000000007.wal.zst.tmp",
                                     "capture.log", "7.wal", "0000000000000000000x.wal", ".wal"}) {
            if (persist::SegmentCatalog::parse_index(rejected, index, encoding)) return 1;
        }
    }

    // ---- ordering is numeric, and non-segments are excluded -----------------
    {
        const auto dir = unique_dir("order");
        std::filesystem::create_directories(dir, error);
        for (const std::uint64_t index : {std::uint64_t{2}, std::uint64_t{0}, std::uint64_t{1}}) {
            touch(dir / name_for(index, ".wal"));
            touch(dir / name_for(index, ".wal.meta"));
        }
        touch(dir / "capture.log");
        touch(dir / "00000000000000000002.wal.zst.tmp");

        persist::SegmentCatalog catalog;
        if (!catalog.scan(dir)) return 1;
        if (catalog.size() != 3) return 1;
        for (std::size_t i = 0; i < catalog.segments().size(); ++i)
            if (catalog.segments()[i].index != i) return 1;
        if (!catalog.stats().contiguous || catalog.stats().index_gaps != 0) return 1;
        if (catalog.stats().raw_segments != 3 || catalog.stats().compressed_segments != 0) return 1;
        if (catalog.stats().first_index != 0 || catalog.stats().last_index != 2) return 1;
        for (const auto& segment : catalog.segments())
            if (!segment.has_metadata) return 1;
        std::filesystem::remove_all(dir, error);
    }

    // ---- a missing index is reported, never silently replayed across --------
    {
        const auto dir = unique_dir("gap");
        std::filesystem::create_directories(dir, error);
        for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{5}})
            touch(dir / name_for(index, ".wal"));

        persist::SegmentCatalog catalog;
        if (!catalog.scan(dir)) return 1;
        if (catalog.size() != 3) return 1;
        if (catalog.stats().contiguous) return 1;
        if (catalog.stats().index_gaps != 1) return 1;
        if (catalog.stats().last_index != 5) return 1;
        std::filesystem::remove_all(dir, error);
    }

    // ---- raw wins when both encodings exist for one index -------------------
    {
        const auto dir = unique_dir("both");
        std::filesystem::create_directories(dir, error);
        touch(dir / name_for(0, ".wal"));
        touch(dir / name_for(0, ".wal.meta"));
        touch(dir / name_for(0, ".wal.zst"));
        touch(dir / name_for(1, ".wal.zst"));

        persist::SegmentCatalog catalog;
        if (!catalog.scan(dir)) return 1;
        if (catalog.size() != 2) return 1;
        if (catalog.segments()[0].encoding != persist::SegmentEncoding::raw) return 1;
        if (!catalog.segments()[0].has_metadata) return 1;
        if (catalog.segments()[1].encoding != persist::SegmentEncoding::compressed) return 1;
        if (catalog.stats().raw_segments != 1 || catalog.stats().compressed_segments != 1) return 1;
        std::filesystem::remove_all(dir, error);
    }

    // ---- empty and missing directories --------------------------------------
    {
        const auto dir = unique_dir("empty");
        std::filesystem::create_directories(dir, error);
        persist::SegmentCatalog catalog;
        if (!catalog.scan(dir)) return 1;
        if (!catalog.empty() || catalog.stats().index_gaps != 0) return 1;
        if (!catalog.stats().contiguous) return 1;
        std::filesystem::remove_all(dir, error);

        if (catalog.scan(unique_dir("absent"))) return 1;
    }

    std::cout << "segment_catalog_parse=pass\n";
    std::cout << "segment_catalog_ordering=pass\n";
    std::cout << "segment_catalog_index_gaps=pass\n";
    std::cout << "segment_catalog_encoding_preference=pass\n";
    return 0;
}
