#include <iostream>
#include <type_traits>

#include "core/clock.hpp"
#include "feed/feed_adapter.hpp"
#include "feed/mt5_pipe_adapter.hpp"

int main() {
    // ---- VirtualClock ----------------------------------------------------
    core::VirtualClock virtual_clock;
    if (virtual_clock.now_ns() != 0) return 1;
    if (!virtual_clock.advance_to(1'000)) return 1;
    if (virtual_clock.now_ns() != 1'000) return 1;

    // Regressions and repeats are ignored, not applied. This is what keeps a
    // replay reproducible when the stream contains equal or out-of-order stamps.
    if (virtual_clock.advance_to(999)) return 1;
    if (virtual_clock.now_ns() != 1'000) return 1;
    if (virtual_clock.advance_to(1'000)) return 1;
    if (virtual_clock.now_ns() != 1'000) return 1;
    if (!virtual_clock.advance_to(1'001)) return 1;
    if (virtual_clock.now_ns() != 1'001) return 1;

    core::VirtualClock seeded{5'000};
    if (seeded.now_ns() != 5'000) return 1;
    seeded.reset(0);
    if (seeded.now_ns() != 0) return 1;

    // Virtual time must never come from the host clock.
    const auto before = core::monotonic_now_ns();
    core::VirtualClock idle;
    if (idle.now_ns() != 0) return 1;
    if (core::monotonic_now_ns() < before) return 1;

    // ---- SystemClock -----------------------------------------------------
    // Delegates to the existing Clock unchanged: same source, monotonic.
    const auto capabilities = core::detect_tsc_capabilities();
    const auto calibration = core::calibrate_tsc(capabilities);
    core::Clock raw{capabilities, calibration};
    core::SystemClock system_clock{raw};
    if (system_clock.source() != raw.source()) return 1;
    const auto first = system_clock.now_ns();
    const auto second = system_clock.now_ns();
    if (second < first) return 1;

    // ---- Interface conformance -------------------------------------------
    core::IClock& as_clock = virtual_clock;
    if (as_clock.now_ns() != 1'001) return 1;
    core::IClock& as_system = system_clock;
    if (as_system.now_ns() < first) return 1;

    static_assert(std::is_base_of_v<core::IClock, core::VirtualClock>);
    static_assert(std::is_base_of_v<core::IClock, core::SystemClock>);
    static_assert(std::is_abstract_v<core::IClock>);
    static_assert(std::is_abstract_v<feed::IFeedAdapter>);
    static_assert(std::is_base_of_v<feed::IFeedAdapter, feed::Mt5PipeAdapter>);

    // The live MT5 adapter satisfies the feed seam. Unbound it reports error
    // rather than dereferencing a null endpoint or clock.
    feed::Mt5PipeAdapter adapter{9, 1, 3'000'000'000ULL};
    feed::IFeedAdapter& as_feed = adapter;
    core::FixedEvent event{};
    if (as_feed.poll(event) != feed::PollResult::error) return 1;
    if (event.header.seq_global != 0) return 1;

    // Binding installs the injected clock; no pipe traffic is generated.
    feed::NamedPipeEndpoint endpoint;
    adapter.bind(endpoint, virtual_clock);
    if (&as_feed.clock() != &static_cast<core::IClock&>(virtual_clock)) return 1;

    std::cout << "clock_virtual_monotonic=pass\n";
    std::cout << "clock_system_delegates=pass\n";
    std::cout << "feed_adapter_conformance=pass\n";
    return 0;
}
