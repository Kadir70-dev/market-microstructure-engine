#include "feed/mt5_pipe_adapter.hpp"

#include <chrono>
#include <cstring>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace {
#if defined(_WIN32)
bool read_exact(HANDLE handle, void* data, DWORD size) noexcept {
    auto* out = static_cast<unsigned char*>(data);
    DWORD total = 0;
    while (total < size) {
        DWORD done = 0;
        if (!ReadFile(handle, out + total, size - total, &done, nullptr) || done == 0) return false;
        total += done;
    }
    return true;
}
bool write_exact(HANDLE handle, const void* data, DWORD size) noexcept {
    const auto* in = static_cast<const unsigned char*>(data);
    DWORD total = 0;
    while (total < size) {
        DWORD done = 0;
        if (!WriteFile(handle, in + total, size - total, &done, nullptr) || done == 0) return false;
        total += done;
    }
    return true;
}
#endif
}  // namespace

namespace feed {

NamedPipeEndpoint::~NamedPipeEndpoint() noexcept { close(); }

bool NamedPipeEndpoint::accept(const char* pipe_name) noexcept {
#if defined(_WIN32)
    close();
    if (pipe_name == nullptr) return false;
    HANDLE handle = CreateNamedPipeA(pipe_name, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, sizeof(mt5::HbRecord) * 4,
        sizeof(mt5::HbRecord) * 4, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    if (!ConnectNamedPipe(handle, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
        CloseHandle(handle);
        return false;
    }
    handle_ = handle;
    return true;
#else
    (void)pipe_name;
    return false;
#endif
}

mt5::HandshakeVerdict NamedPipeEndpoint::handshake(
    const mt5::HelloRecord& hello, mt5::HelloAckRecord& ack,
    const std::array<std::uint8_t, 32>& expected_server_hash,
    mt5::MarginMode expected_margin_mode) noexcept {
#if defined(_WIN32)
    const auto handle = static_cast<HANDLE>(handle_);
    if (handle == nullptr || !write_exact(handle, &hello, sizeof(hello)) ||
        !read_exact(handle, &ack, sizeof(ack))) return mt5::HandshakeVerdict::bad_magic;
    return mt5::validate_handshake(hello, ack, expected_server_hash, expected_margin_mode);
#else
    (void)hello; (void)ack; (void)expected_server_hash; (void)expected_margin_mode;
    return mt5::HandshakeVerdict::bad_magic;
#endif
}

bool NamedPipeEndpoint::read_next(WireRecord& record) noexcept {
#if defined(_WIN32)
    const auto handle = static_cast<HANDLE>(handle_);
    mt5::RecordPrefix prefix{};
    if (handle == nullptr || !read_exact(handle, &prefix, sizeof(prefix))) return false;
    record.type = static_cast<mt5::RecordType>(prefix.type);
    if (record.type == mt5::RecordType::market_data) {
        record.market_data.prefix = prefix;
        return read_exact(handle, reinterpret_cast<unsigned char*>(&record.market_data) + sizeof(prefix),
                          sizeof(record.market_data) - sizeof(prefix));
    }
    if (record.type == mt5::RecordType::heartbeat) {
        record.heartbeat.prefix = prefix;
        return read_exact(handle, reinterpret_cast<unsigned char*>(&record.heartbeat) + sizeof(prefix),
                          sizeof(record.heartbeat) - sizeof(prefix));
    }
    return false;
#else
    (void)record;
    return false;
#endif
}

void NamedPipeEndpoint::close() noexcept {
#if defined(_WIN32)
    const auto handle = static_cast<HANDLE>(handle_);
    if (handle != nullptr) {
        FlushFileBuffers(handle);
        DisconnectNamedPipe(handle);
        CloseHandle(handle);
        handle_ = nullptr;
    }
#endif
}

Mt5PipeAdapter::Mt5PipeAdapter(std::uint64_t session_epoch, std::uint16_t source_id,
                               std::uint64_t heartbeat_timeout_ns) noexcept
    : epoch_(session_epoch), source_id_(source_id), heartbeat_(heartbeat_timeout_ns) {}

IngressVerdict Mt5PipeAdapter::validate_prefix(const mt5::RecordPrefix& prefix,
                                                mt5::RecordType type) noexcept {
    if (prefix.magic != mt5::protocol_magic) return IngressVerdict::bad_magic;
    if (prefix.version != mt5::protocol_version) return IngressVerdict::bad_version;
    if (prefix.session_epoch != epoch_) { ++expired_; return IngressVerdict::stale_epoch; }
    if (prefix.type != static_cast<std::uint16_t>(type)) return IngressVerdict::wrong_type;
    const auto sequence = sequence_.observe(prefix.sequence);
    if (sequence.status == SequenceStatus::gap) return IngressVerdict::sequence_gap;
    if (sequence.status == SequenceStatus::stale_or_duplicate) return IngressVerdict::stale_sequence;
    return IngressVerdict::accepted;
}

IngressResult Mt5PipeAdapter::ingest(const mt5::MdRecord& record,
    std::uint64_t local_monotonic_ns, std::uint64_t local_utc_ns) noexcept {
    IngressResult result{};
    result.verdict = validate_prefix(record.prefix, mt5::RecordType::market_data);
    if (result.verdict != IngressVerdict::accepted) return result;
    clock_.observe(record.ts_terminal_ms * 1000000ULL, local_utc_ns);
    auto& header = result.event.header;
    header.seq_global = ++seq_global_;
    header.ts_broker_ns = record.ts_broker_ms * 1000000ULL;
    header.ts_terminal_ns = clock_.correct(record.ts_terminal_ms * 1000000ULL);
    header.ts_local_ns = local_monotonic_ns;
    header.seq_source = static_cast<std::uint32_t>(record.prefix.sequence);
    header.symbol_id = record.symbol_id;
    header.source_id = source_id_;
    header.type = static_cast<std::uint16_t>(core::EventType::quote);
    core::QuotePayload payload{record.bid_ticks, record.ask_ticks,
                               record.bid_volume, record.ask_volume, record.flags};
    std::memcpy(result.event.payload.data(), &payload, sizeof(payload));
    return result;
}

IngressResult Mt5PipeAdapter::ingest(const mt5::HbRecord& record,
    std::uint64_t local_monotonic_ns, std::uint64_t local_utc_ns) noexcept {
    IngressResult result{};
    result.verdict = validate_prefix(record.prefix, mt5::RecordType::heartbeat);
    if (result.verdict != IngressVerdict::accepted) return result;
    clock_.observe(record.ts_terminal_ms * 1000000ULL, local_utc_ns);
    const auto observed_state = heartbeat_.observe(local_monotonic_ns, record.connected != 0);
    (void)observed_state;
    if (record.connected == 0) result.verdict = IngressVerdict::disconnected;
    auto& header = result.event.header;
    header.seq_global = ++seq_global_;
    header.ts_terminal_ns = clock_.correct(record.ts_terminal_ms * 1000000ULL);
    header.ts_local_ns = local_monotonic_ns;
    header.seq_source = static_cast<std::uint32_t>(record.prefix.sequence);
    header.source_id = source_id_;
    header.type = static_cast<std::uint16_t>(core::EventType::heartbeat);
    core::HeartbeatPayload payload{source_id_, header.ts_terminal_ns, record.connected,
        record.trade_allowed, record.book_source[0], {}};
    std::memcpy(result.event.payload.data(), &payload, sizeof(payload));
    return result;
}

HeartbeatState Mt5PipeAdapter::heartbeat_state(std::uint64_t now_ns) noexcept {
    return heartbeat_.poll(now_ns);
}

void Mt5PipeAdapter::bind(NamedPipeEndpoint& endpoint, core::IClock& clock) noexcept {
    endpoint_ = &endpoint;
    clock_source_ = &clock;
}

core::IClock& Mt5PipeAdapter::clock() noexcept { return *clock_source_; }

PollResult Mt5PipeAdapter::poll(core::FixedEvent& out) noexcept {
    if (endpoint_ == nullptr || clock_source_ == nullptr) return PollResult::error;
    WireRecord wire{};
    if (!endpoint_->read_next(wire)) return PollResult::end_of_stream;

    // A live feed is the one place a wall-clock read is correct: these are the
    // ingress timestamps being recorded, not a consumer asking what time it is.
    // Mirrors record_main's loop exactly so behaviour is unchanged.
    const auto local_ns = clock_source_->now_ns();
    const auto utc_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    IngressResult ingress{};
    if (wire.type == mt5::RecordType::market_data)
        ingress = ingest(wire.market_data, local_ns, utc_ns);
    else if (wire.type == mt5::RecordType::heartbeat)
        ingress = ingest(wire.heartbeat, local_ns, utc_ns);
    else
        return PollResult::idle;

    last_verdict_ = ingress.verdict;
    if (ingress.verdict != IngressVerdict::accepted &&
        ingress.verdict != IngressVerdict::disconnected) return PollResult::error;
    out = ingress.event;
    return PollResult::event;
}

}  // namespace feed
