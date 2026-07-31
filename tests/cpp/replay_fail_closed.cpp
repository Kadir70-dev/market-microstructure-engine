#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "replay/replay_engine.hpp"
#include "replay_fixture.hpp"

// Objective 9: fail closed on a corrupted WAL. A replay that silently stops at
// the first bad byte and reports success would produce a truncated history that
// looks complete — the most dangerous possible failure for a backtest, because
// every downstream number stays plausible.

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}

std::vector<std::filesystem::path> segments_of(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        if (entry.path().extension() == ".wal") out.push_back(entry.path());
    std::sort(out.begin(), out.end());
    return out;
}

bool flip_byte(const std::filesystem::path& path, std::uint64_t offset) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) return false;
    file.seekg(static_cast<std::streamoff>(offset));
    char value = 0;
    if (!file.read(&value, 1)) return false;
    value = static_cast<char>(value ^ 1);
    file.seekp(static_cast<std::streamoff>(offset));
    return static_cast<bool>(file.write(&value, 1));
}
}

int main() {
    // ---- baseline: a clean run must succeed --------------------------------
    {
        const auto dir = replay_fixture::make_wal("fc_clean", 2'000, 64 * 1024);
        replay::ReplayEngine engine;
        const auto result = engine.run(dir);
        check(result.ok, "fail_closed_clean_run_ok");
        check(result.events == 2'000, "fail_closed_clean_event_count");
        replay_fixture::remove_all(dir);
    }

    // ---- corrupted payload: CRC must stop the replay, not truncate it ------
    {
        const auto dir = replay_fixture::make_wal("fc_corrupt", 2'000, 64 * 1024);
        const auto files = segments_of(dir);
        check(!files.empty(), "fail_closed_fixture_has_segments");
        // Well past the 256-byte header, inside a committed frame.
        check(flip_byte(files.front(), 256 + 600), "fail_closed_byte_flipped");

        replay::ReplayEngine engine;
        const auto result = engine.run(dir);
        check(!result.ok, "fail_closed_corrupt_run_rejected");
        check(result.error == feed::ReplayError::corrupt_frame, "fail_closed_reports_corrupt_frame");
        check(result.digest == 0, "fail_closed_emits_no_digest");
        replay_fixture::remove_all(dir);
    }

    // ---- missing segment: a hole must not be replayed across ---------------
    {
        const auto dir = replay_fixture::make_wal("fc_gap", 6'000, 32 * 1024);
        auto files = segments_of(dir);
        check(files.size() >= 3, "fail_closed_gap_fixture_multi_segment");
        if (files.size() >= 3) {
            // Remove a middle segment and its sidecar: the catalog becomes
            // non-contiguous, which Part 21 treats as an unusable interval.
            std::error_code ignored;
            std::filesystem::remove(files[1], ignored);
            std::filesystem::remove(files[1].string() + ".meta", ignored);

            replay::ReplayEngine engine;
            const auto result = engine.run(dir);
            check(!result.ok, "fail_closed_gap_rejected");
            check(result.error == feed::ReplayError::catalog_not_contiguous,
                  "fail_closed_reports_non_contiguous");
        }
        replay_fixture::remove_all(dir);
    }

    // ---- empty and missing directories ------------------------------------
    {
        const auto dir = replay_fixture::unique_dir("fc_empty");
        std::filesystem::create_directories(dir);
        replay::ReplayEngine engine;
        const auto result = engine.run(dir);
        check(!result.ok, "fail_closed_empty_dir_rejected");
        check(result.error == feed::ReplayError::catalog_empty, "fail_closed_reports_empty");
        replay_fixture::remove_all(dir);

        const auto missing = replay_fixture::unique_dir("fc_missing");
        const auto absent = engine.run(missing);
        check(!absent.ok, "fail_closed_missing_dir_rejected");
    }

    return failures == 0 ? 0 : 1;
}
