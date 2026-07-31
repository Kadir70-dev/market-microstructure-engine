#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>

#include "feed/book_source.hpp"
#include "feed/clock_offset.hpp"
#include "feed/heartbeat_monitor.hpp"
#include "feed/mt5_pipe_adapter.hpp"
#include "feed/sequence_tracker.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

int main() {
    using namespace feed;
    SequenceTracker sequence;
    if (sequence.observe(100).status != SequenceStatus::synchronized) return 1;
    if (sequence.observe(101).status != SequenceStatus::in_order) return 1;
    const auto gap = sequence.observe(104);
    if (gap.status != SequenceStatus::gap || gap.expected != 102 || gap.lost != 2) return 1;
    if (sequence.observe(104).status != SequenceStatus::stale_or_duplicate) return 1;

    HeartbeatMonitor heartbeat(1'000, 2);
    if (heartbeat.observe(100, true) != HeartbeatState::recovering) return 1;
    if (heartbeat.observe(200, true) != HeartbeatState::healthy) return 1;
    if (heartbeat.poll(1'201) != HeartbeatState::stale) return 1;
    if (heartbeat.observe(1'300, false) != HeartbeatState::stale) return 1;

    ClockOffsetEstimator clock;
    clock.observe(1'000, 1'120);
    clock.observe(2'000, 2'080);
    if (clock.offset_ns() != 80 || clock.correct(3'000) != 3'080) return 1;

    if (classify_book_source(false, false, false, false) != BookSource::l1_only) return 1;
    if (classify_book_source(true, false, false, false) != BookSource::dom_aggregated) return 1;
    if (classify_book_source(true, false, false, true) != BookSource::dom_synthetic) return 1;
    if (classify_book_source(true, true, false, false) != BookSource::l2_exchange) return 1;
    if (classify_book_source(true, true, true, false) != BookSource::l3_mbo) return 1;

    Mt5PipeAdapter adapter(77, 4, 3'000'000'000ULL);
    mt5::MdRecord md{};
    md.prefix = {mt5::protocol_magic, mt5::protocol_version,
        static_cast<std::uint16_t>(mt5::RecordType::market_data), 77, 1};
    md.market_data_type = static_cast<std::uint16_t>(mt5::MarketDataType::quote);
    md.symbol_id = 2;
    md.ts_broker_ms = 1'000;
    md.ts_terminal_ms = 1'001;
    md.bid_ticks = 100; md.ask_ticks = 102; md.bid_volume = 3; md.ask_volume = 4;
    auto result = adapter.ingest(md, 5'000, 1'001'000'100ULL);
    if (result.verdict != IngressVerdict::accepted || result.event.header.symbol_id != 2 ||
        result.event.header.ts_broker_ns != 1'000'000'000ULL) return 1;
    core::QuotePayload quote{};
    std::memcpy(&quote, result.event.payload.data(), sizeof(quote));
    if (quote.bid != 100 || quote.ask != 102 || quote.bid_size != 3 || quote.ask_size != 4) return 1;

    md.prefix.sequence = 3;
    if (adapter.ingest(md, 6'000, 1'001'000'200ULL).verdict != IngressVerdict::sequence_gap) return 1;
    md.prefix.sequence = 4; md.prefix.session_epoch = 76;
    if (adapter.ingest(md, 7'000, 1'001'000'300ULL).verdict != IngressVerdict::stale_epoch ||
        adapter.expired_on_restart() != 1) return 1;

    mt5::HbRecord hb{};
    hb.prefix = {mt5::protocol_magic, mt5::protocol_version,
        static_cast<std::uint16_t>(mt5::RecordType::heartbeat), 77, 4};
    hb.ts_terminal_ms = 1'002; hb.connected = 1; hb.book_source[0] = 0;
    if (adapter.ingest(hb, 10'000, 1'002'000'100ULL).verdict != IngressVerdict::accepted) return 1;
    hb.prefix.sequence = 5;
    if (adapter.ingest(hb, 11'000, 1'002'000'200ULL).verdict != IngressVerdict::accepted) return 1;
    hb.prefix.sequence = 6;
    if (adapter.ingest(hb, 12'000, 1'002'000'300ULL).verdict != IngressVerdict::accepted) return 1;
    if (adapter.heartbeat_state(13'000) != HeartbeatState::healthy) return 1;

#if defined(_WIN32)
    const char* pipe_name = "\\\\.\\pipe\\mme_phase2b_feed_test";
    std::array<std::uint8_t, 32> server_hash{}; server_hash[0] = 9;
    mt5::HelloRecord hello{};
    hello.prefix = {mt5::protocol_magic, mt5::protocol_version,
        static_cast<std::uint16_t>(mt5::RecordType::hello), 55, 1};
    hello.account_expected = 1234;
    bool client_ok = false;
    std::thread client([&] {
        while (!WaitNamedPipeA(pipe_name, 1000)) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) return;
        }
        HANDLE pipe = CreateFileA(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return;
        mt5::HelloRecord received{}; DWORD done = 0;
        if (!ReadFile(pipe, &received, sizeof(received), &done, nullptr) || done != sizeof(received)) {
            CloseHandle(pipe); return;
        }
        mt5::HelloAckRecord ack{};
        ack.prefix = {mt5::protocol_magic, mt5::protocol_version,
            static_cast<std::uint16_t>(mt5::RecordType::hello_ack), 55, 2};
        ack.account_actual = 1234; ack.server_hash = server_hash;
        ack.account_margin_mode = static_cast<std::uint32_t>(mt5::MarginMode::hedging);
        if (!WriteFile(pipe, &ack, sizeof(ack), &done, nullptr) || done != sizeof(ack)) {
            CloseHandle(pipe); return;
        }
        mt5::MdRecord wire_md = md;
        wire_md.prefix.session_epoch = 55; wire_md.prefix.sequence = 3;
        client_ok = WriteFile(pipe, &wire_md, sizeof(wire_md), &done, nullptr) && done == sizeof(wire_md);
        CloseHandle(pipe);
    });
    NamedPipeEndpoint endpoint;
    if (!endpoint.accept(pipe_name)) { client.join(); return 1; }
    mt5::HelloAckRecord ack{};
    if (endpoint.handshake(hello, ack, server_hash, mt5::MarginMode::hedging) !=
        mt5::HandshakeVerdict::accept) { client.join(); return 1; }
    WireRecord wire{};
    if (!endpoint.read_next(wire) || wire.type != mt5::RecordType::market_data ||
        wire.market_data.bid_ticks != md.bid_ticks) { client.join(); return 1; }
    client.join();
    if (!client_ok) return 1;
#endif

    std::cout << "phase2b_feed_components=pass\n";
    return 0;
}
