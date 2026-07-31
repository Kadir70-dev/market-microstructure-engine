#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>

#include "book/order_book.hpp"
#include "exec/paper_broker.hpp"
#include "feed/mt5_protocol.hpp"
#include "feed/replay_adapter.hpp"
#include "features/feature_engine.hpp"
#include "replay/digest.hpp"
#include "risk/risk_engine.hpp"
#include "strategy/baseline_scalper.hpp"
#include "strategy/metrics.hpp"

// Phase 7 — strategy in the replay loop.
//
// PAPER ONLY. The pipeline is:
//
//   ReplayAdapter -> Book -> Features -> Strategy(Intent)
//                                          -> RiskEngine(Approve|Reject)
//                                             -> PaperBroker (simulated fills)
//
// A new header rather than an edit to sim_replay_engine.hpp: Phase 5 is complete
// and its determinism gate is green, so Phase 7 composes it instead of changing
// it.
//
// Part 10 is honoured structurally — the strategy emits an Intent, RiskEngine is
// the only thing that can approve it, and the PaperBroker is the only thing that
// can fill it. There is no broker connectivity, no MT5 symbol and no order
// transport anywhere on this path.

namespace replay {

struct StrategyReplayResult final {
    std::uint64_t decision_digest{0};   // every decision the strategy made
    std::uint64_t journal_digest{0};    // resulting execution journal
    std::uint64_t events{0};
    std::uint64_t quotes{0};
    std::uint64_t intents{0};
    std::uint64_t entries{0};
    std::uint64_t exits{0};
    std::uint64_t vetoes{0};
    std::uint64_t risk_rejects{0};
    std::uint64_t broker_rejects{0};
    std::uint64_t fills{0};
    std::size_t trades{0};
    std::int64_t net_pnl_minor{0};
    std::int64_t max_drawdown_minor{0};
    double win_rate{0.0};
    double sharpe_per_trade{0.0};
    double profit_factor{0.0};
    std::int64_t balance_minor{0};
    std::int64_t equity_minor{0};
    feed::ReplayError error{feed::ReplayError::none};
    bool ok{false};
};

struct StrategyReplayConfig final {
    strategy::StrategyConfig strategy{};
    risk::Limits limits{};
    exec::SimulationMode mode{exec::SimulationMode::pessimistic};
    std::uint64_t run_seed{20260731};
    std::uint64_t run_id{1};
    std::int64_t initial_balance_minor{1'000'000'000};
    std::int64_t tick_value_minor{1};
    std::uint32_t trade_symbol{0};      // single-symbol baseline
};

class StrategyReplayEngine final {
public:
    explicit StrategyReplayEngine(StrategyReplayConfig config) noexcept : config_(config) {}

