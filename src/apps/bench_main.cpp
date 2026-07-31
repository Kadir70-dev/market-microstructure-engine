#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>

#include "core/ring_buffer.hpp"

int main() {
    constexpr std::uint64_t messages = 20000000;
    core::RingBuffer<std::uint64_t, 65536> ring;
    std::uint64_t output{};
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < messages; ++i) {
        if (!ring.try_push(i) || !ring.try_pop(output) || output != i) return 2;
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    const double rate = static_cast<double>(messages) / seconds;
    std::cout << std::fixed << std::setprecision(2)
              << "ring_messages_per_second=" << rate << '\n';
    return rate >= 10000000.0 ? 0 : 1;
}
