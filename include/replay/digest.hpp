#pragma once

#include <cstdint>
#include <cstring>

#include "core/event_header.hpp"

// Deterministic replay digest — the Phase 4 stand-in for the Part 11.3
// "identical journal hash".
//
// The journal proper is a Phase 6 artifact (order and position transitions),
// which do not exist yet. What Phase 4 must prove is narrower and is fully
// available today: replaying the same WAL twice produces the same event
// ordering, the same book state and the same feature values. This digest is the
// commitment over exactly those three things.
//
// Doubles are mixed by their IEEE-754 bit pattern rather than by value. Two runs
// that differ in the last ulp are not "close enough" — Part 11.3 requires
// bit-identical output, and hashing the bits is what makes that detectable
// rather than rounded away.

namespace replay {

class Digest final {
public:
    void mix(std::uint64_t value) noexcept {
        for (int byte = 0; byte < 8; ++byte) {
            hash_ ^= (value >> (byte * 8)) & 0xFFULL;
            hash_ *= 1099511628211ULL;
        }
    }

    void mix_double(double value) noexcept {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        mix(bits);
    }

    // Every field that a consumer could observe. Deliberately includes the
    // timestamps: Objective 4 requires identical timestamps across runs, so they
    // must be part of the commitment and not merely assumed stable.
    void mix_header(const core::EventHeader& header) noexcept {
        mix(header.seq_global);
        mix(header.ts_broker_ns);
        mix(header.ts_terminal_ns);
        mix(header.ts_local_ns);
        mix(header.corr_id);
        mix(header.seq_source);
        mix(header.symbol_id);
        mix(static_cast<std::uint64_t>(header.source_id));
        mix(static_cast<std::uint64_t>(header.type));
        mix(static_cast<std::uint64_t>(header.source_priority));
        mix(static_cast<std::uint64_t>(header.flags));
    }

    [[nodiscard]] std::uint64_t value() const noexcept { return hash_; }
    void reset() noexcept { hash_ = 1469598103934665603ULL; }

private:
    std::uint64_t hash_{1469598103934665603ULL};
};

}  // namespace replay
