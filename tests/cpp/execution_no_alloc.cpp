#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>

#include "exec/paper_broker.hpp"

// Zero dynamic allocation on the steady-state order and fill hot path.
//
// Construction of the broker is NOT the hot path: it is done once and here it is
// heap-placed deliberately (the object embeds a 1.3 MiB journal). Tracking is
// switched on only around the submit/quote/fill loop, and startup allocations
// are attributed separately so they cannot hide a per-event allocation.

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
    exec::BrokerConfig config{};
    config.run_id = 1;
    config.run_seed = 7;
    config.mode = exec::SimulationMode::base;
    config.hedging = true;
    config.initial_balance_minor = 1'000'000'000;

    // Startup allocation, measured and reported separately.
    const auto startup_before = allocations.load(std::memory_order_relaxed);
    tracking.store(true, std::memory_order_relaxed);
    auto broker = std::make_unique<exec::PaperBroker>(config);
    const auto startup_allocations =
        allocations.load(std::memory_order_relaxed) - startup_before;

    exec::SymbolSpec spec{};
    spec.symbol_id = 0;
    spec.tick_size_ticks = 1;
    spec.volume_min = 1;
    spec.volume_max = 1'000'000;
    spec.volume_step = 1;
    spec.contract_size = 100'000;
    spec.tick_value_minor = 1;
    spec.commission_per_lot_minor = 700;
    spec.margin_rate_bp = 100;
    spec.tradable = true;
    broker->set_symbol(spec);

    // ---- steady-state loop -------------------------------------------------
    const auto steady_before = allocations.load(std::memory_order_relaxed);

    std::uint64_t ts = 1'000'000;
    std::uint64_t submitted = 0;
    for (std::uint64_t i = 0; i < 200'000; ++i) {
        const std::int64_t drift = static_cast<std::int64_t>(i % 40);
        broker->on_quote(0, 1'000 + drift, 1'010 + drift, 500, 500, ts);
        ts += 1'000;

        // Cycle orders so the live-order array is continuously reused rather
        // than filled once and left static.
        if ((i % 16) == 0 && !broker->halted()) {
            exec::OrderRequest request{};
            request.corr_id = i;
            request.strategy_id = 1;
            request.symbol_id = 0;
            request.side = ((submitted % 2) == 0) ? exec::Side::buy : exec::Side::sell;
            request.type = exec::OrderType::market;
            request.volume = 1;
            request.seq_global = submitted;
            if (broker->submit(request, ts).accepted) ++submitted;
            ts += 1'000;
        }
    }

    const auto steady_allocations =
        allocations.load(std::memory_order_relaxed) - steady_before;
    tracking.store(false, std::memory_order_relaxed);

    std::cout << "exec_startup_allocations=" << startup_allocations << '\n';
    std::cout << "exec_steady_state_allocations=" << steady_allocations << '\n';
    std::cout << "exec_orders_submitted=" << submitted << '\n';
    std::cout << "exec_fills=" << broker->fills() << '\n';
    std::cout << "exec_journal_records=" << broker->journal().size() << '\n';

    int failures = 0;
    if (submitted == 0) { std::cout << "exec_orders_exercised=FAIL\n"; ++failures; }
    if (broker->fills() == 0) { std::cout << "exec_fills_exercised=FAIL\n"; ++failures; }

    if (steady_allocations != 0) {
        std::cout << "exec_zero_alloc_steady_state=FAIL\n";
        ++failures;
    } else {
        std::cout << "exec_zero_alloc_steady_state=pass\n";
    }

    // Startup must be O(1), not proportional to the work that followed.
    if (startup_allocations > 8) {
        std::cout << "exec_startup_allocation_bounded=FAIL\n";
        ++failures;
    } else {
        std::cout << "exec_startup_allocation_bounded=pass\n";
    }

    return failures == 0 ? 0 : 1;
}
