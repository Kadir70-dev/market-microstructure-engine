#pragma once

#include <chrono>
#include <cstdint>

namespace core {

struct TscCapabilities final {
    bool constant_tsc{false};
    bool nonstop_tsc{false};
    [[nodiscard]] constexpr bool usable() const noexcept {
        return constant_tsc && nonstop_tsc;
    }
};

struct TscCalibration final {
    std::uint64_t tsc_origin{0};
    std::uint64_t ns_origin{0};
    double ns_per_tick{0.0};
    bool valid{false};
};

[[nodiscard]] TscCapabilities detect_tsc_capabilities() noexcept;
[[nodiscard]] std::uint64_t read_tsc() noexcept;
[[nodiscard]] TscCalibration calibrate_tsc(
    TscCapabilities capabilities,
    std::chrono::milliseconds interval = std::chrono::milliseconds{20}) noexcept;

}  // namespace core
