#include <iostream>

#include "core/slab_pool.hpp"

int main() {
    core::SlabPool<1040, 4> pool;
    const auto first = pool.acquire();
    if (!first || pool.get(*first) == nullptr) return 1;
    pool.get(*first)->front() = std::byte{0x2a};
    if (!pool.release(*first) || pool.get(*first) != nullptr) return 1;
    const auto reused = pool.acquire();
    if (!reused || reused->index != first->index || reused->generation == first->generation) return 1;
    std::cout << "stale_generation_detected=1\n";
    return 0;
}
