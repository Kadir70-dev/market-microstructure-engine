#include <iostream>

#include "replay/replay_engine.hpp"
#include "replay_fixture.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}
}

int main() {
    // Small segments so the run spans several files: the segment-advance path is
    // where a replay is most likely to lose or duplicate events, and a
    // single-segment fixture would never exercise it.
    const auto dir = replay_fixture::make_wal("determinism", 5'000, 64 * 1024);
    if (dir.empty()) { std::cout << "fixture=FAIL\n"; return 1; }

    replay::ReplayEngine engine;
    const auto first = engine.run(dir);
    check(first.ok, "replay_first_run_ok");
    check(first.events == 5'000, "replay_all_events_read");
    check(first.quotes == 5'000, "replay_all_quotes_decoded");
    check(first.segments > 1, "replay_spans_multiple_segments");
    check(first.feature_vectors == 5'000, "replay_feature_vector_per_quote");
    check(first.error == feed::ReplayError::none, "replay_no_error");

    // Part 18 Phase 4 gate: 100 consecutive runs, identical output.
    std::uint64_t mismatches = 0;
    std::uint64_t event_mismatches = 0;
    for (int run = 1; run < 100; ++run) {
        const auto next = engine.run(dir);
        if (!next.ok) { ++mismatches; continue; }
        if (next.digest != first.digest) ++mismatches;
        if (next.events != first.events) ++event_mismatches;
    }
    check(mismatches == 0, "replay_100_runs_identical_digest");
    check(event_mismatches == 0, "replay_100_runs_identical_event_count");

    // A fresh engine instance must agree with the original: determinism cannot
    // depend on residual state carried between runs.
    replay::ReplayEngine independent;
    const auto separate = independent.run(dir);
    check(separate.digest == first.digest, "replay_fresh_engine_same_digest");

    // The digest must actually discriminate. If it were constant, every check
    // above would pass vacuously.
    replay::ReplayEngine other_params(4);   // different warm-up threshold
    const auto varied = other_params.run(dir);
    check(varied.digest != first.digest, "replay_digest_is_discriminating");

    replay_fixture::remove_all(dir);
    return failures == 0 ? 0 : 1;
}
