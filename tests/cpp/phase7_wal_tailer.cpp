#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include "feed/replay_adapter.hpp"
#include "persist/wal_writer.hpp"
#include "replay/digest.hpp"
#include "replay/wal_tailer.hpp"
#include "replay_fixture.hpp"

// Regression suite for the tailing defect.
//
// The original implementation compared a skip counter against the resume cursor
// while incrementing that cursor inside the same loop, so it delivered every
// other frame: ceil(9451/2) = 4726 of 9451. Every test below exists to make that
// class of error impossible to reintroduce silently.

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}

// Drains a tailer to exhaustion, recording order and a digest.
struct Drain final {
    std::uint64_t count{0};
    std::uint64_t digest{0};
    std::vector<std::uint64_t> sequence;
    replay::TailStatus terminal{replay::TailStatus::idle};
};

Drain drain(replay::WalTailer& tailer, std::uint64_t guard = 100'000) {
    Drain d{};
    replay::Digest digest;
    core::FixedEvent event{};
    for (std::uint64_t i = 0; i < guard; ++i) {
        const auto status = tailer.next(event);
        if (status == replay::TailStatus::ok) {
            ++d.count;
            digest.mix_header(event.header);
            d.sequence.push_back(event.header.seq_global);
            continue;
        }
        d.terminal = status;
        break;
    }
    d.digest = digest.value();
    return d;
}

// Same WAL read through the finished-run adapter, for cross-checking.
Drain drain_adapter(const std::filesystem::path& dir) {
    Drain d{};
    replay::Digest digest;
    feed::ReplayAdapter adapter;
    if (!adapter.open(dir)) return d;
    core::FixedEvent event{};
    for (;;) {
        if (adapter.poll(event) != feed::PollResult::event) break;
        ++d.count;
        digest.mix_header(event.header);
        d.sequence.push_back(event.header.seq_global);
    }
    d.digest = digest.value();
    return d;
}

// A writer left open so the segment can be grown between reads.
struct GrowingWal final {
    std::filesystem::path dir;
    persist::WalWriter writer;
    std::uint64_t written{0};

    [[nodiscard]] bool start(const char* label, std::uint64_t segment_bytes) {
        dir = replay_fixture::unique_dir(label);
        persist::WalConfig config{dir};
        config.segment_data_bytes = segment_bytes;
        return writer.open(config, {}, 0);
    }
    [[nodiscard]] bool append(int records) {
        core::FixedEvent event{};
        for (int i = 0; i < records; ++i) {
            event.header = core::EventHeader{};
            event.header.seq_global = ++written;
            event.header.ts_local_ns = 1'000'000ULL + written * 1'000ULL;
            event.header.seq_source = static_cast<std::uint32_t>(written);
            event.header.type = static_cast<std::uint16_t>(core::EventType::quote);
            if (writer.append(0, 0, &event, sizeof(event)) != persist::AppendResult::committed)
                return false;
        }
        return writer.flush();
    }
};
}

