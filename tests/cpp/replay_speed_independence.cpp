#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "feed/replay_adapter.hpp"
#include "replay/replay_engine.hpp"
#include "replay_fixture.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}

struct Trace final {
    std::vector<std::uint64_t> clock_ns;
    std::vector<std::uint64_t> seq_global;
    std::vector<std::uint64_t> ts_local_ns;
    friend bool operator==(const Trace& a, const Trace& b) {
        return a.clock_ns == b.clock_ns && a.seq_global == b.seq_global &&
               a.ts_local_ns == b.ts_local_ns;
    }
};

// `pace_us` injects real wall-clock delay between polls, emulating a slow
// (1x-style) replay. Part 11.3 requires 1x and maximum speed to be identical;
// they can only differ if something on the path reads the host clock.
Trace collect(const std::filesystem::path& dir, unsigned pace_us) {
    Trace trace;
    feed::ReplayAdapter adapter;
    if (!adapter.open(dir)) return trace;
    core::FixedEvent event{};
    for (;;) {
        const auto poll = adapter.poll(event);
        if (poll != feed::PollResult::event) break;
        trace.clock_ns.push_back(adapter.clock().now_ns());
        trace.seq_global.push_back(event.header.seq_global);
        trace.ts_local_ns.push_back(event.header.ts_local_ns);
        if (pace_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(pace_us));
    }
    return trace;
}
}

int main() {
    // Deliberately small: the paced pass sleeps per event, so this is sized to
    // stay a fast test while still crossing a segment boundary.
    const auto dir = replay_fixture::make_wal("speed", 400, 16 * 1024);
    if (dir.empty()) { std::cout << "fixture=FAIL\n"; return 1; }

    const auto fast = collect(dir, 0);
    const auto paced = collect(dir, 200);   // ~80 ms of injected wall time

    check(!fast.clock_ns.empty(), "speed_fast_produced_events");
    check(fast.clock_ns.size() == paced.clock_ns.size(), "speed_same_event_count");
    check(fast == paced, "speed_traces_identical_1x_vs_max");

    // Same assertion at the engine level, where book and feature state also
    // participate: the digest must not depend on how fast the replay ran.
    replay::ReplayEngine engine;
    const auto quick = engine.run(dir);

    const auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto slow = engine.run(dir);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    check(quick.ok && slow.ok, "speed_engine_runs_ok");
    check(quick.digest == slow.digest, "speed_engine_digest_independent_of_wall_time");
    check(quick.events == slow.events, "speed_engine_event_count_stable");
    check(elapsed >= std::chrono::milliseconds(50), "speed_wall_time_actually_elapsed");

    replay_fixture::remove_all(dir);
    return failures == 0 ? 0 : 1;
}
