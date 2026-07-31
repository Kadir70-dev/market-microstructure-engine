#include <iostream>

#include "feed/transport_spike.hpp"

int main() {
#if defined(_WIN32)
    constexpr std::size_t samples = 50000;
    const auto pipe = feed::transport_spike::benchmark_named_pipe(samples);
    if (!pipe.supported || !pipe.publication_proven || pipe.corruption_count != 0) return 1;
    const auto shm = feed::transport_spike::benchmark_fence_free_shared_memory(samples);
    if (!shm.supported || shm.publication_proven) return 1;
    std::cout << "named_pipe samples=" << pipe.samples
              << " throughput=" << pipe.throughput_per_second
              << " p50_us=" << pipe.p50_us << " p95_us=" << pipe.p95_us
              << " p99_us=" << pipe.p99_us << " cpu_percent=" << pipe.cpu_percent
              << " corruptions=" << pipe.corruption_count << '\n';
    std::cout << "fence_free_shm samples=" << shm.samples
              << " throughput=" << shm.throughput_per_second
              << " p50_us=" << shm.p50_us << " p95_us=" << shm.p95_us
              << " p99_us=" << shm.p99_us << " cpu_percent=" << shm.cpu_percent
              << " corruptions=" << shm.corruption_count
              << " publication_proven=" << shm.publication_proven << '\n';
#else
    std::cout << "transport_runtime_skipped_non_windows=1\n";
#endif
    return 0;
}
