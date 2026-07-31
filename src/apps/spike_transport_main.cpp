#include <cstdlib>
#include <iomanip>
#include <iostream>

#include "feed/transport_spike.hpp"

namespace {
void print(const feed::transport_spike::Metrics& value) {
    std::cout << value.transport
              << " samples=" << value.samples
              << " throughput_per_second=" << std::fixed << std::setprecision(2)
              << value.throughput_per_second
              << " p50_us=" << value.p50_us
              << " p95_us=" << value.p95_us
              << " p99_us=" << value.p99_us
              << " cpu_percent=" << value.cpu_percent
              << " corruptions=" << value.corruption_count
              << " publication_proven=" << value.publication_proven << '\n';
}
}

int main(int argc, char** argv) {
    std::size_t samples = 50000;
    if (argc == 2) samples = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
    const auto pipe = feed::transport_spike::benchmark_named_pipe(samples);
    const auto shm = feed::transport_spike::benchmark_fence_free_shared_memory(samples);
    print(pipe);
    print(shm);
    if (!pipe.supported || pipe.corruption_count != 0) return 1;
    // Shared memory is never selected by empirical speed alone.
    return 0;
}
