#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>

#include "core/event.hpp"
#include "persist/wal_writer.hpp"

namespace { std::atomic<std::uint64_t> allocations{0}; std::atomic<bool> track{false}; }
void* operator new(std::size_t size) {
    if (track.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc{};
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (track.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size);
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }

int main() {
    const auto path = std::filesystem::temp_directory_path() / "mme_wal_no_alloc";
    std::error_code error; std::filesystem::remove_all(path, error);
    persist::WalConfig config{path}; config.segment_data_bytes = 2 << 20;
    persist::WalWriter writer;
    if (!writer.open(config, {}, 0)) return 1;
    core::FixedEvent event{};
    track.store(true, std::memory_order_relaxed);
    for (std::uint64_t i = 0; i < 10000; ++i) {
        event.header.seq_global = i;
        if (writer.append(0, 0, &event, sizeof(event)) != persist::AppendResult::committed) return 1;
    }
    track.store(false, std::memory_order_relaxed);
    if (allocations.load(std::memory_order_relaxed) != 0) return 1;
    writer.close(); std::filesystem::remove_all(path, error);
    std::cout << "phase2c_append_allocations=0\n";
    return 0;
}
