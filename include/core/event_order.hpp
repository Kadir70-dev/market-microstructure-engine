#pragma once

#include "core/event_header.hpp"

// Total event ordering, Architecture Part 3 and Part 11.3:
//
//   "total order: (ts_local_ns, source_priority, seq_source)"
//
// The tuple exists because timestamps alone are not a total order. Two sources
// can stamp the same nanosecond, and on a coarse clock a single source can too.
// Without a deterministic tie-break the merge order would depend on arrival or
// scheduling, and a replay could legitimately produce a different sequence from
// the live run it is reproducing — which would make the Part 11.3 determinism
// gate unachievable rather than merely failing.

namespace core {

[[nodiscard]] constexpr bool event_order_less(const EventHeader& lhs,
                                              const EventHeader& rhs) noexcept {
    if (lhs.ts_local_ns != rhs.ts_local_ns) return lhs.ts_local_ns < rhs.ts_local_ns;
    if (lhs.source_priority != rhs.source_priority) return lhs.source_priority < rhs.source_priority;
    return lhs.seq_source < rhs.seq_source;
}

[[nodiscard]] constexpr bool event_order_equal(const EventHeader& lhs,
                                               const EventHeader& rhs) noexcept {
    return lhs.ts_local_ns == rhs.ts_local_ns &&
           lhs.source_priority == rhs.source_priority &&
           lhs.seq_source == rhs.seq_source;
}

// True when `next` sorts strictly before `previous`, i.e. the stream went
// backwards. Replay treats this as corruption and fails closed: a WAL whose
// recorded order violates the total order cannot be replayed faithfully, and
// silently sorting it would fabricate a history that never occurred.
[[nodiscard]] constexpr bool event_order_regression(const EventHeader& previous,
                                                    const EventHeader& next) noexcept {
    return event_order_less(next, previous);
}

}  // namespace core
