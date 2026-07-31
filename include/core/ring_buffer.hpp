#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace core {

template <typename T, std::size_t Capacity>
class RingBuffer final {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "Hot-path ring values must be trivially copyable");

public:
    [[nodiscard]] bool try_push(const T& value) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) == Capacity) return false;
        storage_[head & mask_] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& value) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        value = storage_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return head_.load(std::memory_order_acquire) -
               tail_.load(std::memory_order_acquire);
    }
    [[nodiscard]] constexpr std::size_t capacity() const noexcept { return Capacity; }

private:
    static constexpr std::size_t mask_ = Capacity - 1;
    std::array<T, Capacity> storage_{};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace core
