#pragma once

#include <array>
#include <cstdint>
#include <fstream>
#include <string>

#include "strategy/l1_signals.hpp"
#include "strategy/strategy_types.hpp"

// Online learning over completed paper trades.
//
// ARCHITECTURE Part 4 governs what this may emit: "ML may output probabilities,
// regime labels, feature weights, parameter sets, confidence scores. ML may
// never output an order, a size, or a risk-limit change."
//
// So this NEVER writes back into the running strategy, never touches risk
// limits, and never clears a halt. It accumulates evidence and writes a
// PROPOSAL to disk for a human to accept or reject. Auto-applying parameters
// from a handful of live trades is precisely how a system fits noise and then
// trades it.
//
// Attribution is by conditioning: each completed trade is bucketed on the signal
// values that were present when its entry was submitted, and per-bucket
// expectancy is accumulated. A bucket only becomes advisory once it has enough
// samples to be worth anything.

namespace learn {

inline constexpr std::size_t bucket_count = 4;
inline constexpr std::size_t signal_count = 5;   // momentum, volatility, spread, spread_ratio, quote_rate

struct TradeObservation final {
    double momentum{0.0};
    double volatility{0.0};
    double spread{0.0};
    double spread_ratio{1.0};
    double quote_rate{0.0};
    double acceleration{0.0};
    int side{0};                    // +1 long, -1 short
    std::int64_t net_minor{0};
    std::uint64_t hold_ns{0};
    strategy::ExitReason exit_reason{strategy::ExitReason::none};
};

struct BucketStat final {
    std::uint64_t count{0}, wins{0};
    std::int64_t net{0};
    [[nodiscard]] double win_rate() const noexcept {
        return count ? static_cast<double>(wins) / static_cast<double>(count) : 0.0;
    }
    [[nodiscard]] double expectancy() const noexcept {
        return count ? static_cast<double>(net) / static_cast<double>(count) : 0.0;
    }
};

// Fixed edges rather than running quantiles: with a handful of trades, quantile
// boundaries move under every new sample and the buckets stop being comparable
// across time. Fixed edges keep the accumulated evidence meaningful.
inline constexpr std::array<std::array<double, 3>, signal_count> bucket_edges{{
    {{1.0e-6, 3.0e-6, 1.0e-5}},   // |momentum|
    {{1.0e-6, 1.0e-5, 3.0e-5}},   // volatility
    {{10.0, 15.0, 25.0}},         // spread ticks
    {{0.95, 1.10, 1.50}},         // spread ratio
    {{1.0, 5.0, 20.0}}            // quote rate
}};

[[nodiscard]] inline std::size_t bucket_of(std::size_t signal, double value) noexcept {
    const auto& e = bucket_edges[signal];
    if (value < e[0]) return 0;
    if (value < e[1]) return 1;
    if (value < e[2]) return 2;
    return 3;
}

inline const char* signal_name(std::size_t i) noexcept {
    switch (i) {
        case 0: return "abs_momentum";
        case 1: return "volatility";
        case 2: return "spread";
        case 3: return "spread_ratio";
        case 4: return "quote_rate";
    }
    return "?";
}

class OnlineLearner final {
public:
    void add(const TradeObservation& t) noexcept {
        if (count_ >= capacity) { overflowed_ = true; return; }
        trades_[count_++] = t;
        total_net_ += t.net_minor;
        if (t.net_minor > 0) ++wins_; else if (t.net_minor < 0) ++losses_;
        switch (t.exit_reason) {
            case strategy::ExitReason::take_profit: ++tp_; break;
            case strategy::ExitReason::stop_loss:   ++sl_; break;
            case strategy::ExitReason::time_stop:   ++ts_; break;
            default: break;
        }
        if (t.side > 0) { ++long_count_; long_net_ += t.net_minor; }
        else if (t.side < 0) { ++short_count_; short_net_ += t.net_minor; }

        const double values[signal_count] = {
            t.momentum < 0.0 ? -t.momentum : t.momentum,
            t.volatility, t.spread, t.spread_ratio, t.quote_rate};
        for (std::size_t s = 0; s < signal_count; ++s) {
            auto& b = stats_[s][bucket_of(s, values[s])];
            ++b.count;
            if (t.net_minor > 0) ++b.wins;
            b.net += t.net_minor;
        }
    }

