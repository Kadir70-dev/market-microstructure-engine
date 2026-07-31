#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>

#include "core/ring_buffer.hpp"

int main() {
    constexpr std::uint64_t count = 500000;
    core::RingBuffer<std::uint64_t, 1024> ring;
    std::atomic<bool> failed{false};
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < count; ++i)
            while (!ring.try_push(i)) std::this_thread::yield();
    });
    std::thread consumer([&] {
        for (std::uint64_t i = 0; i < count; ++i) {
            std::uint64_t value{};
            while (!ring.try_pop(value)) std::this_thread::yield();
            if (value != i) failed.store(true, std::memory_order_relaxed);
        }
    });
    producer.join();
    consumer.join();
    if (ring.size() != 0) failed.store(true, std::memory_order_relaxed);
    std::cout << "ring ordered=" << !failed.load() << '\n';
    return failed.load() ? 1 : 0;
}
