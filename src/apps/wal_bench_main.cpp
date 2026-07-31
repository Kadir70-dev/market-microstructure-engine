#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>

#include "core/event.hpp"
#include "persist/wal_writer.hpp"

int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;
    constexpr std::size_t samples = 100000;
    const auto directory = argc > 1 ? std::filesystem::path(argv[1]) :
        std::filesystem::temp_directory_path() / "mme_wal_benchmark";
    persist::WalConfig config{directory};
    config.segment_data_bytes = 32ULL * 1024ULL * 1024ULL;
    config.flush_interval = std::chrono::milliseconds{100};
    persist::WalFileHeader header{};
    persist::WalWriter writer;
    if (!writer.open(config, header, 0)) return 1;
    core::FixedEvent event{};
    event.header.type = static_cast<std::uint16_t>(core::EventType::quote);
    std::array<double, samples> latency_us{};
    const auto all_start = Clock::now();
    for (std::size_t i = 0; i < samples; ++i) {
        event.header.seq_global = i + 1;
        const auto start = Clock::now();
        const auto result = writer.append(event.header.type, event.header.flags, &event, sizeof(event));
        const auto end = Clock::now();
        if (result != persist::AppendResult::committed) return 1;
        latency_us[i] = std::chrono::duration<double, std::micro>(end - start).count();
    }
    const auto elapsed = std::chrono::duration<double>(Clock::now() - all_start).count();
    std::sort(latency_us.begin(), latency_us.end());
    const auto percentile = [&](double q) { return latency_us[static_cast<std::size_t>(q * (samples - 1))]; };
    const auto p99 = percentile(0.99);
    std::cout << "samples=" << samples
              << " throughput_events_s=" << static_cast<double>(samples) / elapsed
              << " p50_us=" << percentile(0.50)
              << " p95_us=" << percentile(0.95)
              << " p99_us=" << p99
              << " segment_bytes=" << config.segment_data_bytes
              << " flush_interval_ms=" << config.flush_interval.count()
#if defined(_WIN32)
              << " os=windows filesystem=ntfs_or_host_configured\n";
#else
              << " os=linux filesystem=host_configured\n";
#endif
    writer.close();
    return p99 < 5.0 && static_cast<double>(samples) / elapsed >= 100000.0 ? 0 : 1;
}
