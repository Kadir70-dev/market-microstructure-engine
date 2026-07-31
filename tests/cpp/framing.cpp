#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "core/event.hpp"
#include "persist/framing.hpp"

int main() {
    static_assert(sizeof(persist::WalFileHeader) == 256);
    static_assert(sizeof(persist::FramePrefix) == 8);
    static_assert(sizeof(core::FixedEvent) == 128);
    static_assert(8 + 128 + 4 == 140);
    core::FixedEvent event{};
    event.header.seq_global = 42;
    std::array<std::byte, persist::max_frame_size> frame{};
    const auto size = persist::encode_frame(frame.data(), frame.size(), 7, 3, &event, sizeof(event));
    if (size != 140) return 1;
    persist::FramePrefix prefix{};
    std::memcpy(&prefix, frame.data(), sizeof(prefix));
    if (prefix.payload_length != 128 || prefix.type != 7 || prefix.flags != 3) return 1;
    std::uint32_t stored = 0;
    std::memcpy(&stored, frame.data() + 136, sizeof(stored));
    if (stored != persist::crc32(frame.data(), 136)) return 1;
    frame[20] ^= std::byte{1};
    if (stored == persist::crc32(frame.data(), 136)) return 1;
    if (persist::valid_payload_length(0) || persist::valid_payload_length(1041) ||
        !persist::valid_payload_length(128) || !persist::valid_payload_length(1040)) return 1;
    auto header = persist::finalize_header({});
    if (!persist::valid_header(header)) return 1;
    ++header.flags;
    if (persist::valid_header(header)) return 1;
    std::cout << "phase2c_framing_crc_layout=pass\n";
    return 0;
}
