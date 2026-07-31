#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "book/order_book.hpp"

// Phase 7 — L1-only microstructure signals for MT5 forex.
//
// Deliberately separate from strategy/signals.hpp, which computes queue
// imbalance and OFI. Those need displayed size; MT5 forex publishes volume 0 at
// top of book, so on that feed they are structurally undefined and the
// depth-based baseline correctly declines every quote.
//
// Everything here is derived from (bid, ask, timestamp) alone. No volume is
// read, no depth is synthesised, and no field is defaulted to a plausible zero:
// a signal that has not warmed reports itself cold.
//
// Fixed-capacity rings, no allocation, no container. All state is per-symbol and
// caller-owned.

namespace strategy {

inline constexpr std::size_t l1_return_window = 32;
inline constexpr std::size_t l1_spread_window = 64;

enum class L1Signal : std::uint32_t {
    spread = 0,             // ticks
    spread_ratio = 1,       // spread / rolling mean spread  (expansion > 1)
    mid_return = 2,         // most recent log return of the mid
    momentum = 3,           // mean log return over the window
    acceleration = 4,       // change in momentum between successive windows
    volatility = 5,         // stddev of log returns over the window
    quote_rate = 6          // quotes per second
};

inline constexpr std::size_t l1_signal_count = 7;

[[nodiscard]] constexpr std::uint32_t l1_bit(L1Signal s) noexcept {
    return 1U << static_cast<std::uint32_t>(s);
}

struct L1SignalSet final {
    double spread{0.0};
    double spread_ratio{1.0};
    double mid_return{0.0};
    double momentum{0.0};
    double acceleration{0.0};
    double volatility{0.0};
    double quote_rate{0.0};
    std::uint32_t warm{0};

    [[nodiscard]] bool ready(L1Signal s) const noexcept { return (warm & l1_bit(s)) != 0; }
};

class L1SignalEngine final {
public:
    // `bid`/`ask` in ticks, `ts_ns` the recorded local timestamp. Returns the
    // signal set for this quote.
    [[nodiscard]] L1SignalSet update(std::int64_t bid_ticks, std::int64_t ask_ticks,
                                     std::uint64_t ts_ns) noexcept {
        L1SignalSet out{};
        if (bid_ticks <= 0 || ask_ticks <= 0 || ask_ticks <= bid_ticks) return out;

        const double spread = static_cast<double>(ask_ticks - bid_ticks);
        const double mid = (static_cast<double>(bid_ticks) + static_cast<double>(ask_ticks)) * 0.5;

        out.spread = spread;
        out.warm |= l1_bit(L1Signal::spread);

        // ---- rolling spread mean, for expansion / contraction --------------
        spread_sum_ += spread;
        if (spread_count_ < l1_spread_window) {
            spreads_[spread_count_++] = spread;
        } else {
            spread_sum_ -= spreads_[spread_head_];
            spreads_[spread_head_] = spread;
            spread_head_ = (spread_head_ + 1) % l1_spread_window;
        }
        if (spread_count_ >= 8) {
            const double mean = spread_sum_ / static_cast<double>(spread_count_);
            if (mean > 0.0) {
                out.spread_ratio = spread / mean;
                out.warm |= l1_bit(L1Signal::spread_ratio);
            }
        }

        // ---- returns -------------------------------------------------------
        if (have_previous_ && previous_mid_ > 0.0 && mid > 0.0) {
            const double ratio = mid / previous_mid_;
            if (ratio > 0.0) {
                const double log_return = std::log(ratio);
                out.mid_return = log_return;
                out.warm |= l1_bit(L1Signal::mid_return);

                return_sum_ += log_return;
                if (return_count_ < l1_return_window) {
                    returns_[return_count_++] = log_return;
                } else {
                    return_sum_ -= returns_[return_head_];
                    returns_[return_head_] = log_return;
                    return_head_ = (return_head_ + 1) % l1_return_window;
                }

                if (return_count_ >= l1_return_window) {
                    const double n = static_cast<double>(return_count_);
                    const double momentum = return_sum_ / n;
                    out.momentum = momentum;
                    out.warm |= l1_bit(L1Signal::momentum);

                    // Two-pass variance over a bounded window: exact, and immune
                    // to the cancellation a running sum-of-squares suffers.
                    double variance = 0.0;
                    for (std::size_t i = 0; i < return_count_; ++i) {
                        const double d = returns_[i] - momentum;
                        variance += d * d;
                    }
                    variance /= (n > 1.0) ? (n - 1.0) : 1.0;
                    if (variance >= 0.0) {
                        out.volatility = std::sqrt(variance);
                        out.warm |= l1_bit(L1Signal::volatility);
                    }

                    if (have_previous_momentum_) {
                        out.acceleration = momentum - previous_momentum_;
                        out.warm |= l1_bit(L1Signal::acceleration);
                    }
                    previous_momentum_ = momentum;
                    have_previous_momentum_ = true;
                }
            }
        }
        previous_mid_ = mid;
        have_previous_ = true;

        // ---- quote arrival rate -------------------------------------------
        // A liquidity proxy that needs no volume: how fast the venue is
        // publishing. Uses the recorded timestamp, never the wall clock, so it
        // is identical under replay at any speed.
        if (ts_count_ == 0) {
            first_ts_ns_ = ts_ns;
        }
        last_ts_ns_ = ts_ns;
        ++ts_count_;
        if (ts_count_ > 1 && last_ts_ns_ > first_ts_ns_) {
            const double seconds = static_cast<double>(last_ts_ns_ - first_ts_ns_) / 1e9;
            if (seconds > 0.0) {
                out.quote_rate = static_cast<double>(ts_count_) / seconds;
                out.warm |= l1_bit(L1Signal::quote_rate);
            }
        }
        // Bound the rate window so it tracks current conditions rather than the
        // whole session average.
        if (ts_count_ >= 512) {
            ts_count_ = 1;
            first_ts_ns_ = ts_ns;
        }

        if (!finite(out)) return L1SignalSet{};
        return out;
    }

    void reset() noexcept {
        spread_count_ = spread_head_ = 0;
        spread_sum_ = 0.0;
        return_count_ = return_head_ = 0;
        return_sum_ = 0.0;
        previous_mid_ = 0.0;
        have_previous_ = false;
        previous_momentum_ = 0.0;
        have_previous_momentum_ = false;
        ts_count_ = 0;
        first_ts_ns_ = last_ts_ns_ = 0;
    }

private:
    [[nodiscard]] static bool finite(const L1SignalSet& s) noexcept {
        return std::isfinite(s.spread) && std::isfinite(s.spread_ratio) &&
               std::isfinite(s.mid_return) && std::isfinite(s.momentum) &&
               std::isfinite(s.acceleration) && std::isfinite(s.volatility) &&
               std::isfinite(s.quote_rate);
    }

    std::array<double, l1_spread_window> spreads_{};
    std::size_t spread_count_{0}, spread_head_{0};
    double spread_sum_{0.0};

    std::array<double, l1_return_window> returns_{};
    std::size_t return_count_{0}, return_head_{0};
    double return_sum_{0.0};

    double previous_mid_{0.0};
    bool have_previous_{false};
    double previous_momentum_{0.0};
    bool have_previous_momentum_{false};

    std::uint64_t ts_count_{0}, first_ts_ns_{0}, last_ts_ns_{0};
};

}  // namespace strategy