int main() {
    // ---- 1. sealed segment, full read --------------------------------------
    {
        const auto dir = replay_fixture::make_wal("tail_sealed", 5'000, 1 << 20);
        replay::WalTailer tailer;
        check(tailer.open(dir), "sealed_open");
        const auto d = drain(tailer);
        check(d.count == 5'000, "sealed_full_read_count");
        check(d.terminal == replay::TailStatus::idle ||
              d.terminal == replay::TailStatus::end_of_data, "sealed_clean_stop");
        check(!tailer.corrupt(), "sealed_not_corrupt");
        replay_fixture::remove_all(dir);
    }

    // ---- 2. THE REGRESSION: 9,451 events -----------------------------------
    {
        const auto dir = replay_fixture::make_wal("tail_9451", 9'451, 1 << 20);
        replay::WalTailer tailer;
        check(tailer.open(dir), "regression_open");
        const auto tailed = drain(tailer);

        // The exact defect: 4,726 = ceil(9451/2).
        check(tailed.count != 4'726, "regression_not_half_consumed");
        check(tailed.count == 9'451, "regression_9451_events");

        const auto once = drain_adapter(dir);
        check(once.count == 9'451, "regression_once_mode_9451");
        check(tailed.digest == once.digest, "regression_digest_matches_once_mode");
        check(tailed.sequence == once.sequence, "regression_sequence_order_identical");

        // Zero duplicates, zero skips: seq_global is dense and strictly rising.
        bool dense = tailed.sequence.size() == 9'451;
        for (std::size_t i = 0; dense && i < tailed.sequence.size(); ++i)
            if (tailed.sequence[i] != i + 1) dense = false;
        check(dense, "regression_no_duplicates_no_skips");
        replay_fixture::remove_all(dir);
    }

    // ---- 3. multi-segment transition ---------------------------------------
    {
        const auto dir = replay_fixture::make_wal("tail_multi", 6'000, 32 * 1024);
        replay::WalTailer tailer;
        check(tailer.open(dir), "multi_open");
        check(tailer.segments() > 1, "multi_segment_fixture");
        const auto tailed = drain(tailer);
        check(tailed.count == 6'000, "multi_all_events_across_segments");
        const auto once = drain_adapter(dir);
        check(tailed.digest == once.digest, "multi_digest_matches");
        check(tailer.cursor().segment_index > 0, "multi_cursor_advanced");
        replay_fixture::remove_all(dir);
    }

    // ---- 4. growing segment -------------------------------------------------
    {
        GrowingWal wal;
        check(wal.start("tail_growing", 1 << 20), "growing_start");
        check(wal.append(500), "growing_first_batch");

        replay::WalTailer tailer;
        check(tailer.open(wal.dir), "growing_open");
        const auto first = drain(tailer);
        check(first.count == 500, "growing_first_read");

        // Nothing new yet: must report idle, not re-deliver.
        core::FixedEvent event{};
        check(tailer.next(event) == replay::TailStatus::idle, "growing_idle_when_no_new_data");

        check(wal.append(750), "growing_second_batch");
        const auto second = drain(tailer);
        check(second.count == 750, "growing_second_read_only_new");
        check(tailer.total_events() == 1'250, "growing_total_no_duplicates");

        // Sequence continues without gap or repeat across the two reads.
        bool contiguous = second.sequence.size() == 750;
        for (std::size_t i = 0; contiguous && i < second.sequence.size(); ++i)
            if (second.sequence[i] != 500 + i + 1) contiguous = false;
        check(contiguous, "growing_sequence_contiguous_across_reads");

        wal.writer.close();
        replay_fixture::remove_all(wal.dir);
    }

    // ---- 5. preallocated but unwritten space --------------------------------
    {
        GrowingWal wal;
        check(wal.start("tail_prealloc", 1 << 20), "prealloc_start");
        check(wal.append(100), "prealloc_write");

        // The segment file is preallocated far beyond the committed data; the
        // tailer must stop at the boundary and never interpret zero-fill.
        const auto physical = std::filesystem::file_size(wal.writer.current_path());
        check(physical > 100 * 140, "prealloc_file_larger_than_data");

        replay::WalTailer tailer;
        check(tailer.open(wal.dir), "prealloc_open");
        const auto d = drain(tailer);
        check(d.count == 100, "prealloc_reads_only_committed");
        check(!tailer.corrupt(), "prealloc_zero_fill_is_clean_stop");

        wal.writer.close();
        replay_fixture::remove_all(wal.dir);
    }

    // ---- 6. partial / corrupt final record ----------------------------------
    {
        const auto dir = replay_fixture::make_wal("tail_partial", 500, 1 << 20);
        // Corrupt a byte inside a committed frame.
        std::filesystem::path segment;
        for (const auto& entry : std::filesystem::directory_iterator(dir))
            if (entry.path().extension() == ".wal") segment = entry.path();
        check(!segment.empty(), "partial_segment_found");
        {
            std::fstream file(segment, std::ios::binary | std::ios::in | std::ios::out);
            file.seekg(256 + 600);
            char value = 0;
            file.read(&value, 1);
            value = static_cast<char>(value ^ 1);
            file.seekp(256 + 600);
            file.write(&value, 1);
        }

        replay::WalTailer tailer;
        check(tailer.open(dir), "partial_open");
        const auto d = drain(tailer);
        check(d.terminal == replay::TailStatus::corrupt, "partial_reports_corrupt");
        check(tailer.corrupt(), "partial_corrupt_latched");
        check(d.count < 500, "partial_stopped_before_end");

        // Corruption is terminal: it must not clear itself on the next call.
        core::FixedEvent event{};
        check(tailer.next(event) == replay::TailStatus::corrupt, "partial_corrupt_is_terminal");
        replay_fixture::remove_all(dir);
    }

    // ---- 7. restart from a persisted cursor ---------------------------------
    {
        const auto dir = replay_fixture::make_wal("tail_restart", 3'000, 32 * 1024);
        const auto cursor_path = dir / "state" / "paper_cursor.txt";

        replay::WalTailer first;
        check(first.open(dir), "restart_first_open");
        core::FixedEvent event{};
        std::vector<std::uint64_t> before;
        for (int i = 0; i < 1'000; ++i) {
            if (first.next(event) != replay::TailStatus::ok) break;
            before.push_back(event.header.seq_global);
        }
        check(before.size() == 1'000, "restart_partial_consume");
        check(first.save_cursor(cursor_path), "restart_cursor_saved");

        replay::TailCursor restored{};
        check(replay::WalTailer::load_cursor(cursor_path, restored), "restart_cursor_loaded");
        check(restored == first.cursor(), "restart_cursor_round_trip");

        replay::WalTailer second;
        check(second.open(dir), "restart_second_open");
        check(second.resume(restored), "restart_resume");
        const auto after = drain(second);
        check(after.count == 2'000, "restart_reads_exact_remainder");

        // No overlap and no gap at the seam.
        check(after.sequence.front() == before.back() + 1, "restart_resumes_at_next_event");
        replay_fixture::remove_all(dir);
    }

    // ---- 8. duplicate prevention at exhaustion ------------------------------
    {
        const auto dir = replay_fixture::make_wal("tail_dupe", 400, 1 << 20);
        replay::WalTailer tailer;
        check(tailer.open(dir), "dupe_open");
        const auto d = drain(tailer);
        check(d.count == 400, "dupe_first_drain");

        // Repeated polling at the end must never re-deliver.
        core::FixedEvent event{};
        for (int i = 0; i < 20; ++i) (void)tailer.next(event);
        check(tailer.total_events() == 400, "dupe_no_redelivery_at_end");
        replay_fixture::remove_all(dir);
    }

    return failures == 0 ? 0 : 1;
}
