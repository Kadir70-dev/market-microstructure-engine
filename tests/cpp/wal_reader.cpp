#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "core/event.hpp"
#include "persist/wal_reader.hpp"
#include "persist/wal_writer.hpp"

namespace {

std::filesystem::path unique_dir(const char* label) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string("mme_rd_") + label + "_" + std::to_string(stamp));
}

struct Fixture {
    std::filesystem::path dir;
    std::filesystem::path wal;
    std::uint64_t boundary{0};
};

// Fixtures are produced by the real WalWriter, so these tests read
// recorder-generated files rather than hand-rolled bytes.
Fixture make_fixture(const char* label, int records) {
    Fixture fixture{unique_dir(label), {}};
    persist::WalConfig config{fixture.dir};
    config.segment_data_bytes = 1 << 20;
    persist::WalWriter writer;
    if (!writer.open(config, {}, 0)) return {};
    core::FixedEvent event{};
    for (int i = 0; i < records; ++i) {
        event.header.seq_global = static_cast<std::uint64_t>(i + 1);
        event.header.ts_local_ns = static_cast<std::uint64_t>(1'000 + i);
        if (writer.append(0, 0, &event, sizeof(event)) != persist::AppendResult::committed)
            return {};
    }
    fixture.boundary = writer.committed_bytes();
    fixture.wal = writer.current_path();
    writer.close();
    return fixture;
}

bool flip_byte(const std::filesystem::path& path, std::uint64_t offset) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(static_cast<std::streamoff>(offset));
    char value = 0;
    file.read(&value, 1);
    value ^= 1;
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(&value, 1);
    return file.good();
}

bool write_u32(const std::filesystem::path& path, std::uint64_t offset, std::uint32_t value) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return file.good();
}

std::vector<char> read_all(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(input)), {});
}

}  // namespace

