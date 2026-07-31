#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>

#include "core/clock.hpp"
#include "core/ring_buffer.hpp"
#include "feed/mt5_pipe_adapter.hpp"
#include "feed/recorder.hpp"
#include "telemetry/histogram.hpp"

namespace {
struct CompressionJob final { std::array<char, 512> path{}; };
static_assert(std::is_trivially_copyable_v<CompressionJob>);

bool parse_hash(const char* text, std::array<std::uint8_t, 32>& hash) noexcept {
    if (!text || std::strlen(text) != 64) return false;
    for (std::size_t i = 0; i < hash.size(); ++i) {
        unsigned value = 0;
        if (std::sscanf(text + i * 2, "%2x", &value) != 1) return false;
        hash[i] = static_cast<std::uint8_t>(value);
    }
    return true;
}
}

int main(int argc, char** argv) {
    if (argc != 9) {
        std::cerr << "usage: phase2c_recorder <pipe> <wal-dir> <account> <epoch> "
                     "<margin-mode> <server-hash-hex> <symbol-hash-hex> <run-seed>\n";
        return 2;
    }
    const auto account = std::strtoull(argv[3], nullptr, 10);
    const auto epoch = std::strtoull(argv[4], nullptr, 10);
    const auto margin = static_cast<feed::mt5::MarginMode>(std::strtoul(argv[5], nullptr, 10));
    const auto run_seed = std::strtoull(argv[8], nullptr, 10);
    std::array<std::uint8_t, 32> server_hash{}, symbol_hash{};
    if (!parse_hash(argv[6], server_hash) || !parse_hash(argv[7], symbol_hash)) return 2;

    persist::WalFileHeader wal_header{};
    wal_header.run_seed = run_seed;
    wal_header.session_epoch = epoch;
#if defined(_MSC_VER)
    std::snprintf(wal_header.toolchain_version.data(), wal_header.toolchain_version.size(),
                  "MSVC-%d", _MSC_VER);
#elif defined(__clang__)
    std::snprintf(wal_header.toolchain_version.data(), wal_header.toolchain_version.size(),
                  "Clang-%d", __clang_major__);
#elif defined(__GNUC__)
    std::snprintf(wal_header.toolchain_version.data(), wal_header.toolchain_version.size(),
                  "GCC-%d", __GNUC__);
#endif
    persist::WalConfig wal_config{std::filesystem::path(argv[2])};

    // Optional rotation override. The writer holds the ACTIVE segment with
    // CreateFileW(dwShareMode = 0), so no other process can open it; a reader
    // only ever sees SEALED segments. Rotation cadence is therefore the
    // visibility latency for anything downstream. Unset leaves the frozen
    // one-hour default (Part 11.2) untouched.
    if (const char* rotation_seconds = std::getenv("MME_ROTATION_SECONDS")) {
        const auto seconds = std::strtoull(rotation_seconds, nullptr, 10);
        if (seconds > 0) {
            wal_config.rotation_interval = std::chrono::seconds(static_cast<long long>(seconds));
            std::cout << "ROTATION_OVERRIDE seconds=" << seconds << '\n';
        }
    }
    std::error_code directory_error;
    std::filesystem::create_directories(wal_config.directory, directory_error);
    if (directory_error) return 1;
    std::filesystem::path recovery_candidate;
    std::uint64_t highest_segment = 0;
    bool found_segment = false;
    for (const auto& entry : std::filesystem::directory_iterator(wal_config.directory, directory_error)) {
        if (directory_error || !entry.is_regular_file() || entry.path().extension() != ".wal") continue;
        const auto stem = entry.path().stem().string();
        char* end = nullptr;
        const auto index = std::strtoull(stem.c_str(), &end, 10);
        if (end == stem.c_str() || *end != '\0') continue;
        if (!found_segment || index > highest_segment) {
            highest_segment = index;
            recovery_candidate = entry.path();
            found_segment = true;
        }
    }
    if (directory_error) return 1;
    if (found_segment) {
        const auto recovery = persist::WalWriter::recover(recovery_candidate);
        if (!recovery.success) {
            std::cerr << "RECOVERY_FAIL segment=" << recovery_candidate.string() << '\n';
            return 1;
        }
        std::cout << "RECOVERY_OK segment=" << recovery_candidate.string()
                  << " committed_boundary=" << recovery.committed_boundary
                  << " valid_boundary=" << recovery.valid_boundary
                  << " records=" << recovery.valid_records
                  << " truncated=" << (recovery.truncated ? 1 : 0) << '\n';
        std::cout.flush();
        wal_config.first_segment_index = highest_segment + 1;
        const auto compressed_path = std::filesystem::path(recovery_candidate.string() + ".zst");
        if (!std::filesystem::exists(compressed_path)) {
            std::filesystem::path output;
            if (!persist::WalWriter::compress_closed_segment(recovery_candidate, output)) {
                std::cerr << "RECOVERY_COMPRESSION_FAIL segment=" << recovery_candidate.string() << '\n';
                return 1;
            }
            std::cout << "RECOVERY_COMPRESSION_OK output=" << output.string() << '\n';
            std::cout.flush();
        }
    }
    persist::WalWriter writer;
    const auto start_ns = core::monotonic_now_ns();
    if (!writer.open(wal_config, wal_header, start_ns)) return 1;
    auto recorder = std::make_unique<feed::Recorder>(writer);

    core::RingBuffer<CompressionJob, 8> compression_queue;
    std::atomic<bool> stop_compressor{false}, compression_failed{false};
    std::thread compressor([&] {
        while (!stop_compressor.load(std::memory_order_acquire) || compression_queue.size() != 0) {
            CompressionJob job{};
            if (!compression_queue.try_pop(job)) { std::this_thread::yield(); continue; }
            std::filesystem::path output;
            if (!persist::WalWriter::compress_closed_segment(job.path.data(), output))
                compression_failed.store(true, std::memory_order_release);
        }
    });

    feed::NamedPipeEndpoint pipe;
    feed::mt5::HelloRecord hello{};
    hello.prefix = {feed::mt5::protocol_magic, feed::mt5::protocol_version,
        static_cast<std::uint16_t>(feed::mt5::RecordType::hello), epoch, 1};
    hello.account_expected = account;
    hello.symbol_set_hash = symbol_hash;
    feed::mt5::HelloAckRecord ack{};
    bool ok = pipe.accept(argv[1]) &&
        pipe.handshake(hello, ack, server_hash, margin) == feed::mt5::HandshakeVerdict::accept;
    if (!ok) {
        std::cerr << "HANDSHAKE_REJECT account_expected=" << account
                  << " epoch=" << epoch << " margin_mode_expected=" << argv[5] << '\n';
    } else {
        std::cout << "PIPE_CONNECTED transport=named_pipe pipe=" << argv[1] << '\n';
        std::cout << "HANDSHAKE_ACCEPT account=" << ack.account_actual
                  << " session_epoch=" << ack.prefix.session_epoch
                  << " account_mode=" << ack.account_margin_mode;
        for (std::size_t i = 0; i < ack.book_source.size(); ++i)
            std::cout << " book_source_" << i << '=' << static_cast<unsigned>(ack.book_source[i]);
        std::cout << '\n';
        std::cout.flush();
    }
    feed::Mt5PipeAdapter adapter(epoch, 1, 3'000'000'000ULL);
    auto last_flush_ns = start_ns;
    auto last_report_ns = start_ns;
    telemetry::Histogram<> ingress_latency_ns;
    std::array<std::uint64_t, feed::mt5::symbol_count> symbol_events{};
    std::uint64_t total_events = 0;
    while (ok && !compression_failed.load(std::memory_order_acquire)) {
        feed::WireRecord wire{};
        if (!pipe.read_next(wire)) break;
        const auto local_ns = core::monotonic_now_ns();
        const auto utc_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        feed::IngressResult ingress{};
        const auto normalize_start_ns = core::monotonic_now_ns();
        if (wire.type == feed::mt5::RecordType::market_data)
            ingress = adapter.ingest(wire.market_data, local_ns, utc_ns);
        else if (wire.type == feed::mt5::RecordType::heartbeat)
            ingress = adapter.ingest(wire.heartbeat, local_ns, utc_ns);
        else continue;
        ingress_latency_ns.record(core::monotonic_now_ns() - normalize_start_ns);
        if (ingress.verdict != feed::IngressVerdict::accepted &&
            ingress.verdict != feed::IngressVerdict::disconnected) {
            std::cerr << "INGRESS_HALT verdict=" << static_cast<unsigned>(ingress.verdict)
                      << " sequence_gaps=" << adapter.sequence_gaps()
                      << " expired_on_restart=" << adapter.expired_on_restart() << '\n';
            ok = false; break;
        }
        if (!recorder->submit(ingress.event)) {
            std::cerr << "RECORDER_HALT reason=wal_ring_saturation dropped_events=0\n";
            ok = false; break;
        }
        core::FixedEvent committed{};
        std::filesystem::path closed;
        const auto drained = recorder->drain_one(committed, local_ns, closed);
        if (drained == feed::DrainResult::halted) { ok = false; break; }
        ++total_events;
        if (committed.header.symbol_id < symbol_events.size() &&
            committed.header.type != static_cast<std::uint16_t>(core::EventType::heartbeat))
            ++symbol_events[committed.header.symbol_id];
        if (drained == feed::DrainResult::rotated) {
            CompressionJob job{};
            const auto text = closed.string();
            if (text.size() >= job.path.size()) { ok = false; break; }
            std::memcpy(job.path.data(), text.c_str(), text.size() + 1);
            if (!compression_queue.try_push(job)) { ok = false; break; }
        }
        const auto flush_period_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(writer.flush_interval()).count());
        if (local_ns - last_flush_ns >= flush_period_ns) {
            if (!writer.flush()) { ok = false; break; }
            last_flush_ns = local_ns;
        }
        if (local_ns - last_report_ns >= 60'000'000'000ULL) {
            std::cout << "CAPTURE_STATS total_events=" << total_events
                      << " ingress_p50_ns=" << ingress_latency_ns.percentile(0.50)
                      << " ingress_p95_ns=" << ingress_latency_ns.percentile(0.95)
                      << " ingress_p99_ns=" << ingress_latency_ns.percentile(0.99)
                      << " sequence_gaps=" << adapter.sequence_gaps()
                      << " dropped_events=0";
            for (std::size_t i = 0; i < symbol_events.size(); ++i)
                std::cout << " symbol_" << i << "_events=" << symbol_events[i];
            std::cout << '\n';
            std::cout.flush();
            last_report_ns = local_ns;
        }
        if (ingress.verdict == feed::IngressVerdict::disconnected) { ok = false; break; }
    }
    pipe.close();
    std::filesystem::path final_segment;
    if (!writer.finalize(final_segment)) ok = false;
    if (!final_segment.empty()) {
        CompressionJob job{};
        const auto text = final_segment.string();
        if (text.size() >= job.path.size()) ok = false;
        else {
            std::memcpy(job.path.data(), text.c_str(), text.size() + 1);
            while (!compression_queue.try_push(job) &&
                   !compression_failed.load(std::memory_order_acquire))
                std::this_thread::yield();
        }
    }
    stop_compressor.store(true, std::memory_order_release);
    compressor.join();
    std::cout << "CAPTURE_STOP total_events=" << total_events
              << " sequence_gaps=" << adapter.sequence_gaps()
              << " dropped_events=0 recorder_ok=" << (ok ? 1 : 0)
              << " compression_ok=" << (compression_failed.load(std::memory_order_acquire) ? 0 : 1)
              << '\n';
    std::cout.flush();
    return ok && !compression_failed.load(std::memory_order_acquire) ? 0 : 1;
}
