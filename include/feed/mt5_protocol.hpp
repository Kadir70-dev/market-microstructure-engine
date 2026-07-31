#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace feed::mt5 {

inline constexpr std::uint32_t protocol_magic = 0x31454D4DU;  // "MME1" little-endian
inline constexpr std::uint16_t protocol_version = 1;
inline constexpr std::size_t symbol_count = 5;

enum class RecordType : std::uint16_t {
    hello = 1, hello_ack = 2, market_data = 3, heartbeat = 4,
    spike_message = 0x7fff
};
enum class MarketDataType : std::uint16_t { quote = 1, trade = 2 };
enum class MarginMode : std::uint32_t { netting = 0, exchange = 1, hedging = 2 };
enum class HandshakeVerdict : std::uint8_t {
    accept, bad_magic, bad_version, stale_epoch, account_mismatch,
    server_mismatch, margin_mode_mismatch
};

#pragma pack(push, 1)
struct RecordPrefix final {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t type;
    std::uint64_t session_epoch;
    std::uint64_t sequence;
};

struct HelloRecord final {
    RecordPrefix prefix;
    std::uint64_t account_expected;
    std::array<std::uint8_t, 32> symbol_set_hash;
};

struct HelloAckRecord final {
    RecordPrefix prefix;
    std::uint64_t account_actual;
    std::array<std::uint8_t, 32> server_hash;
    std::uint32_t account_margin_mode;
    std::array<std::uint8_t, symbol_count> book_source;
    std::array<std::uint8_t, 3> reserved;
    std::array<std::uint8_t, 32> symbol_meta_hash;
};

struct MdRecord final {
    RecordPrefix prefix;
    std::uint16_t market_data_type;
    std::uint16_t reserved0;
    std::uint32_t symbol_id;
    std::uint64_t ts_broker_ms;
    std::uint64_t ts_terminal_ms;
    std::int64_t bid_ticks;
    std::int64_t ask_ticks;
    std::int64_t last_ticks;
    std::int64_t bid_volume;
    std::int64_t ask_volume;
    std::int64_t tick_volume;
    std::uint32_t flags;
};

struct HbRecord final {
    RecordPrefix prefix;
    std::uint64_t ts_terminal_ms;
    std::uint64_t account;
    std::array<std::uint8_t, 32> server_hash;
    std::uint32_t trade_mode;
    std::uint32_t margin_mode;
    std::uint8_t connected;
    std::uint8_t trade_allowed;
    std::array<std::uint8_t, symbol_count> book_source;
    std::uint8_t reserved0;
    std::int64_t equity_minor;
    std::int64_t balance_minor;
    std::int64_t margin_minor;
    std::int64_t free_margin_minor;
    std::uint32_t account_currency_id;
    std::uint32_t engine_hb_age_ms;
};

struct SpikeMessage final {
    RecordPrefix prefix;
    std::uint64_t payload_sequence;
    std::array<std::uint8_t, 88> payload;
    std::uint64_t checksum;
};
#pragma pack(pop)

static_assert(sizeof(RecordPrefix) == 24);
static_assert(sizeof(HelloRecord) == 64);
static_assert(sizeof(HelloAckRecord) == 108);
static_assert(sizeof(MdRecord) == 100);
static_assert(sizeof(HbRecord) == 128);
static_assert(sizeof(SpikeMessage) == 128);
static_assert(offsetof(RecordPrefix, magic) == 0);
static_assert(offsetof(RecordPrefix, version) == 4);
static_assert(offsetof(RecordPrefix, type) == 6);
static_assert(offsetof(RecordPrefix, session_epoch) == 8);
static_assert(offsetof(RecordPrefix, sequence) == 16);
static_assert(offsetof(HelloAckRecord, account_actual) == 24);
static_assert(offsetof(HelloAckRecord, server_hash) == 32);
static_assert(offsetof(HelloAckRecord, account_margin_mode) == 64);
static_assert(offsetof(HelloAckRecord, book_source) == 68);
static_assert(offsetof(HelloAckRecord, symbol_meta_hash) == 76);
static_assert(offsetof(MdRecord, symbol_id) == 28);
static_assert(offsetof(MdRecord, ts_broker_ms) == 32);
static_assert(offsetof(MdRecord, bid_ticks) == 48);
static_assert(offsetof(MdRecord, flags) == 96);
static_assert(offsetof(HbRecord, ts_terminal_ms) == 24);
static_assert(offsetof(HbRecord, book_source) == 82);
static_assert(offsetof(HbRecord, equity_minor) == 88);
static_assert(std::is_trivially_copyable_v<HelloRecord>);
static_assert(std::is_trivially_copyable_v<HelloAckRecord>);

[[nodiscard]] constexpr bool host_is_little_endian() noexcept {
    union Value { std::uint16_t number; std::uint8_t bytes[2]; };
    return Value{1}.bytes[0] == 1;
}

[[nodiscard]] inline bool hash_equal(const std::array<std::uint8_t, 32>& lhs,
                                     const std::array<std::uint8_t, 32>& rhs) noexcept {
    return std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

[[nodiscard]] inline HandshakeVerdict validate_handshake(
    const HelloRecord& hello, const HelloAckRecord& ack,
    const std::array<std::uint8_t, 32>& expected_server_hash,
    MarginMode expected_margin_mode) noexcept {
    if (hello.prefix.magic != protocol_magic || ack.prefix.magic != protocol_magic)
        return HandshakeVerdict::bad_magic;
    if (hello.prefix.version != protocol_version || ack.prefix.version != protocol_version)
        return HandshakeVerdict::bad_version;
    if (hello.prefix.session_epoch != ack.prefix.session_epoch)
        return HandshakeVerdict::stale_epoch;
    if (hello.account_expected != ack.account_actual)
        return HandshakeVerdict::account_mismatch;
    if (!hash_equal(ack.server_hash, expected_server_hash))
        return HandshakeVerdict::server_mismatch;
    if (ack.account_margin_mode != static_cast<std::uint32_t>(expected_margin_mode))
        return HandshakeVerdict::margin_mode_mismatch;
    return HandshakeVerdict::accept;
}

[[nodiscard]] inline std::uint64_t spike_checksum(const SpikeMessage& message) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&message);
    for (std::size_t i = 0; i < offsetof(SpikeMessage, checksum); ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

}  // namespace feed::mt5
