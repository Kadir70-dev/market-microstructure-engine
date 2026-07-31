#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "core/event_header.hpp"

namespace core {

enum class EventType : std::uint16_t {
    quote, trade, book_delta, timer, order_ack, order_reject, fill,
    partial_fill, cancel_ack, position_update, account_update, heartbeat,
    gap, backpressure, symbol_meta_change, book_rebase, clock_step
};

#pragma pack(push, 1)
struct QuotePayload final {
    std::int64_t bid, ask, bid_size, ask_size;
    std::uint32_t flags;
};
struct TradePayload final {
    std::int64_t price, size;
    std::uint32_t aggressor_side;
    std::uint64_t trade_id;
};
struct BookDeltaPayload final {
    std::uint8_t side;
    std::int64_t price, new_size;
    std::uint8_t action;
    std::array<std::byte, 8> reserved{};
};
struct TimerPayload final { std::uint32_t timer_id; std::uint64_t scheduled_ns; };
struct OrderAckPayload final {
    std::uint64_t logical_order_id, broker_order_id;
    std::int64_t price, volume;
};
struct OrderRejectPayload final {
    std::uint64_t logical_order_id;
    std::uint32_t retcode, reason_class;
};
struct FillPayload final {
    std::uint64_t logical_order_id, deal_id, position_ticket;
    std::int64_t price, volume, commission, swap, remaining;
    std::uint8_t is_final;
    std::array<std::byte, 3> reserved{};
};
struct CancelAckPayload final { std::uint64_t logical_order_id; std::int64_t cancelled_volume; };
struct PositionUpdatePayload final {
    std::uint64_t position_ticket;
    std::uint32_t symbol_id;
    std::int64_t volume, avg_price, unrealized, margin;
};
struct AccountUpdatePayload final {
    std::int64_t balance, equity, margin, free_margin, margin_level;
    std::uint32_t currency_id;
};
struct HeartbeatPayload final {
    std::uint16_t source_id;
    std::uint64_t ts;
    std::uint8_t connected, trade_allowed, book_source;
    std::array<std::byte, 7> reserved{};
};
struct GapPayload final {
    std::uint16_t source_id;
    std::uint32_t expected_seq, received_seq, lost_count;
    std::uint16_t reserved;
};
struct BackpressurePayload final {
    std::uint32_t ring_id, occupancy_pct, dropped_count;
};
struct SymbolMetaChangePayload final { std::uint32_t symbol_id, changed_mask; };
struct BookRebasePayload final {
    std::uint32_t symbol_id;
    std::int64_t old_ref, new_ref;
};
struct ClockStepPayload final { std::int64_t wall_delta_ns; std::uint64_t detected_at; };
struct BookSnapshotRef final {
    std::uint32_t slab_index{0};
    std::uint32_t generation{0};
    std::uint16_t n_bid{0};
    std::uint16_t n_ask{0};
    std::uint64_t checksum{0};
    std::uint8_t book_source{0};
};
#pragma pack(pop)

struct FixedEvent final {
    EventHeader header{};
    alignas(8) std::array<std::byte, 72> payload{};
};

static_assert(sizeof(BookSnapshotRef) == 21);
static_assert(sizeof(QuotePayload) == 36);
static_assert(sizeof(TradePayload) == 28);
static_assert(sizeof(BookDeltaPayload) == 26);
static_assert(sizeof(TimerPayload) == 12);
static_assert(sizeof(OrderAckPayload) == 32);
static_assert(sizeof(OrderRejectPayload) == 16);
static_assert(sizeof(FillPayload) == 68);
static_assert(sizeof(CancelAckPayload) == 16);
static_assert(sizeof(PositionUpdatePayload) == 44);
static_assert(sizeof(AccountUpdatePayload) == 44);
static_assert(sizeof(HeartbeatPayload) == 20);
static_assert(sizeof(GapPayload) == 16);
static_assert(sizeof(BackpressurePayload) == 12);
static_assert(sizeof(SymbolMetaChangePayload) == 8);
static_assert(sizeof(BookRebasePayload) == 20);
static_assert(sizeof(ClockStepPayload) == 16);
static_assert(sizeof(FixedEvent) == 128);
static_assert(std::is_trivially_copyable_v<FixedEvent>);
static_assert(std::is_standard_layout_v<FixedEvent>);

}  // namespace core
