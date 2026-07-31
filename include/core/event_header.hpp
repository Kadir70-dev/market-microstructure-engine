#pragma once

#include <cstdint>
#include <type_traits>

namespace core {

struct EventHeader final {
    std::uint64_t seq_global{0};
    std::uint64_t ts_broker_ns{0};
    std::uint64_t ts_terminal_ns{0};
    std::uint64_t ts_local_ns{0};
    std::uint64_t corr_id{0};
    std::uint32_t seq_source{0};
    std::uint32_t symbol_id{0};
    std::uint16_t source_id{0};
    std::uint16_t type{0};
    std::uint8_t source_priority{0};
    std::uint8_t flags{0};
    std::uint16_t _pad{0};
};

static_assert(sizeof(EventHeader) == 56);
static_assert(std::is_trivially_copyable_v<EventHeader>);
static_assert(std::is_standard_layout_v<EventHeader>);

}  // namespace core
