#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace core {

struct SlabHandle final {
    std::uint32_t index{0};
    std::uint32_t generation{0};
    friend constexpr bool operator==(SlabHandle lhs, SlabHandle rhs) noexcept {
        return lhs.index == rhs.index && lhs.generation == rhs.generation;
    }
};

template <std::size_t SlabSize = 1040, std::size_t Capacity = 4096>
class SlabPool final {
public:
    using Slab = std::array<std::byte, SlabSize>;

    SlabPool() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) free_[i] = Capacity - 1 - i;
    }

    [[nodiscard]] std::optional<SlabHandle> acquire() noexcept {
        if (free_count_ == 0) return std::nullopt;
        const auto index = static_cast<std::uint32_t>(free_[--free_count_]);
        used_[index] = true;
        return SlabHandle{index, generation_[index]};
    }

    [[nodiscard]] bool release(SlabHandle handle) noexcept {
        if (!valid(handle)) return false;
        used_[handle.index] = false;
        ++generation_[handle.index];
        free_[free_count_++] = handle.index;
        return true;
    }

    [[nodiscard]] Slab* get(SlabHandle handle) noexcept {
        return valid(handle) ? &slabs_[handle.index] : nullptr;
    }
    [[nodiscard]] const Slab* get(SlabHandle handle) const noexcept {
        return valid(handle) ? &slabs_[handle.index] : nullptr;
    }
    [[nodiscard]] bool valid(SlabHandle handle) const noexcept {
        return handle.index < Capacity && used_[handle.index] &&
               generation_[handle.index] == handle.generation;
    }

private:
    std::array<Slab, Capacity> slabs_{};
    std::array<std::uint32_t, Capacity> generation_{};
    std::array<std::size_t, Capacity> free_{};
    std::array<bool, Capacity> used_{};
    std::size_t free_count_{Capacity};
};

static_assert(sizeof(typename SlabPool<>::Slab) == 1040);

}  // namespace core
