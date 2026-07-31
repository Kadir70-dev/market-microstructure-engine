#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>

#include "book/order_book.hpp"
#include "core/event.hpp"
#include "feed/mt5_protocol.hpp"
#include "feed/replay_adapter.hpp"
#include "features/feature_engine.hpp"
#include "replay/digest.hpp"

// Phase 4 — Replay engine.
//
// Drives the recorded stream through the *same* Book Engine and Feature Engine
// used in live mode (Objective 2). There is no replay-specific book and no
// replay-specific feature path; the only substitution is IFeedAdapter, exactly
// as Part 3's central invariant requires.
//
// All state is function-local and fixed-size, so two runs cannot influence each
// other through residual state. That is what makes "100 consecutive runs" a
// meaningful test rather than a test of whether reset() was written correctly.

namespace replay {

struct ReplayResult final {
    std::uint64_t digest{0};
    std::uint64_t events{0};
    std::uint64_t quotes{0};
    std::uint64_t heartbeats{0};
    std::uint64_t other_events{0};
    std::uint64_t feature_vectors{0};
    std::uint64_t rejected_updates{0};
    std::uint64_t crossed_halts{0};
    std::uint64_t segments{0};
    std::uint64_t first_ts_local_ns{0};
    std::uint64_t last_ts_local_ns{0};
    feed::ReplayError error{feed::ReplayError::none};
    bool ok{false};

    [[nodiscard]] std::uint64_t span_ns() const noexcept {
        return last_ts_local_ns > first_ts_local_ns ? last_ts_local_ns - first_ts_local_ns : 0;
    }
};

class ReplayEngine final {
public:
    explicit ReplayEngine(std::uint64_t min_return_samples = 32) noexcept
        : min_return_samples_(min_return_samples) {}

    [[nodiscard]] ReplayResult run(const std::filesystem::path& directory) noexcept {
        ReplayResult result{};

        feed::ReplayAdapter adapter;
        if (!adapter.open(directory)) {
            result.error = adapter.error();
            return result;
        }

        std::array<book::OrderBook, feed::mt5::symbol_count> books{};
        std::array<features::FeatureEngine, feed::mt5::symbol_count> engines{};
        for (std::size_t i = 0; i < books.size(); ++i) {
            books[i] = book::OrderBook(static_cast<std::uint32_t>(i));
            engines[i] = features::FeatureEngine(min_return_samples_);
        }

        // Heartbeats carry book_source for symbol 0 only (HbRecord in the frozen
        // protocol). Treated as a run-wide hint rather than invented per symbol:
        // fabricating per-symbol sources the wire never sent would be a lie the
        // feature validity matrix would then act on.
        auto source = book::BookSource::l1_only;

        Digest digest;
        core::FixedEvent event{};

        for (;;) {
            const auto poll = adapter.poll(event);
            if (poll == feed::PollResult::end_of_stream) break;
            if (poll == feed::PollResult::error) {
                result.error = adapter.error();
                return result;
            }
            if (poll == feed::PollResult::idle) continue;

            ++result.events;
            digest.mix_header(event.header);

            const auto type = static_cast<core::EventType>(event.header.type);
            if (type == core::EventType::quote) {
                core::QuotePayload payload{};
                std::memcpy(&payload, event.payload.data(), sizeof(payload));
                digest.mix(static_cast<std::uint64_t>(payload.bid));
                digest.mix(static_cast<std::uint64_t>(payload.ask));
                digest.mix(static_cast<std::uint64_t>(payload.bid_size));
                digest.mix(static_cast<std::uint64_t>(payload.ask_size));
                digest.mix(static_cast<std::uint64_t>(payload.flags));

                const auto symbol = event.header.symbol_id;
                if (symbol >= books.size()) { ++result.other_events; continue; }

                const auto status = books[symbol].apply_quote(
                    payload.bid, payload.ask, payload.bid_size, payload.ask_size,
                    event.header.ts_local_ns);
                if (status == book::BookStatus::rejected) ++result.rejected_updates;
                if (status == book::BookStatus::crossed_halt) ++result.crossed_halts;
                digest.mix(static_cast<std::uint64_t>(status));
                digest.mix(books[symbol].checksum());

                const auto vector = engines[symbol].compute(books[symbol], source);
                digest.mix(static_cast<std::uint64_t>(vector.warm_mask));
                for (std::size_t i = 0; i < features::feature_count; ++i)
                    digest.mix_double(vector.values[i]);
                ++result.feature_vectors;
                ++result.quotes;
            } else if (type == core::EventType::heartbeat) {
                core::HeartbeatPayload payload{};
                std::memcpy(&payload, event.payload.data(), sizeof(payload));
                digest.mix(static_cast<std::uint64_t>(payload.book_source));
                digest.mix(static_cast<std::uint64_t>(payload.connected));
                if (payload.book_source <= static_cast<std::uint8_t>(book::BookSource::l3_mbo))
                    source = static_cast<book::BookSource>(payload.book_source);
                ++result.heartbeats;
            } else {
                ++result.other_events;
            }
        }

        result.digest = digest.value();
        result.segments = adapter.stats().segments_opened;
        result.first_ts_local_ns = adapter.stats().first_ts_local_ns;
        result.last_ts_local_ns = adapter.stats().last_ts_local_ns;
        result.error = feed::ReplayError::none;
        result.ok = true;
        return result;
    }

private:
    std::uint64_t min_return_samples_{32};
};

}  // namespace replay
