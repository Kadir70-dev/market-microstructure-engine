#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

#include "feed/mt5_protocol.hpp"

int main() {
    using namespace feed::mt5;
    if (!host_is_little_endian()) return 1;
    if (sizeof(RecordPrefix) != 24 || sizeof(HelloRecord) != 64 ||
        sizeof(HelloAckRecord) != 108 || sizeof(MdRecord) != 100 ||
        sizeof(HbRecord) != 128 || sizeof(SpikeMessage) != 128) return 1;

    RecordPrefix prefix{protocol_magic, protocol_version,
                        static_cast<std::uint16_t>(RecordType::hello),
                        0x0102030405060708ULL, 9};
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&prefix);
    if (bytes[0] != 0x4d || bytes[1] != 0x4d || bytes[2] != 0x45 || bytes[3] != 0x31)
        return 1;
    if (bytes[8] != 0x08 || bytes[15] != 0x01) return 1;

    HelloRecord hello{};
    hello.prefix = prefix;
    hello.account_expected = 123456;
    HelloAckRecord ack{};
    ack.prefix = {protocol_magic, protocol_version,
                  static_cast<std::uint16_t>(RecordType::hello_ack),
                  hello.prefix.session_epoch, 10};
    ack.account_actual = hello.account_expected;
    ack.account_margin_mode = static_cast<std::uint32_t>(MarginMode::hedging);
    std::array<std::uint8_t, 32> server{};
    server[0] = 7;
    ack.server_hash = server;
    if (validate_handshake(hello, ack, server, MarginMode::hedging) != HandshakeVerdict::accept)
        return 1;
    ++ack.prefix.version;
    if (validate_handshake(hello, ack, server, MarginMode::hedging) != HandshakeVerdict::bad_version)
        return 1;
    ack.prefix.version = protocol_version;
    ++ack.prefix.session_epoch;
    if (validate_handshake(hello, ack, server, MarginMode::hedging) != HandshakeVerdict::stale_epoch)
        return 1;
    ack.prefix.session_epoch = hello.prefix.session_epoch;
    ++ack.account_actual;
    if (validate_handshake(hello, ack, server, MarginMode::hedging) != HandshakeVerdict::account_mismatch)
        return 1;
    ack.account_actual = hello.account_expected;
    auto wrong_server = server; ++wrong_server[0];
    if (validate_handshake(hello, ack, wrong_server, MarginMode::hedging) != HandshakeVerdict::server_mismatch)
        return 1;
    if (validate_handshake(hello, ack, server, MarginMode::netting) != HandshakeVerdict::margin_mode_mismatch)
        return 1;
    std::cout << "protocol_layout_and_handshake=pass\n";
    return 0;
}
