#include "core/tsc_calibration.hpp"

#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#include <x86intrin.h>
#endif

namespace core {

TscCapabilities detect_tsc_capabilities() noexcept {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    int highest[4]{};
    __cpuid(highest, static_cast<int>(0x80000000U));
    if (static_cast<unsigned>(highest[0]) < 0x80000007U) return {};
    int info[4]{};
    __cpuid(info, static_cast<int>(0x80000007U));
    const bool invariant = (info[3] & (1 << 8)) != 0;
    return {invariant, invariant};
#elif defined(__i386__) || defined(__x86_64__)
    if (__get_cpuid_max(0x80000000U, nullptr) < 0x80000007U) return {};
    unsigned eax{}, ebx{}, ecx{}, edx{};
    __get_cpuid(0x80000007U, &eax, &ebx, &ecx, &edx);
    const bool invariant = (edx & (1U << 8U)) != 0U;
    return {invariant, invariant};
#else
    return {};
#endif
}

std::uint64_t read_tsc() noexcept {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    return __rdtsc();
#elif defined(__i386__) || defined(__x86_64__)
    return __rdtsc();
#else
    return 0;
#endif
}

TscCalibration calibrate_tsc(TscCapabilities capabilities,
                             std::chrono::milliseconds interval) noexcept {
    if (!capabilities.usable() || interval.count() <= 0) return {};
    const auto ns_start = std::chrono::steady_clock::now();
    const auto tsc_start = read_tsc();
    std::this_thread::sleep_for(interval);
    const auto tsc_end = read_tsc();
    const auto ns_end = std::chrono::steady_clock::now();
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        ns_end - ns_start).count();
    if (tsc_end <= tsc_start || elapsed_ns <= 0) return {};
    return {tsc_start,
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                ns_start.time_since_epoch()).count()),
            static_cast<double>(elapsed_ns) / static_cast<double>(tsc_end - tsc_start),
            true};
}

}  // namespace core
