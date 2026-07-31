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

// Phase 5 — replay + simulator integration.
//
// Runs the recorded stream through the same ReplayAdapter, VirtualClock, Book
// Engine and Feature Engine as Phase 4, then feeds every quote to the Paper
// Execution Simulator. Part 9.1 makes this the Phase 5 exit criterion: "The
// Phase 4 determinism test is re-run as a Phase 5 exit criterion with the
// simulator in the loop."
//
// ORDER SCHEDULE IS A TEST HARNESS, NOT A STRATEGY. Orders are emitted on a
// fixed mechanical cadence with alternating sides. It reads no feature, forms
// no view, and is not alpha logic — its only job is to exercise the execution
// path deterministically. Phase 7 owns strategies.

namespace replay {

struct SimReplayResult final {
    std::uint64_t stream_digest{0};      // Phase 4 digest: ordering, book, features
    std::uint64_t journal_digest{0};     // Phase 5 digest: full execution journal
    std::uint64_t events{0};
    std::uint64_t quotes{0};
    std::uint64_t orders_submitted{0};
    std::uint64_t orders_accepted{0};
    std::uint64_t fills{0};
    std::uint64_t journal_records{0};
    std::int64_t balance_minor{0};
    std::int64_t equity_minor{0};
    std::int64_t margin_used_minor{0};
    std::int64_t free_margin_minor{0};
    std::int64_t realized_pnl_minor{0};
    std::int64_t unrealized_pnl_minor{0};
    std::uint32_t open_positions{0};
    bool stopped_out{false};
    bool halted{false};
    bool journal_overflowed{false};
    feed::ReplayError error{feed::ReplayError::none};
    bool ok{false};
};

struct SimReplayConfig final {
    exec::SimulationMode mode{exec::SimulationMode::pessimistic};
    bool hedging{true};
    std::uint64_t run_seed{20260731};
    std::uint64_t run_id{1};
    std::int64_t initial_balance_minor{1'000'000'000};
    std::uint64_t order_every_n_quotes{500};
    std::int64_t order_volume{1};
    std::uint64_t min_return_samples{32};
};

class SimReplayEngine final {
public:
    explicit SimReplayEngine(SimReplayConfig config = {}) noexcept : config_(config) {}

    [[nodiscard]] SimReplayResult run(const std::filesystem::path& directory) noexcept {
        SimReplayResult result{};

        feed::ReplayAdapter adapter;
        if (!adapter.open(directory)) { result.error = adapter.error(); return result; }

        std::array<book::OrderBook, feed::mt5::symbol_count> books{};
        std::array<features::FeatureEngine, feed::mt5::symbol_count> engines{};
        for (std::size_t i = 0; i < books.size(); ++i) {
            books[i] = book::OrderBook(static_cast<std::uint32_t>(i));
            engines[i] = features::FeatureEngine(config_.min_return_samples);
        }

        exec::BrokerConfig broker_config{};
        broker_config.run_id = config_.run_id;
        broker_config.run_seed = config_.run_seed;
        broker_config.mode = config_.mode;
        broker_config.hedging = config_.hedging;
        broker_config.initial_balance_minor = config_.initial_balance_minor;
        broker_config.queue_validated = false;   // L1_ONLY recording -> NOT_VALIDATED
        exec::PaperBroker broker(broker_config);

        for (std::uint32_t i = 0; i < feed::mt5::symbol_count && i < exec::max_symbols; ++i) {
            exec::SymbolSpec spec{};
            spec.symbol_id = i;
            spec.tick_size_ticks = 1;
            spec.volume_min = 1;
            spec.volume_max = 1'000'000;
            spec.volume_step = 1;
            spec.contract_size = 100'000;
            spec.tick_value_minor = 1;
            spec.commission_per_lot_minor = 700;
            spec.margin_rate_bp = 100;
            spec.tradable = true;
            broker.set_symbol(spec);
        }

        Digest stream;
        core::FixedEvent event{};
        auto source = book::BookSource::l1_only;
        std::uint64_t quote_index = 0;
        std::uint64_t order_seq = 0;

        for (;;) {
            const auto poll = adapter.poll(event);
            if (poll == feed::PollResult::end_of_stream) break;
            if (poll == feed::PollResult::error) { result.error = adapter.error(); return result; }
            if (poll == feed::PollResult::idle) continue;

            ++result.events;
            stream.mix_header(event.header);

            const auto type = static_cast<core::EventType>(event.header.type);
            if (type == core::EventType::quote) {
                core::QuotePayload payload{};
                std::memcpy(&payload, event.payload.data(), sizeof(payload));
                const auto symbol = event.header.symbol_id;
                if (symbol >= books.size()) continue;

                (void)books[symbol].apply_quote(payload.bid, payload.ask, payload.bid_size,
                                                payload.ask_size, event.header.ts_local_ns);
                stream.mix(books[symbol].checksum());
                const auto vector = engines[symbol].compute(books[symbol], source);
                stream.mix(static_cast<std::uint64_t>(vector.warm_mask));
                for (std::size_t i = 0; i < features::feature_count; ++i)
                    stream.mix_double(vector.values[i]);

                // Simulator sees the same quote the book saw, at the same
                // virtual timestamp.
                broker.on_quote(symbol, payload.bid, payload.ask, payload.bid_size,
                                payload.ask_size, event.header.ts_local_ns);

                ++quote_index;
                ++result.quotes;

                if (config_.order_every_n_quotes > 0 &&
                    (quote_index % config_.order_every_n_quotes) == 0) {
                    exec::OrderRequest request{};
                    request.corr_id = event.header.seq_global;
                    request.strategy_id = 1;
                    request.symbol_id = symbol;
                    request.side = ((order_seq % 2) == 0) ? exec::Side::buy : exec::Side::sell;
                    request.type = exec::OrderType::market;
                    request.volume = config_.order_volume;
                    request.seq_global = order_seq;
                    ++order_seq;
                    ++result.orders_submitted;
                    if (broker.submit(request, event.header.ts_local_ns).accepted)
                        ++result.orders_accepted;
                }
            } else if (type == core::EventType::heartbeat) {
                core::HeartbeatPayload payload{};
                std::memcpy(&payload, event.payload.data(), sizeof(payload));
                stream.mix(static_cast<std::uint64_t>(payload.book_source));
                if (payload.book_source <= static_cast<std::uint8_t>(book::BookSource::l3_mbo))
                    source = static_cast<book::BookSource>(payload.book_source);
            }
        }

        result.stream_digest = stream.value();
        result.journal_digest = broker.journal().digest();
        result.journal_records = broker.journal().size();
        result.journal_overflowed = broker.journal().overflowed();
        result.fills = broker.fills();
        result.balance_minor = broker.account().balance_minor;
        result.equity_minor = broker.account().equity_minor;
        result.margin_used_minor = broker.account().margin_used_minor;
        result.free_margin_minor = broker.account().free_margin_minor;
        result.realized_pnl_minor = broker.account().realized_pnl_minor;
        result.unrealized_pnl_minor = broker.account().unrealized_pnl_minor;
        result.open_positions = broker.account().open_positions;
        result.stopped_out = broker.account().stopped_out;
        result.halted = broker.halted();
        result.error = feed::ReplayError::none;
        result.ok = true;
        return result;
    }

private:
    SimReplayConfig config_{};
};

}  // namespace replay
