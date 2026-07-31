#pragma once

#include <cstdint>

#include "core/clock.hpp"
#include "core/event.hpp"

namespace feed {

// Outcome of one pull against a feed. `idle` means the source is healthy but has
// nothing available right now; `end_of_stream` is terminal and is how a replay
// signals exhaustion; `error` is terminal and unrecoverable for this source.
enum class PollResult : std::uint8_t { event, idle, end_of_stream, error };

// The feed seam required by ARCHITECTURE_V1 §3. Live MT5 ingest and WAL replay
// both satisfy this, which is what lets the Feature Engine, Backtest Engine, OMS
// and Execution Simulator run identical code against either source.
//
// Pull model: the consumer drives. There is no producer thread and no ring
// buffer behind this interface, so event ordering is whatever the source
// produced and never depends on scheduling.
class IFeedAdapter {
public:
    virtual ~IFeedAdapter() = default;
    IFeedAdapter() = default;
    IFeedAdapter(const IFeedAdapter&) = delete;
    IFeedAdapter& operator=(const IFeedAdapter&) = delete;

    // Retrieves the next event. `out` is only modified when PollResult::event is
    // returned. Must not allocate.
    [[nodiscard]] virtual PollResult poll(core::FixedEvent& out) noexcept = 0;

    // The time source consumers must read. Live sources return a SystemClock;
    // replay returns a VirtualClock driven by the recorded event stream.
    [[nodiscard]] virtual core::IClock& clock() noexcept = 0;
};

}  // namespace feed
