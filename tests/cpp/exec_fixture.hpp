#pragma once

#include <cstdint>
#include <iostream>

#include "exec/paper_broker.hpp"

// Shared Phase 5 test scaffolding. Deliberately tiny: every test builds its own
// broker so no state can leak between cases.

namespace exec_fixture {

inline int failures = 0;

inline void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}

// A symbol with sizes published, so queue and partial-fill mechanics are
// exercisable. Real MT5 forex publishes no L1 size; that case is covered
// separately where it matters.
inline exec::SymbolSpec sized_symbol(std::uint32_t symbol_id = 0) noexcept {
    exec::SymbolSpec spec{};
    spec.symbol_id = symbol_id;
    spec.tick_size_ticks = 1;
    spec.volume_min = 1;
    spec.volume_max = 1'000'000;
    spec.volume_step = 1;
    spec.contract_size = 100;
    spec.tick_value_minor = 1;
    spec.commission_per_lot_minor = 0;
    spec.margin_rate_bp = 100;      // 1% of notional
    spec.tradable = true;
    return spec;
}

inline exec::BrokerConfig config(exec::SimulationMode mode = exec::SimulationMode::optimistic,
                                 bool hedging = true,
                                 std::int64_t balance_minor = 100'000'000) noexcept {
    exec::BrokerConfig cfg{};
    cfg.run_id = 1;
    cfg.run_seed = 20260731;
    cfg.mode = mode;
    cfg.hedging = hedging;
    cfg.initial_balance_minor = balance_minor;
    cfg.queue_validated = false;    // L1 source -> NOT_VALIDATED
    return cfg;
}

inline exec::OrderRequest market(std::uint32_t symbol_id, exec::Side side, std::int64_t volume,
                                 std::uint64_t seq) noexcept {
    exec::OrderRequest request{};
    request.corr_id = seq;
    request.strategy_id = 1;
    request.symbol_id = symbol_id;
    request.side = side;
    request.type = exec::OrderType::market;
    request.volume = volume;
    request.seq_global = seq;
    return request;
}

inline exec::OrderRequest limit(std::uint32_t symbol_id, exec::Side side, std::int64_t volume,
                                std::int64_t price, std::uint64_t seq) noexcept {
    auto request = market(symbol_id, side, volume, seq);
    request.type = exec::OrderType::limit;
    request.limit_price_ticks = price;
    return request;
}

}  // namespace exec_fixture
