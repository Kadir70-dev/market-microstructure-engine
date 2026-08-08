#include <chrono>
#include <filesystem>
#include <string>

#include "persist/execution_wal.hpp"
#include "phase6_test.hpp"

int main() {
    Phase6Test t;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("mme_exec_wal_smoke_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);

    persist::WalConfig config{};
    config.directory = dir;
    config.segment_data_bytes = 1024 * 1024;  // small, must be >= max_frame_size
    persist::ExecutionWalWriter writer;
    persist::WalFileHeader header{};
    t.check(writer.open(config, header, 0), "writer open");

    exec::JournalRecord r1{};
    r1.type = static_cast<std::uint16_t>(exec::JournalRecordType::command);
    r1.run_id = 1; r1.logical_order_id = 42; r1.b = 10;
    t.check(writer.append(r1, 0), "append record 1");
    t.check(writer.flush(), "flush after record 1");

    // Read while the writer still holds the segment open -- this is the
    // whole point of the Windows share-mode fix.
    persist::ExecutionWalTailer tailer;
    t.check(tailer.open(dir), "tailer open while writer live");
    exec::JournalRecord out{};
    auto status = tailer.next(out);
    t.check(status == persist::ExecTailStatus::ok, "tailer reads record 1 while segment is growing");
    t.check(out.logical_order_id == 42 && out.b == 10, "record 1 content correct");

    status = tailer.next(out);
    t.check(status == persist::ExecTailStatus::idle, "tailer idle with nothing new");

    exec::JournalRecord r2{};
    r2.type = static_cast<std::uint16_t>(exec::JournalRecordType::fill);
    r2.run_id = 1; r2.logical_order_id = 42; r2.a = 5;
    t.check(writer.append(r2, 1), "append record 2");
    t.check(writer.flush(), "flush after record 2");

    status = tailer.next(out);
    t.check(status == persist::ExecTailStatus::ok, "tailer reads record 2 after it's flushed");
    t.check(out.logical_order_id == 42 && out.a == 5, "record 2 content correct");

    writer.close();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return t.result();
}
