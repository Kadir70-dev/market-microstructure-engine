#pragma once

#include <array>
#include <cstdint>

#include "core/clock.hpp"
#include "core/event.hpp"
#include "feed/book_source.hpp"
#include "feed/clock_offset.hpp"
#include "feed/feed_adapter.hpp"
#include "feed/heartbeat_monitor.hpp"
#include "feed/mt5_protocol.hpp"
#include "feed/sequence_tracker.hpp"

namespace feed {

enum class IngressVerdict : std::uint8_t {
    accepted, bad_magic, bad_version, stale_epoch, wrong_type, sequence_gap,
    stale_sequence, disconnected
};

struct IngressResult final {
    IngressVerdict verdict{IngressVerdict::accepted};
    core::FixedEvent event{};
};

struct WireRecord final {
    mt5::RecordType type{mt5::RecordType::market_data};
    mt5::MdRecord market_data{};
    mt5::HbRecord heartbeat{};
};

class NamedPipeEndpoint final {
public:
    NamedPipeEndpoint() noexcept = default;
    ~NamedPipeEndpoint() noexcept;
    NamedPipeEndpoint(const NamedPipeEndpoint&) = delete;
    NamedPipeEndpoint& operator=(const NamedPipeEndpoint&) = delete;

    [[nodiscard]] bool accept(const char* pipe_name) noexcept;
    [[nodiscard]] mt5::HandshakeVerdict handshake(
        const mt5::HelloRecord& hello, mt5::HelloAckRecord& ack,
        const std::array<std::uint8_t, 32>& expected_server_hash,
        mt5::MarginMode expected_margin_mode) noexcept;
    [[nodiscard]] bool read_next(WireRecord& record) noexcept;
    void close() noexcept;
    [[nodiscard]] static constexpr bool supported() noexcept {
#if defined(_WIN32)
        return true;
#else
        return false;
#endif
    }

private:
    void* handle_{nullptr};
};

class Mt5PipeAdapter final : public IFeedAdapter {
public:
    Mt5PipeAdapter(std::uint64_t session_epoch, std::uint16_t source_id,
                   std::uint64_t heartbeat_timeout_ns) noexcept;

    [[nodiscard]] IngressResult ingest(const mt5::MdRecord& record,
        std::uint64_t local_monotonic_ns, std::uint64_t local_utc_ns) noexcept;
    [[nodiscard]] IngressResult ingest(const mt5::HbRecord& record,
        std::uint64_t local_monotonic_ns, std::uint64_t local_utc_ns) noexcept;
    [[nodiscard]] HeartbeatState heartbeat_state(std::uint64_t now_ns) noexcept;
    [[nodiscard]] std::uint64_t expired_on_restart() const noexcept { return expired_; }
    [[nodiscard]] std::uint64_t sequence_gaps() const noexcept { return sequence_.gaps(); }
    [[nodiscard]] std::int64_t clock_offset_ns() const noexcept { return clock_.offset_ns(); }

    // ---- IFeedAdapter -----------------------------------------------------
    // Binding is separate from construction so that the existing constructor,
    // ingest() surface and every current call site are unchanged. record_main
    // keeps its explicit read/ingest loop; poll() is the same normalisation
    // reached through the shared seam, and both delegate to ingest().
    void bind(NamedPipeEndpoint& endpoint, core::IClock& clock) noexcept;

    [[nodiscard]] PollResult poll(core::FixedEvent& out) noexcept override;
    [[nodiscard]] core::IClock& clock() noexcept override;
    [[nodiscard]] IngressVerdict last_verdict() const noexcept { return last_verdict_; }

private:
    [[nodiscard]] IngressVerdict validate_prefix(const mt5::RecordPrefix&, mt5::RecordType) noexcept;
    std::uint64_t epoch_;
    std::uint64_t seq_global_{0};
    std::uint64_t expired_{0};
    std::uint16_t source_id_;
    SequenceTracker sequence_{};
    HeartbeatMonitor heartbeat_;
    ClockOffsetEstimator clock_{};
    NamedPipeEndpoint* endpoint_{nullptr};
    core::IClock* clock_source_{nullptr};
    IngressVerdict last_verdict_{IngressVerdict::accepted};
};

}  // namespace feed
