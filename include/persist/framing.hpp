#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace persist {

inline constexpr std::uint32_t wal_format_version = 1;
inline constexpr std::size_t wal_header_size = 256;
inline constexpr std::size_t frame_prefix_size = 8;
inline constexpr std::size_t frame_crc_size = 4;
inline constexpr std::size_t fixed_event_payload_size = 128;
inline constexpr std::size_t max_wal_payload_size = 1040;
inline constexpr std::size_t max_frame_size = frame_prefix_size + max_wal_payload_size + frame_crc_size;

#pragma pack(push, 1)
struct FramePrefix final {
    std::uint32_t payload_length;
    std::uint16_t type;
    std::uint16_t flags;
};

struct WalFileHeader final {
    std::array<char, 8> magic;
    std::uint16_t format_version;
    std::uint16_t header_size;
    std::uint32_t byte_order_marker;
    std::uint32_t flags;
    std::array<std::uint8_t, 16> run_id;
    std::uint64_t run_seed;
    std::uint64_t session_epoch;
    std::array<std::uint8_t, 32> effective_limits_hash;
    std::array<std::uint8_t, 32> params_hash;
    std::array<std::uint8_t, 32> build_hash;
    std::array<char, 32> toolchain_version;
    std::array<std::uint8_t, 64> clock_calibration_record;
    std::array<std::uint8_t, 8> reserved;
    std::uint32_t header_crc32;
};
#pragma pack(pop)

static_assert(sizeof(FramePrefix) == 8);
static_assert(sizeof(WalFileHeader) == 256);
static_assert(offsetof(WalFileHeader, magic) == 0);
static_assert(offsetof(WalFileHeader, format_version) == 8);
static_assert(offsetof(WalFileHeader, header_size) == 10);
static_assert(offsetof(WalFileHeader, byte_order_marker) == 12);
static_assert(offsetof(WalFileHeader, flags) == 16);
static_assert(offsetof(WalFileHeader, run_id) == 20);
static_assert(offsetof(WalFileHeader, run_seed) == 36);
static_assert(offsetof(WalFileHeader, session_epoch) == 44);
static_assert(offsetof(WalFileHeader, effective_limits_hash) == 52);
static_assert(offsetof(WalFileHeader, params_hash) == 84);
static_assert(offsetof(WalFileHeader, build_hash) == 116);
static_assert(offsetof(WalFileHeader, toolchain_version) == 148);
static_assert(offsetof(WalFileHeader, clock_calibration_record) == 180);
static_assert(offsetof(WalFileHeader, reserved) == 244);
static_assert(offsetof(WalFileHeader, header_crc32) == 252);
static_assert(std::is_trivially_copyable_v<WalFileHeader>);
static_assert(frame_prefix_size + fixed_event_payload_size + frame_crc_size == 140);

namespace detail {
constexpr std::uint32_t crc_entry(std::uint32_t value) noexcept {
    for (int bit = 0; bit < 8; ++bit)
        value = (value >> 1U) ^ ((value & 1U) ? 0xedb88320U : 0U);
    return value;
}
constexpr std::array<std::uint32_t, 256> make_crc_table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) table[i] = crc_entry(static_cast<std::uint32_t>(i));
    return table;
}
inline constexpr auto crc_table = make_crc_table();
}  // namespace detail

[[nodiscard]] inline std::uint32_t crc32(const void* data, std::size_t size,
                                         std::uint32_t seed = 0) noexcept {
    auto crc = seed ^ 0xffffffffU;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i)
        crc = detail::crc_table[(crc ^ bytes[i]) & 0xffU] ^ (crc >> 8U);
    return crc ^ 0xffffffffU;
}

[[nodiscard]] inline bool valid_payload_length(std::uint32_t length) noexcept {
    return length > 0 && length <= max_wal_payload_size;
}

[[nodiscard]] inline std::size_t encode_frame(std::byte* destination,
    std::size_t capacity, std::uint16_t type, std::uint16_t flags,
    const void* payload, std::uint32_t payload_length) noexcept {
    if (destination == nullptr || payload == nullptr || !valid_payload_length(payload_length)) return 0;
    const std::size_t total = frame_prefix_size + payload_length + frame_crc_size;
    if (capacity < total) return 0;
    const FramePrefix prefix{payload_length, type, flags};
    std::memcpy(destination, &prefix, sizeof(prefix));
    std::memcpy(destination + sizeof(prefix), payload, payload_length);
    const auto checksum = crc32(destination, sizeof(prefix) + payload_length);
    std::memcpy(destination + sizeof(prefix) + payload_length, &checksum, sizeof(checksum));
    return total;
}

[[nodiscard]] inline WalFileHeader finalize_header(WalFileHeader header) noexcept {
    header.magic = {'M','M','E','W','A','L','0','1'};
    header.format_version = wal_format_version;
    header.header_size = static_cast<std::uint16_t>(wal_header_size);
    header.byte_order_marker = 0x01020304U;
    header.reserved.fill(0);
    header.header_crc32 = crc32(&header, offsetof(WalFileHeader, header_crc32));
    return header;
}

[[nodiscard]] inline bool valid_header(const WalFileHeader& header) noexcept {
    constexpr std::array<char, 8> magic{'M','M','E','W','A','L','0','1'};
    return header.magic == magic && header.format_version == wal_format_version &&
        header.header_size == wal_header_size && header.byte_order_marker == 0x01020304U &&
        crc32(&header, offsetof(WalFileHeader, header_crc32)) == header.header_crc32;
}

}  // namespace persist
