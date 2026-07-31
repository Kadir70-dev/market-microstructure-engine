#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace feed::transport_spike {

struct Metrics final {
    std::string transport;
    std::uint64_t samples{0};
    double throughput_per_second{0.0};
    double p50_us{0.0};
    double p95_us{0.0};
    double p99_us{0.0};
    double cpu_percent{0.0};
    std::uint64_t corruption_count{0};
    bool supported{false};
    bool publication_proven{false};
};

[[nodiscard]] Metrics benchmark_named_pipe(std::size_t samples) noexcept;
[[nodiscard]] Metrics benchmark_fence_free_shared_memory(std::size_t samples) noexcept;

}  // namespace feed::transport_spike
