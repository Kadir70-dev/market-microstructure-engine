#pragma once

#include <chrono>
#include <cstdint>

#include "core/tsc_calibration.hpp"

namespace core {

enum class ClockSource : std::uint8_t { monotonic, invariant_tsc };

[[nodiscard]] constexpr ClockSource select_clock_source(
    TscCapabilities capabilities, const TscCalibration& calibration) noexcept {
    return capabilities.usable() && calibration.valid
        ? ClockSource::invariant_tsc : ClockSource::monotonic;
}

[[nodiscard]] inline std::uint64_t monotonic_now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

class Clock final {
public:
    Clock(TscCapabilities capabilities, TscCalibration calibration) noexcept
        : calibration_(calibration), source_(select_clock_source(capabilities, calibration)) {}

    [[nodiscard]] std::uint64_t now_ns() const noexcept {
        if (source_ == ClockSource::monotonic) return monotonic_now_ns();
        const auto ticks = read_tsc() - calibration_.tsc_origin;
        return calibration_.ns_origin +
            static_cast<std::uint64_t>(static_cast<double>(ticks) * calibration_.ns_per_tick);
    }
    [[nodiscard]] ClockSource source() const noexcept { return source_; }

private:
    TscCalibration calibration_{};
    ClockSource source_{ClockSource::monotonic};
};

// Time seam required by ARCHITECTURE_V1 §3: replay, paper and live share one
// codebase and differ only in IFeedAdapter, IBroker and IClock. Consumers must
// read time through this interface and never call monotonic_now_ns() directly,
// otherwise a 1x run and a maximum-speed replay observe different values and
// deterministic replay becomes unachievable.
class IClock {
public:
    virtual ~IClock() = default;
    IClock() = default;
    IClock(const IClock&) = delete;
    IClock& operator=(const IClock&) = delete;

    [[nodiscard]] virtual std::uint64_t now_ns() const noexcept = 0;
};

// Live and paper trading: delegates to the existing Clock unchanged.
class SystemClock final : public IClock {
public:
    explicit SystemClock(Clock clock) noexcept : clock_(clock) {}

    [[nodiscard]] std::uint64_t now_ns() const noexcept override { return clock_.now_ns(); }
    [[nodiscard]] ClockSource source() const noexcept { return clock_.source(); }

private:
    Clock clock_;
};

// Replay: time is driven by the event stream, never by the host. advance_to()
// is monotonic by construction, so an out-of-order or repeated timestamp cannot
// move virtual time backwards and cannot make a replay non-reproducible.
class VirtualClock final : public IClock {
public:
    VirtualClock() noexcept = default;
    explicit VirtualClock(std::uint64_t origin_ns) noexcept : now_(origin_ns) {}

    [[nodiscard]] std::uint64_t now_ns() const noexcept override { return now_; }

    // Returns false when the requested time is not in the future, meaning the
    // caller supplied a regression that was ignored rather than applied.
    bool advance_to(std::uint64_t ns) noexcept {
        if (ns <= now_) return false;
        now_ = ns;
        return true;
    }

    void reset(std::uint64_t origin_ns) noexcept { now_ = origin_ns; }

private:
    std::uint64_t now_{0};
};

}  // namespace core
