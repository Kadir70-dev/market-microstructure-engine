#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <zstd.h>

#include "core/event.hpp"
#include "feed/recorder.hpp"
#include "persist/wal_writer.hpp"

namespace {
std::filesystem::path unique_dir(const char* label) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string("mme_wal_") + label + "_" + std::to_string(stamp));
}
bool flip_byte(const std::filesystem::path& path, std::uint64_t offset) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(static_cast<std::streamoff>(offset)); char value = 0;
    file.read(&value, 1); value ^= 1; file.seekp(static_cast<std::streamoff>(offset));
    file.write(&value, 1); return file.good();
}
bool write_u32(const std::filesystem::path& path, std::uint64_t offset, std::uint32_t value) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(reinterpret_cast<const char*>(&value), sizeof(value)); return file.good();
}
struct Fixture {
    std::filesystem::path dir;
    std::filesystem::path wal;
    std::uint64_t boundary{0};
};
std::vector<char> read_all(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(input)), {});
}
// Independent round-trip proof: the .zst must decode to the original byte count
// with a matching CRC. compress_closed_segment verifies this internally; asserting
// it here keeps that guarantee observable if the internal check ever regresses.
bool round_trips_exactly(const std::filesystem::path& original,
                         const std::filesystem::path& compressed) {
    const auto source = read_all(original);
    const auto encoded = read_all(compressed);
    if (source.empty() || encoded.empty()) return false;
    const auto declared = ZSTD_getFrameContentSize(encoded.data(), encoded.size());
    if (declared == ZSTD_CONTENTSIZE_ERROR || declared == ZSTD_CONTENTSIZE_UNKNOWN) return false;
    if (declared != source.size()) return false;
    std::vector<char> decoded(static_cast<std::size_t>(declared));
    const auto produced = ZSTD_decompress(decoded.data(), decoded.size(),
                                          encoded.data(), encoded.size());
    if (ZSTD_isError(produced) || produced != source.size()) return false;
    return persist::crc32(decoded.data(), decoded.size()) ==
           persist::crc32(source.data(), source.size());
}
Fixture make_fixture(const char* label, int records = 2) {
    Fixture fixture{unique_dir(label), {}};
    persist::WalConfig config{fixture.dir}; config.segment_data_bytes = 1 << 20;
    persist::WalWriter writer;
    if (!writer.open(config, {}, 0)) return {};
    core::FixedEvent event{};
    for (int i = 0; i < records; ++i) {
        event.header.seq_global = static_cast<std::uint64_t>(i + 1);
        if (writer.append(0, 0, &event, sizeof(event)) != persist::AppendResult::committed) return {};
    }
    fixture.boundary = writer.committed_bytes(); fixture.wal = writer.current_path();
    writer.close(); return fixture;
}
}