int main() {
    std::error_code error;

    // ---- happy path: read every event a recorder wrote ---------------------
    {
        auto fixture = make_fixture("valid", 4);
        if (fixture.wal.empty()) return 1;
        persist::WalReader reader;
        if (!reader.open(fixture.wal)) return 1;
        if (!reader.boundary_from_metadata()) return 1;
        if (reader.committed_boundary() != fixture.boundary) return 1;

        core::FixedEvent event{};
        for (std::uint64_t expected = 1; expected <= 4; ++expected) {
            if (reader.next(event) != persist::ReadStatus::ok) return 1;
            if (event.header.seq_global != expected) return 1;
            if (event.header.ts_local_ns != 999 + expected) return 1;
        }
        if (reader.next(event) != persist::ReadStatus::boundary_reached) return 1;
        const auto& stats = reader.stats();
        if (stats.frames_read != 4 || stats.crc_failures != 0 || stats.sequence_gaps != 0) return 1;
        if (stats.first_seq != 1 || stats.last_seq != 4) return 1;
        if (stats.first_ts_local_ns != 1'000 || stats.last_ts_local_ns != 1'003) return 1;
        if (stats.bytes_read != 4 * 140) return 1;
        reader.close();
        std::filesystem::remove_all(fixture.dir, error);
    }

    // ---- CRC corruption stops at the damaged frame, never past it ----------
    {
        auto fixture = make_fixture("crc", 3);
        if (fixture.wal.empty()) return 1;
        if (!flip_byte(fixture.wal, persist::wal_header_size + 140 + 20)) return 1;
        persist::WalReader reader;
        if (!reader.open(fixture.wal)) return 1;
        core::FixedEvent event{};
        if (reader.next(event) != persist::ReadStatus::ok) return 1;
        if (reader.next(event) != persist::ReadStatus::crc_mismatch) return 1;
        if (reader.stats().crc_failures != 1) return 1;
        if (reader.stats().frames_read != 1) return 1;
        reader.close();
        std::filesystem::remove_all(fixture.dir, error);
    }

    // ---- out-of-range payload length ---------------------------------------
    {
        auto fixture = make_fixture("length", 3);
        if (fixture.wal.empty()) return 1;
        if (!write_u32(fixture.wal, persist::wal_header_size + 140,
                       static_cast<std::uint32_t>(persist::max_wal_payload_size + 1))) return 1;
        persist::WalReader reader;
        if (!reader.open(fixture.wal)) return 1;
        core::FixedEvent event{};
        if (reader.next(event) != persist::ReadStatus::ok) return 1;
        if (reader.next(event) != persist::ReadStatus::bad_length) return 1;
        reader.close();
        std::filesystem::remove_all(fixture.dir, error);
    }

    // ---- committed boundary is honoured over physical size -----------------
    // The segment is grown with zeros past its boundary, exactly as an unclean
    // shutdown leaves preallocated space. The reader must stop at the boundary.
    {
        auto fixture = make_fixture("prefault", 2);
        if (fixture.wal.empty()) return 1;
        std::filesystem::resize_file(fixture.wal, persist::wal_header_size + (1 << 20), error);
        if (error) return 1;
        persist::WalReader reader;
        if (!reader.open(fixture.wal)) return 1;
        if (!reader.boundary_from_metadata()) return 1;
        if (reader.committed_boundary() != 280) return 1;
        core::FixedEvent event{};
        if (reader.next(event) != persist::ReadStatus::ok) return 1;
        if (reader.next(event) != persist::ReadStatus::ok) return 1;
        if (reader.next(event) != persist::ReadStatus::boundary_reached) return 1;
        if (reader.stats().frames_read != 2) return 1;
        reader.close();
        std::filesystem::remove_all(fixture.dir, error);
    }

    // ---- zero fill without a sidecar reads as a clean stop, not corruption --
    {
        auto fixture = make_fixture("zerofill", 2);
        if (fixture.wal.empty()) return 1;
        std::filesystem::remove(fixture.wal.string() + ".meta", error);
        std::filesystem::resize_file(fixture.wal, persist::wal_header_size + 280 + 512, error);
        if (error) return 1;
        persist::WalReader reader;
        if (!reader.open(fixture.wal)) return 1;
        if (reader.boundary_from_metadata()) return 1;  // fell back to file size
        core::FixedEvent event{};
        if (reader.next(event) != persist::ReadStatus::ok) return 1;
        if (reader.next(event) != persist::ReadStatus::ok) return 1;
        if (reader.next(event) != persist::ReadStatus::zero_fill) return 1;
        reader.close();
        std::filesystem::remove_all(fixture.dir, error);
    }

    // ---- truncated tail ----------------------------------------------------
    {
        auto fixture = make_fixture("torn", 3);
        if (fixture.wal.empty()) return 1;
        std::filesystem::resize_file(
            fixture.wal, persist::wal_header_size + fixture.boundary - 20, error);
        if (error) return 1;
        persist::WalReader reader;
        if (!reader.open(fixture.wal)) return 1;
        core::FixedEvent event{};
        if (reader.next(event) != persist::ReadStatus::ok) return 1;
        if (reader.next(event) != persist::ReadStatus::ok) return 1;
        const auto status = reader.next(event);
        if (status != persist::ReadStatus::frame_exceeds_boundary &&
            status != persist::ReadStatus::incomplete_payload) return 1;
        reader.close();
        std::filesystem::remove_all(fixture.dir, error);
    }

    // ---- sequence gap detection --------------------------------------------
    {
        auto fixture = unique_dir("gap");
        persist::WalConfig config{fixture};
        config.segment_data_bytes = 1 << 20;
        persist::WalWriter writer;
        if (!writer.open(config, {}, 0)) return 1;
        core::FixedEvent event{};
        for (const std::uint64_t seq : {std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{9}}) {
            event.header.seq_global = seq;
            if (writer.append(0, 0, &event, sizeof(event)) != persist::AppendResult::committed)
                return 1;
        }
        const auto path = writer.current_path();
        writer.close();

        persist::WalReader reader;
        if (!reader.open(path)) return 1;
        core::FixedEvent decoded{};
        while (reader.next(decoded) == persist::ReadStatus::ok) {}
        if (reader.stats().frames_read != 3) return 1;
        if (reader.stats().sequence_gaps != 1) return 1;
        if (reader.stats().lost_sequences != 6) return 1;  // expected 3, saw 9
        reader.close();
        std::filesystem::remove_all(fixture, error);
    }

    // ---- rejects a file that is not a WAL ----------------------------------
    {
        const auto dir = unique_dir("badheader");
        std::filesystem::create_directories(dir, error);
        const auto path = dir / "00000000000000000000.wal";
        {
            std::ofstream output(path, std::ios::binary);
            const std::vector<char> junk(persist::wal_header_size + 140, 'x');
            output.write(junk.data(), static_cast<std::streamsize>(junk.size()));
        }
        persist::WalReader reader;
        if (reader.open(path)) return 1;
        core::FixedEvent event{};
        if (reader.next(event) != persist::ReadStatus::not_open) return 1;
        std::filesystem::remove_all(dir, error);
    }

    // ---- the reader never mutates the segment or its sidecar ---------------
    {
        auto fixture = make_fixture("readonly", 5);
        if (fixture.wal.empty()) return 1;
        const auto meta = std::filesystem::path(fixture.wal.string() + ".meta");
        const auto wal_before = read_all(fixture.wal);
        const auto meta_before = read_all(meta);
        const auto wal_time_before = std::filesystem::last_write_time(fixture.wal, error);
        const auto meta_time_before = std::filesystem::last_write_time(meta, error);

        persist::WalReader reader;
        if (!reader.open(fixture.wal)) return 1;
        core::FixedEvent event{};
        while (reader.next(event) == persist::ReadStatus::ok) {}
        reader.close();

        if (read_all(fixture.wal) != wal_before) return 1;
        if (read_all(meta) != meta_before) return 1;
        if (std::filesystem::last_write_time(fixture.wal, error) != wal_time_before) return 1;
        if (std::filesystem::last_write_time(meta, error) != meta_time_before) return 1;
        std::filesystem::remove_all(fixture.dir, error);
    }

    // ---- repeated full passes are identical --------------------------------
    {
        auto fixture = make_fixture("repeat", 6);
        if (fixture.wal.empty()) return 1;
        std::vector<std::uint64_t> first_pass;
        for (int pass = 0; pass < 3; ++pass) {
            persist::WalReader reader;
            if (!reader.open(fixture.wal)) return 1;
            std::vector<std::uint64_t> seen;
            core::FixedEvent event{};
            while (reader.next(event) == persist::ReadStatus::ok)
                seen.push_back(event.header.seq_global);
            reader.close();
            if (pass == 0) first_pass = seen;
            else if (seen != first_pass) return 1;
        }
        if (first_pass.size() != 6) return 1;
        std::filesystem::remove_all(fixture.dir, error);
    }

    std::cout << "wal_reader_frames=pass\n";
    std::cout << "wal_reader_crc=pass\n";
    std::cout << "wal_reader_boundary=pass\n";
    std::cout << "wal_reader_sequence_gaps=pass\n";
    std::cout << "wal_reader_readonly=pass\n";
    return 0;
}
