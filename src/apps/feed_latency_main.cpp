#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>

#include "feed/mt5_pipe_adapter.hpp"

int main() {
    using Clock = std::chrono::steady_clock;
    constexpr std::size_t samples = 100000;
    std::array<double, samples> latency_us{};
    feed::Mt5PipeAdapter adapter(9, 1, 3'000'000'000ULL);
    feed::mt5::MdRecord record{};
    record.prefix = {feed::mt5::protocol_magic, feed::mt5::protocol_version,
        static_cast<std::uint16_t>(feed::mt5::RecordType::market_data), 9, 0};
    record.market_data_type = static_cast<std::uint16_t>(feed::mt5::MarketDataType::quote);
    record.bid_ticks = 100; record.ask_ticks = 101;
    const auto all_start = Clock::now();
    for (std::size_t i = 0; i < samples; ++i) {
        record.prefix.sequence = i + 1;
        record.ts_broker_ms = i + 1;
        record.ts_terminal_ms = i + 1;
        const auto start = Clock::now();
        const auto result = adapter.ingest(record, i + 1, (i + 1) * 1000000ULL + 50);
        const auto stop = Clock::now();
        if (result.verdict != feed::IngressVerdict::accepted) return 1;
        latency_us[i] = std::chrono::duration<double, std::micro>(stop - start).count();
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - all_start).count();
    std::sort(latency_us.begin(), latency_us.end());
    const auto percentile = [&](double q) { return latency_us[static_cast<std::size_t>(q * (samples - 1))]; };
    std::cout << "samples=" << samples
              << " throughput_events_s=" << static_cast<double>(samples) / seconds
              << " p50_us=" << percentile(0.50)
              << " p95_us=" << percentile(0.95)
              << " p99_us=" << percentile(0.99) << '\n';
    return percentile(0.99) < 200.0 ? 0 : 1;
}
