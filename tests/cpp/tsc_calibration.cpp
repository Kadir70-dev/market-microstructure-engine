#include <chrono>
#include <iostream>

#include "core/clock.hpp"

int main() {
    const core::TscCalibration invalid{};
    if (core::select_clock_source({false, false}, invalid) != core::ClockSource::monotonic) return 1;
    if (core::select_clock_source({true, false}, invalid) != core::ClockSource::monotonic) return 1;
    core::Clock fallback{{false, false}, invalid};
    if (fallback.source() != core::ClockSource::monotonic || fallback.now_ns() == 0) return 1;

    const auto capabilities = core::detect_tsc_capabilities();
    const auto calibration = core::calibrate_tsc(capabilities, std::chrono::milliseconds{5});
    if (capabilities.usable() && !calibration.valid) return 1;
    core::Clock clock{capabilities, calibration};
    if (clock.now_ns() == 0) return 1;
    std::cout << "tsc_usable=" << capabilities.usable()
              << " source=" << static_cast<int>(clock.source()) << '\n';
    return 0;
}
