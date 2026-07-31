#pragma once

#include <cmath>
#include <cstdint>

#include "book/order_book.hpp"
#include "features/feature_engine.hpp"

// Phase 7 — microstructure signals.
//
// Computed in include/strategy/ rather than added to the Phase 3 feature engine:
// Phase 3 is complete and its determinism gate is green, so Phase 7 consumes it
// instead of editing it.
//
// Every signal is a pure function of book state (plus, for OFI, the immediately
// preceding book state). No wall-clock read, no allocation, no container.
//
// ⚠ NOT_VALIDATED on L1_ONLY / DOM_* sources: queue imbalance and OFI both need
//   genuine displayed size. MT5 forex publishes volume 0 at L1, so on that feed
//   these signals are structurally undefined and report themselves cold rather
//   than returning a plausible zero.

namespace strategy {

// One side of the top of book, as seen at a point in time. OFI is defined on the
// transition between two of these, so the previous snapshot must be retained.
struct TopOfBook final {
    std::int64_t bid_ticks{0};
    std::int64_t ask_ticks{0};
    std::int64_t bid_size{0};
    std::int64_t ask_size{0};
    bool valid{false};

    [[nodiscard]] constexpr std::int64_t spread_ticks() const noexcept {
        return ask_ticks - bid_ticks;
    }
    [[nodiscard]] constexpr bool has_size() const noexcept { return bid_size > 0 || ask_size > 0; }
};

[[nodiscard]] inline TopOfBook top_of_book(const book::OrderBook& b) noexcept {
    TopOfBook top{};
    if (!b.has_both_sides()) return top;
    const auto bid = b.best(book::Side::bid);
    const auto ask = b.best(book::Side::ask);
    top.bid_ticks = bid.price_ticks;
    top.ask_ticks = ask.price_ticks;
    top.bid_size = bid.size;
    top.ask_size = ask.size;
    top.valid = true;
    return top;
}

// Which signals are usable. A cold signal is unavailable, not zero — the same
// discipline as the Phase 3 warm mask, and for the same reason: a strategy that
// reads 0.0 from an undefined signal will happily trade on it.
enum class Signal : std::uint32_t {
    book_imbalance = 0,
    microprice = 1,
    ofi = 2,
    spread = 3,
    queue_imbalance = 4,
    momentum = 5,
    volatility = 6
};

inline constexpr std::size_t signal_count = 7;

[[nodiscard]] constexpr std::uint32_t signal_bit(Signal s) noexcept {
    return 1U << static_cast<std::uint32_t>(s);
}

struct SignalSet final {
    double book_imbalance{0.0};   // (bid_depth - ask_depth) / total, [-1, 1]
    double microprice{0.0};       // size-weighted top of book, in ticks
    double ofi{0.0};              // Cont-Kukanov-Stoikov order flow imbalance
    double spread{0.0};           // ticks
    double queue_imbalance{0.0};  // (bid_size - ask_size) / total at L1, [-1, 1]
    double momentum{0.0};         // mid return over the feature window
    double volatility{0.0};       // realised stddev of mid log-returns
    std::uint32_t warm{0};

