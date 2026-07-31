#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

#include "core/event.hpp"
#include "persist/wal_writer.hpp"

// Shared Phase 4 fixture. WALs are produced by the real WalWriter, so replay
// tests read recorder-format files rather than hand-rolled bytes — the same
// discipline the Phase 2c reader tests already follow.

namespace replay_fixture {

inline std::filesystem::path unique_dir(const char* label) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string("mme_replay_") + label + "_" + std::to_string(stamp));
}

// Writes `records` quote events shaped exactly as Mt5PipeAdapter::ingest emits
// them, rotating whenever the writer asks so the multi-segment advance path is
// genuinely exercised. Prices walk deterministically and never cross.
inline std::filesystem::path make_wal(const char* label, int records,
                                      std::uint64_t segment_bytes = 1 << 20,
                                      std::uint64_t timestamp_step_ns = 1'000) {
    const auto dir = unique_dir(label);
    persist::WalConfig config{dir};
    config.segment_data_bytes = segment_bytes;

    persist::WalWriter writer;
    if (!writer.open(config, {}, 0)) return {};

    core::FixedEvent event{};
    for (int i = 0; i < records; ++i) {
        const auto n = static_cast<std::uint64_t>(i);
        event.header = core::EventHeader{};
        event.header.seq_global = n + 1;
        event.header.ts_local_ns = 1'000'000ULL + n * timestamp_step_ns;
        event.header.ts_broker_ns = 2'000'000ULL + n * timestamp_step_ns;
        event.header.ts_terminal_ns = 3'000'000ULL + n * timestamp_step_ns;
        event.header.seq_source = static_cast<std::uint32_t>(i + 1);
        event.header.symbol_id = static_cast<std::uint32_t>(i % 5);
        event.header.source_id = 1;
        event.header.source_priority = 0;
        event.header.type = static_cast<std::uint16_t>(core::EventType::quote);

        const std::int64_t drift = static_cast<std::int64_t>(i % 17);
        const core::QuotePayload payload{100'000 + drift, 100'010 + drift,
                                         1 + (i % 7), 1 + (i % 5), 0};
        std::memcpy(event.payload.data(), &payload, sizeof(payload));

        std::filesystem::path closed;
        if (writer.needs_rotation(n, sizeof(event)) && !writer.rotate(n, closed)) return {};
        if (writer.append(0, 0, &event, sizeof(event)) != persist::AppendResult::committed)
            return {};
    }
    std::filesystem::path closed;
    if (!writer.finalize(closed)) return {};
    writer.close();
    return dir;
}

inline void remove_all(const std::filesystem::path& dir) noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

}  // namespace replay_fixture
