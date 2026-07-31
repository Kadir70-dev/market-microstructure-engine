#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>

#include "core/event.hpp"
#include "core/ring_buffer.hpp"
#include "core/slab_pool.hpp"
#include "telemetry/histogram.hpp"

namespace {
std::atomic<std::size_t> allocations{0};
std::atomic<bool> tracking{false};
}

void* operator new(std::size_t size) {
    if (tracking.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* value = std::malloc(size)) return value;
    throw std::bad_alloc{};
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }

int main() {
    core::RingBuffer<core::FixedEvent, 64> ring;
    core::SlabPool<1040, 8> slabs;
    telemetry::Histogram<> histogram;
    core::FixedEvent input{}, output{};
    tracking.store(true, std::memory_order_relaxed);
    for (std::uint64_t i = 0; i < 10000; ++i) {
        input.header.seq_global = i;
        if (!ring.try_push(input) || !ring.try_pop(output)) return 1;
        const auto handle = slabs.acquire();
        if (!handle || slabs.get(*handle) == nullptr || !slabs.release(*handle)) return 1;
        histogram.record(i + 1);
    }
    tracking.store(false, std::memory_order_relaxed);
    std::cout << "post_init_allocations=" << allocations.load() << '\n';
    return allocations.load() == 0 ? 0 : 1;
}
