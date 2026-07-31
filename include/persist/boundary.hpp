#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "persist/framing.hpp"

namespace persist {

// Read-side view of the committed-boundary sidecar written alongside every
// segment as `<segment>.meta`. The layout is frozen by WAL_FORMAT.md: two
// alternating 32-byte slots, highest valid generation authoritative.
//
// DEBT: WalWriter keeps its own copy of this layout in an anonymous namespace
// (src/persist/wal_writer.cpp). The definitions are asserted identical below,
// but they should be unified once the capture phase is unfrozen. The writer was
// deliberately left untouched here.
#pragma pack(push, 1)
struct BoundarySlotView final {
    std::array<char, 8> magic;
    std::uint64_t generation;
    std::uint64_t committed;
    std::uint32_t crc;
    std::array<std::uint8_t, 4> reserved;
};

struct BoundaryMetadataView final { std::array<BoundarySlotView, 2> slots; };
#pragma pack(pop)

static_assert(sizeof(BoundarySlotView) == 32);
static_assert(sizeof(BoundaryMetadataView) == 64);
static_assert(offsetof(BoundarySlotView, magic) == 0);
static_assert(offsetof(BoundarySlotView, generation) == 8);
static_assert(offsetof(BoundarySlotView, committed) == 16);
static_assert(offsetof(BoundarySlotView, crc) == 24);
static_assert(offsetof(BoundarySlotView, reserved) == 28);

inline constexpr std::array<char, 8> boundary_magic{'M', 'M', 'E', 'B', 'N', 'D', '0', '1'};

[[nodiscard]] inline bool valid_boundary_slot(const BoundarySlotView& slot) noexcept {
    return slot.magic == boundary_magic &&
           slot.crc == crc32(&slot, offsetof(BoundarySlotView, crc));
}

// Highest valid generation wins, exactly as recovery selects. Returns false when
// neither slot is usable, in which case the caller must fall back to file size.
[[nodiscard]] inline bool select_boundary(const BoundaryMetadataView& metadata,
                                          std::uint64_t& committed) noexcept {
    bool found = false;
    std::uint64_t best_generation = 0;
    for (const auto& slot : metadata.slots) {
        if (!valid_boundary_slot(slot)) continue;
        if (!found || slot.generation > best_generation) {
            best_generation = slot.generation;
            committed = slot.committed;
            found = true;
        }
    }
    return found;
}

}  // namespace persist