    [[nodiscard]] bool ready(Signal s) const noexcept { return (warm & signal_bit(s)) != 0; }
};

// Order Flow Imbalance, Cont / Kukanov / Stoikov. The canonical form:
//
//   e = 1{Pb_n >= Pb_{n-1}}*qb_n - 1{Pb_n <= Pb_{n-1}}*qb_{n-1}
//     - 1{Pa_n <= Pa_{n-1}}*qa_n + 1{Pa_n >= Pa_{n-1}}*qa_{n-1}
//
// It is deliberately not "delta of imbalance": OFI attributes size changes to
// the side that caused them, distinguishing a bid stepping up (buying pressure)
// from an ask being cancelled (also lifting price, but different information).
[[nodiscard]] inline double order_flow_imbalance(const TopOfBook& previous,
                                                 const TopOfBook& current) noexcept {
    if (!previous.valid || !current.valid) return 0.0;
    double e = 0.0;
    if (current.bid_ticks >= previous.bid_ticks) e += static_cast<double>(current.bid_size);
    if (current.bid_ticks <= previous.bid_ticks) e -= static_cast<double>(previous.bid_size);
    if (current.ask_ticks <= previous.ask_ticks) e -= static_cast<double>(current.ask_size);
    if (current.ask_ticks >= previous.ask_ticks) e += static_cast<double>(previous.ask_size);
    return e;
}

[[nodiscard]] inline double queue_imbalance(const TopOfBook& top) noexcept {
    const auto total = top.bid_size + top.ask_size;
    if (total <= 0) return 0.0;
    return static_cast<double>(top.bid_size - top.ask_size) / static_cast<double>(total);
}

// Depth-weighted imbalance across the whole tracked ladder. Distinct from queue
// imbalance, which is top-of-book only.
[[nodiscard]] inline double book_imbalance(const book::OrderBook& b) noexcept {
    double bid_total = 0.0;
    double ask_total = 0.0;
    for (std::size_t i = 0; i < b.depth(book::Side::bid); ++i)
        bid_total += static_cast<double>(b.level(book::Side::bid, i).size);
    for (std::size_t i = 0; i < b.depth(book::Side::ask); ++i)
        ask_total += static_cast<double>(b.level(book::Side::ask, i).size);
    const auto total = bid_total + ask_total;
    if (total <= 0.0) return 0.0;
    return (bid_total - ask_total) / total;
}

// Computes the Phase 7 signal set. `features` supplies the Phase 3 outputs that
// already exist (microprice, momentum proxy, realised volatility) so nothing is
// recomputed twice and the two layers cannot disagree.
class SignalEngine final {
public:
    [[nodiscard]] SignalSet compute(const book::OrderBook& b,
                                    const features::FeatureVector& fv,
                                    book::BookSource source) noexcept {
        SignalSet out{};
        const auto top = top_of_book(b);
        if (!top.valid) { previous_ = top; return out; }

        // Spread is available from any two-sided book.
        out.spread = static_cast<double>(top.spread_ticks());
        out.warm |= signal_bit(Signal::spread);

        // Size-derived signals require displayed size. Cold, not zero, when the
        // venue publishes none.
        if (top.has_size()) {
            out.queue_imbalance = queue_imbalance(top);
            out.warm |= signal_bit(Signal::queue_imbalance);

            out.book_imbalance = book_imbalance(b);
            out.warm |= signal_bit(Signal::book_imbalance);

            if (previous_.valid && previous_.has_size()) {
                out.ofi = order_flow_imbalance(previous_, top);
                out.warm |= signal_bit(Signal::ofi);
            }
        }

        // Depth-derived signals are meaningless without a depth-bearing source.
        if (static_cast<std::uint8_t>(source) <
            static_cast<std::uint8_t>(book::BookSource::dom_aggregated))
            out.warm &= ~signal_bit(Signal::book_imbalance);

        if (fv.warm(features::Feature::microprice)) {
            out.microprice = fv.value(features::Feature::microprice);
            out.warm |= signal_bit(Signal::microprice);
        }
        if (fv.warm(features::Feature::return_mean)) {
            out.momentum = fv.value(features::Feature::return_mean);
            out.warm |= signal_bit(Signal::momentum);
        }
        if (fv.warm(features::Feature::return_stddev)) {
            out.volatility = fv.value(features::Feature::return_stddev);
            out.warm |= signal_bit(Signal::volatility);
        }

        // Non-finite can only arrive from upstream; refuse it rather than let a
        // NaN comparison silently evaluate false and look like "no signal".
        if (!finite(out)) return SignalSet{};

        previous_ = top;
        return out;
    }

    void reset() noexcept { previous_ = TopOfBook{}; }
    [[nodiscard]] const TopOfBook& previous() const noexcept { return previous_; }

private:
    [[nodiscard]] static bool finite(const SignalSet& s) noexcept {
        return std::isfinite(s.book_imbalance) && std::isfinite(s.microprice) &&
               std::isfinite(s.ofi) && std::isfinite(s.spread) &&
               std::isfinite(s.queue_imbalance) && std::isfinite(s.momentum) &&
               std::isfinite(s.volatility);
    }

    TopOfBook previous_{};
};

}  // namespace strategy
