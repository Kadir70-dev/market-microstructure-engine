#include <algorithm>
#include <iostream>
#include <vector>

#include "core/event_order.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}

core::EventHeader make(std::uint64_t ts, std::uint8_t priority, std::uint32_t seq) {
    core::EventHeader header{};
    header.ts_local_ns = ts;
    header.source_priority = priority;
    header.seq_source = seq;
    return header;
}
}

int main() {
    // Primary key: timestamp.
    check(core::event_order_less(make(1, 0, 0), make(2, 0, 0)), "tie_ts_primary");
    check(!core::event_order_less(make(2, 0, 0), make(1, 0, 0)), "tie_ts_primary_reverse");

    // Simultaneous timestamps fall through to source_priority. Without this the
    // merge order of two sources stamping the same nanosecond would depend on
    // arrival, and replay could not reproduce the live sequence.
    check(core::event_order_less(make(5, 0, 9), make(5, 1, 0)), "tie_priority_secondary");
    check(!core::event_order_less(make(5, 1, 0), make(5, 0, 9)), "tie_priority_secondary_reverse");

    // Same timestamp and priority: seq_source is the final discriminator.
    check(core::event_order_less(make(5, 1, 1), make(5, 1, 2)), "tie_seq_tertiary");
    check(!core::event_order_less(make(5, 1, 2), make(5, 1, 1)), "tie_seq_tertiary_reverse");

    // Fully equal keys are neither less nor greater — required for a strict weak
    // ordering, and what makes std::sort well-defined over these headers.
    const auto a = make(5, 1, 1);
    const auto b = make(5, 1, 1);
    check(!core::event_order_less(a, b) && !core::event_order_less(b, a), "tie_equal_is_not_less");
    check(core::event_order_equal(a, b), "tie_equal_predicate");

    // Irreflexivity and transitivity across the whole tuple.
    check(!core::event_order_less(a, a), "tie_irreflexive");
    const auto low = make(5, 0, 1), mid = make(5, 0, 2), high = make(5, 1, 0);
    check(core::event_order_less(low, mid) && core::event_order_less(mid, high) &&
          core::event_order_less(low, high), "tie_transitive");

    // A deterministic total order must sort a shuffled batch to one answer.
    std::vector<core::EventHeader> batch{
        make(7, 1, 3), make(5, 0, 2), make(7, 0, 9), make(5, 0, 1), make(7, 1, 1)};
    auto sorted = batch;
    std::sort(sorted.begin(), sorted.end(), core::event_order_less);
    auto reshuffled = batch;
    std::reverse(reshuffled.begin(), reshuffled.end());
    std::sort(reshuffled.begin(), reshuffled.end(), core::event_order_less);
    bool same = sorted.size() == reshuffled.size();
    for (std::size_t i = 0; same && i < sorted.size(); ++i)
        same = core::event_order_equal(sorted[i], reshuffled[i]);
    check(same, "tie_sort_order_independent_of_input_order");
    check(sorted.front().ts_local_ns == 5 && sorted.front().seq_source == 1, "tie_sort_first");
    check(sorted.back().ts_local_ns == 7 && sorted.back().source_priority == 1 &&
          sorted.back().seq_source == 3, "tie_sort_last");

    // Regression detection is what replay uses to fail closed.
    check(core::event_order_regression(make(10, 0, 5), make(9, 0, 0)), "tie_regression_detected");
    check(!core::event_order_regression(make(9, 0, 0), make(10, 0, 5)), "tie_forward_not_regression");
    check(!core::event_order_regression(a, b), "tie_equal_not_regression");

    return failures == 0 ? 0 : 1;
}
