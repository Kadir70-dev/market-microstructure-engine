#include <iostream>
#include <vector>

#include "core/clock.hpp"
#include "feed/replay_adapter.hpp"
#include "replay_fixture.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}
}

int main() {
    // ---- VirtualClock in isolation ---------------------------------------
    {
        core::VirtualClock clock;
        check(clock.now_ns() == 0, "vclock_starts_at_origin");
        check(clock.advance_to(100), "vclock_advances_forward");
        check(clock.now_ns() == 100, "vclock_value_applied");
        // Monotonic by construction: a regression is refused, not applied, so a
        // malformed stream cannot rewind virtual time mid-replay.
        check(!clock.advance_to(50), "vclock_refuses_regression");
        check(clock.now_ns() == 100, "vclock_unchanged_after_regression");
        check(!clock.advance_to(100), "vclock_refuses_equal");
        check(clock.now_ns() == 100, "vclock_unchanged_after_equal");
        clock.reset(7);
        check(clock.now_ns() == 7, "vclock_reset");
    }

    // ---- clock is driven by the stream, never by the host -----------------
    const auto dir = replay_fixture::make_wal("vclock", 1'000, 64 * 1024);
    if (dir.empty()) { std::cout << "fixture=FAIL\n"; return 1; }

    feed::ReplayAdapter adapter;
    check(adapter.open(dir), "vclock_adapter_open");

    core::FixedEvent event{};
    std::vector<std::uint64_t> clock_trace;
    std::vector<std::uint64_t> event_trace;
    clock_trace.reserve(1'000);
    event_trace.reserve(1'000);

    std::uint64_t previous_clock = 0;
    bool monotonic = true;
    bool clock_tracks_event = true;

    for (;;) {
        const auto poll = adapter.poll(event);
        if (poll == feed::PollResult::end_of_stream) break;
        check(poll == feed::PollResult::event, "vclock_poll_ok");
        if (poll != feed::PollResult::event) break;

        const auto now = adapter.clock().now_ns();
        if (now < previous_clock) monotonic = false;
        // Virtual time equals the event's own recorded local timestamp: replay
        // observes exactly the time the recorder observed.
        if (now != event.header.ts_local_ns) clock_tracks_event = false;
        previous_clock = now;
        clock_trace.push_back(now);
        event_trace.push_back(event.header.ts_local_ns);
    }

    check(clock_trace.size() == 1'000, "vclock_all_events_polled");
    check(monotonic, "vclock_never_runs_backwards");
    check(clock_tracks_event, "vclock_equals_recorded_ts_local");
    check(adapter.stats().first_ts_local_ns == event_trace.front(), "vclock_first_ts_recorded");
    check(adapter.stats().last_ts_local_ns == event_trace.back(), "vclock_last_ts_recorded");

    // A second pass over the same WAL must reproduce the identical clock trace.
    feed::ReplayAdapter again;
    check(again.open(dir), "vclock_reopen");
    std::vector<std::uint64_t> second_trace;
    second_trace.reserve(1'000);
    for (;;) {
        const auto poll = again.poll(event);
        if (poll != feed::PollResult::event) break;
        second_trace.push_back(again.clock().now_ns());
    }
    check(second_trace == clock_trace, "vclock_trace_reproducible");

    replay_fixture::remove_all(dir);
    return failures == 0 ? 0 : 1;
}