int main() {
    std::error_code error;
    {
        auto fixture = make_fixture("valid"); if (fixture.wal.empty()) return 1;
        auto recovered = persist::WalWriter::recover(fixture.wal);
        if (!recovered.success || recovered.valid_records != 2 || recovered.valid_boundary != 280) return 1;
        auto again = persist::WalWriter::recover(fixture.wal);
        if (!again.success || again.truncated || again.valid_boundary != 280) return 1;
        std::filesystem::remove_all(fixture.dir, error);
    }
    {
        auto fixture = make_fixture("crc");
        if (!flip_byte(fixture.wal, persist::wal_header_size + 140 + 20)) return 1;
        auto recovered = persist::WalWriter::recover(fixture.wal);
        if (!recovered.success || recovered.valid_records != 1 || recovered.valid_boundary != 140 || !recovered.truncated) return 1;
        std::filesystem::remove_all(fixture.dir, error);
    }
    {
        auto fixture = make_fixture("length");
        if (!write_u32(fixture.wal, persist::wal_header_size + 140, 1041)) return 1;
        auto recovered = persist::WalWriter::recover(fixture.wal);
        if (!recovered.success || recovered.valid_boundary != 140) return 1;
        std::filesystem::remove_all(fixture.dir, error);
    }
    for (const auto cut : {std::uint64_t{136}, std::uint64_t{20}, std::uint64_t{1}}) {
        auto fixture = make_fixture("torn");
        const auto physical = persist::wal_header_size + fixture.boundary - cut;
        std::filesystem::resize_file(fixture.wal, physical, error); if (error) return 1;
        auto recovered = persist::WalWriter::recover(fixture.wal);
        if (!recovered.success || recovered.valid_boundary != 140 || !recovered.truncated) return 1;
        std::filesystem::remove_all(fixture.dir, error);
    }
    {
        auto fixture = make_fixture("prefault");
        std::filesystem::resize_file(fixture.wal, persist::wal_header_size + (1 << 20), error);
        auto recovered = persist::WalWriter::recover(fixture.wal);
        if (!recovered.success || recovered.valid_records != 2 || recovered.valid_boundary != 280 || !recovered.truncated) return 1;
        std::filesystem::remove_all(fixture.dir, error);
    }
    {
        const auto dir = unique_dir("rotation");
        persist::WalConfig config{dir}; config.segment_data_bytes = 1120;
        persist::WalWriter writer; if (!writer.open(config, {}, 0)) return 1;
        auto recorder = std::make_unique<feed::Recorder>(writer);
        core::FixedEvent event{}, output{}; std::filesystem::path closed;
        for (std::uint64_t i = 1; i <= 8; ++i) {
            event.header.seq_global = i; if (!recorder->submit(event)) return 1;
            if (recorder->drain_one(output, i, closed) != feed::DrainResult::committed) return 1;
        }
        event.header.seq_global = 9; if (!recorder->submit(event)) return 1;
        if (recorder->drain_one(output, 9, closed) != feed::DrainResult::rotated || closed.empty()) return 1;
        writer.close();
        std::filesystem::path compressed;
        if (!persist::WalWriter::compress_closed_segment(closed, compressed) ||
            !std::filesystem::exists(closed) || !std::filesystem::exists(compressed)) return 1;
        // Compression must round-trip to the exact original byte count and CRC.
        if (!round_trips_exactly(closed, compressed)) return 1;
        std::filesystem::remove_all(dir, error);
    }
    {
        // Production compressor-thread path (record_main.cpp:116). A default
        // std::thread inherits the image stack reserve, so this covers the call
        // site rather than asserting a smaller stack.
        auto fixture = make_fixture("thread_compress"); if (fixture.wal.empty()) return 1;
        std::filesystem::path compressed;
        bool ok = false;
        std::thread worker([&] {
            ok = persist::WalWriter::compress_closed_segment(fixture.wal, compressed);
        });
        worker.join();
        if (!ok || !std::filesystem::exists(compressed)) return 1;
        if (!round_trips_exactly(fixture.wal, compressed)) return 1;
        std::filesystem::remove_all(fixture.dir, error);
    }
    {
        // Production startup-recovery path (record_main.cpp:96): recover an
        // existing segment, then compress the recovered file.
        auto fixture = make_fixture("recover_compress"); if (fixture.wal.empty()) return 1;
        auto recovered = persist::WalWriter::recover(fixture.wal);
        if (!recovered.success || recovered.valid_records != 2 ||
            recovered.valid_boundary != 280) return 1;
        std::filesystem::path compressed;
        if (!persist::WalWriter::compress_closed_segment(fixture.wal, compressed) ||
            !std::filesystem::exists(compressed)) return 1;
        if (!round_trips_exactly(fixture.wal, compressed)) return 1;
        std::filesystem::remove_all(fixture.dir, error);
    }
    {
        const auto dir = unique_dir("hourly_rotation");
        persist::WalConfig config{dir}; config.segment_data_bytes = 1 << 20;
        config.rotation_interval = std::chrono::nanoseconds{10};
        persist::WalWriter writer; if (!writer.open(config, {}, 100)) return 1;
        auto recorder = std::make_unique<feed::Recorder>(writer);
        core::FixedEvent event{}, output{}; std::filesystem::path closed;
        if (!recorder->submit(event)) return 1;
        if (recorder->drain_one(output, 111, closed) != feed::DrainResult::rotated || closed.empty()) return 1;
        writer.close(); std::filesystem::remove_all(dir, error);
    }
    {
        const auto dir = unique_dir("saturation");
        persist::WalConfig config{dir}; config.segment_data_bytes = 1 << 20;
        persist::WalWriter writer; if (!writer.open(config, {}, 0)) return 1;
        auto recorder = std::make_unique<feed::Recorder>(writer);
        core::FixedEvent event{};
        for (std::size_t i = 0; i < 65536; ++i) if (!recorder->submit(event)) return 1;
        if (recorder->submit(event) || recorder->state() != feed::RecorderState::halted) return 1;
        writer.close(); std::filesystem::remove_all(dir, error);
    }
    std::cout << "phase2c_wal_recovery_rotation_compression=pass\n";
    return 0;
}