    [[nodiscard]] std::uint64_t trades() const noexcept { return count_; }
    [[nodiscard]] std::int64_t net() const noexcept { return total_net_; }
    [[nodiscard]] std::uint64_t wins() const noexcept { return wins_; }

    // Human-readable evidence, rewritten on each flush.
    void write_report(const std::string& path) const {
        std::ofstream f(path, std::ios::trunc);
        if (!f) return;
        f << "trades=" << count_ << " wins=" << wins_ << " losses=" << losses_
          << " net_minor=" << total_net_
          << " win_rate=" << (count_ ? static_cast<double>(wins_) / static_cast<double>(count_) : 0.0)
          << "\nexits tp=" << tp_ << " sl=" << sl_ << " time=" << ts_
          << "\nby_side long_n=" << long_count_ << " long_net=" << long_net_
          << " short_n=" << short_count_ << " short_net=" << short_net_ << "\n";
        for (std::size_t s = 0; s < signal_count; ++s) {
            f << signal_name(s) << ':';
            for (std::size_t b = 0; b < bucket_count; ++b)
                f << " b" << b << "(n=" << stats_[s][b].count
                  << ",wr=" << stats_[s][b].win_rate()
                  << ",exp=" << stats_[s][b].expectancy() << ')';
            f << '\n';
        }
        f << "overflowed=" << (overflowed_ ? 1 : 0) << '\n';
    }

    // Advisory only. Never applied automatically; a human reads this and edits
    // the parameter file if the evidence justifies it.
    void write_proposal(const std::string& path, std::uint64_t min_samples) const {
        std::ofstream f(path, std::ios::trunc);
        if (!f) return;
        f << "{\n  \"status\": \"ADVISORY_ONLY\",\n"
          << "  \"never_auto_applied\": true,\n"
          << "  \"basis\": \"ARCHITECTURE_V1 Part 4: ML may propose parameter sets, never orders, sizes or risk-limit changes\",\n"
          << "  \"trades_observed\": " << count_ << ",\n"
          << "  \"min_samples_per_bucket\": " << min_samples << ",\n"
          << "  \"net_minor\": " << total_net_ << ",\n";
        const bool enough = count_ >= min_samples;
        f << "  \"sufficient_evidence\": " << (enough ? "true" : "false") << ",\n";
        f << "  \"proposals\": [";
        bool first = true;
        if (enough) {
            for (std::size_t s = 0; s < signal_count; ++s) {
                std::size_t best = 0;
                bool have = false;
                for (std::size_t b = 0; b < bucket_count; ++b) {
                    if (stats_[s][b].count < min_samples) continue;
                    if (!have || stats_[s][b].expectancy() > stats_[s][best].expectancy()) {
                        best = b; have = true;
                    }
                }
                if (!have || stats_[s][best].expectancy() <= 0.0) continue;
                if (!first) f << ',';
                first = false;
                f << "\n    {\"signal\": \"" << signal_name(s)
                  << "\", \"favourable_bucket\": " << best
                  << ", \"samples\": " << stats_[s][best].count
                  << ", \"win_rate\": " << stats_[s][best].win_rate()
                  << ", \"expectancy_minor\": " << stats_[s][best].expectancy() << "}";
            }
        }
        f << (first ? "" : "\n  ") << "],\n";
        f << "  \"note\": \"Insufficient evidence produces an empty proposal list rather than a guess.\"\n}\n";
    }

private:
    static constexpr std::size_t capacity = 8192;
    std::array<TradeObservation, capacity> trades_{};
    std::array<std::array<BucketStat, bucket_count>, signal_count> stats_{};
    std::uint64_t count_{0}, wins_{0}, losses_{0}, tp_{0}, sl_{0}, ts_{0};
    std::uint64_t long_count_{0}, short_count_{0};
    std::int64_t total_net_{0}, long_net_{0}, short_net_{0};
    bool overflowed_{false};
};

}  // namespace learn