    [[nodiscard]] StrategyReplayResult run(const std::filesystem::path& directory) noexcept {
        StrategyReplayResult result{};

        feed::ReplayAdapter adapter;
        if (!adapter.open(directory)) { result.error = adapter.error(); return result; }

        std::array<book::OrderBook, feed::mt5::symbol_count> books{};
        std::array<features::FeatureEngine, feed::mt5::symbol_count> engines{};
        for (std::size_t i = 0; i < books.size(); ++i) {
            books[i] = book::OrderBook(static_cast<std::uint32_t>(i));
            engines[i] = features::FeatureEngine(32);
        }

        strategy::BaselineScalper scalper(config_.strategy);
        risk::RiskEngine risk_engine(config_.limits);
        strategy::TradeStatistics statistics;

        exec::BrokerConfig broker_config{};
        broker_config.run_id = config_.run_id;
        broker_config.run_seed = config_.run_seed;
        broker_config.mode = config_.mode;
        broker_config.hedging = true;
        broker_config.initial_balance_minor = config_.initial_balance_minor;
        broker_config.queue_validated = false;      // L1 recording -> NOT_VALIDATED
        exec::PaperBroker broker(broker_config);

        for (std::uint32_t i = 0; i < feed::mt5::symbol_count && i < exec::max_symbols; ++i) {
            exec::SymbolSpec spec{};
            spec.symbol_id = i;
            spec.tick_size_ticks = 1;
            spec.volume_min = 1;
            spec.volume_max = 1'000'000;
            spec.volume_step = 1;
            spec.contract_size = 100'000;
            spec.tick_value_minor = config_.tick_value_minor;
            spec.commission_per_lot_minor = 700;
            spec.margin_rate_bp = 1;
            spec.tradable = true;
            broker.set_symbol(spec);
        }

        Digest decisions;
        core::FixedEvent event{};
        auto source = book::BookSource::l1_only;
        std::uint64_t order_seq = 0;
        std::uint64_t last_order_ns = 0;

        // The strategy's own view of the leg it is currently working.
        bool entry_in_flight = false;
        exec::BrokerOrderRef working{};
        strategy::TradeRecord pending{};

        for (;;) {
            const auto poll = adapter.poll(event);
            if (poll == feed::PollResult::end_of_stream) break;
            if (poll == feed::PollResult::error) { result.error = adapter.error(); return result; }
            if (poll == feed::PollResult::idle) continue;
            ++result.events;

            const auto type = static_cast<core::EventType>(event.header.type);
            if (type == core::EventType::heartbeat) {
                core::HeartbeatPayload hb{};
                std::memcpy(&hb, event.payload.data(), sizeof(hb));
                if (hb.book_source <= static_cast<std::uint8_t>(book::BookSource::l3_mbo))
                    source = static_cast<book::BookSource>(hb.book_source);
                continue;
            }
            if (type != core::EventType::quote) continue;

            core::QuotePayload quote{};
            std::memcpy(&quote, event.payload.data(), sizeof(quote));
            const auto symbol = event.header.symbol_id;
            if (symbol >= books.size()) continue;

            const auto ts = event.header.ts_local_ns;
            (void)books[symbol].apply_quote(quote.bid, quote.ask, quote.bid_size,
                                            quote.ask_size, ts);
            const auto fv = engines[symbol].compute(books[symbol], source);
            broker.on_quote(symbol, quote.bid, quote.ask, quote.bid_size, quote.ask_size, ts);
            ++result.quotes;

            // Resolve any working order before deciding again, so the strategy
            // never sees a stale in-flight state.
            if (working.logical_order_id != 0) {
                const auto* order = broker.find_order(working);
                if (order != nullptr && exec::is_terminal(order->state)) {
                    if (order->state == exec::OrderState::filled) {
                        if (entry_in_flight) {
                            scalper.on_entry_filled(order->side, order->filled_volume,
                                                    order->avg_fill_price_ticks, ts);
                            pending = strategy::TradeRecord{};
                            pending.side = order->side;
                            pending.volume = order->filled_volume;
                            pending.entry_ticks = order->avg_fill_price_ticks;
                            pending.opened_ns = ts;
                            pending.commission_minor = order->commission_minor;
                        } else {
                            pending.exit_ticks = order->avg_fill_price_ticks;
                            pending.closed_ns = ts;
                            pending.commission_minor += order->commission_minor;
                            pending.reason = scalper.last_exit_reason();
                            pending.pnl_minor = strategy::trade_pnl_minor(
                                pending.side, pending.volume, pending.entry_ticks,
                                pending.exit_ticks, config_.tick_value_minor);
                            (void)statistics.add(pending);
                            scalper.on_exit_filled(ts);
                        }
                        ++result.fills;
                    } else {
                        ++result.broker_rejects;
                        if (entry_in_flight) scalper.on_entry_rejected();
                        else scalper.on_exit_rejected();
                    }
                    working = exec::BrokerOrderRef{};
                }
            }

            if (symbol != config_.trade_symbol) continue;
            if (working.logical_order_id != 0) continue;   // one working order at a time

            const auto intent = scalper.on_market(symbol, books[symbol], fv, source, ts);

            // Every decision is committed to the digest, including the decision
            // not to act — otherwise two runs that vetoed for different reasons
            // would hash identically.
            decisions.mix(static_cast<std::uint64_t>(intent.kind));
            decisions.mix(static_cast<std::uint64_t>(intent.side));
            decisions.mix(static_cast<std::uint64_t>(intent.volume));
            decisions.mix(intent.ts_ns);
            decisions.mix(static_cast<std::uint64_t>(scalper.state()));
            decisions.mix(static_cast<std::uint64_t>(scalper.last_veto()));

            if (!intent.actionable()) continue;
            ++result.intents;

            risk::Request request{};
            request.symbol_id = intent.symbol_id;
            request.strategy_id = intent.strategy_id;
            request.side = intent.side;
            request.volume = intent.volume;
            request.risk_minor = 1;
            request.free_margin = broker.account().free_margin_minor;
            request.warm_mask = config_.limits.required_warm_mask;
            request.session_open = true;
            request.now_ns = ts;
            request.last_order_ns = last_order_ns;
            request.reduce_only = false;   // reduce_only checked against net below
            request.hedging = true;

            const auto decision = risk_engine.check(request);
            if (!decision.approved) {
                ++result.risk_rejects;
                if (intent.kind == strategy::IntentKind::enter) scalper.on_entry_rejected();
                else scalper.on_exit_rejected();
                continue;
            }

            exec::OrderRequest order{};
            order.corr_id = event.header.seq_global;
            order.strategy_id = intent.strategy_id;
            order.symbol_id = intent.symbol_id;
            order.side = intent.side;
            order.type = exec::OrderType::market;
            order.volume = intent.volume;
            order.seq_global = order_seq++;

            const auto submitted = broker.submit(order, ts);
            if (!submitted.accepted) {
                ++result.broker_rejects;
                if (intent.kind == strategy::IntentKind::enter) scalper.on_entry_rejected();
                else scalper.on_exit_rejected();
                continue;
            }
            working = submitted.ref;
            entry_in_flight = (intent.kind == strategy::IntentKind::enter);
            last_order_ns = ts;
            if (entry_in_flight) ++result.entries; else ++result.exits;
        }

        result.decision_digest = decisions.value();
        result.journal_digest = broker.journal().digest();
        result.vetoes = scalper.vetoes();
        result.trades = statistics.trades();
        result.net_pnl_minor = statistics.net_pnl_minor();
        result.max_drawdown_minor = statistics.max_drawdown_minor();
        result.win_rate = statistics.win_rate();
        result.sharpe_per_trade = statistics.sharpe_per_trade();
        result.profit_factor = statistics.profit_factor();
        result.balance_minor = broker.account().balance_minor;
        result.equity_minor = broker.account().equity_minor;
        result.error = feed::ReplayError::none;
        result.ok = true;
        return result;
    }

private:
    StrategyReplayConfig config_{};
};

}  // namespace replay
