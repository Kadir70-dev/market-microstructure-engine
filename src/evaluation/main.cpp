#include <algorithm>
#include <cstdio>
#include <iostream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "evaluation/metrics.hpp"

namespace {

using Tick          = std::pair<long long, double>;
using TickSeries    = std::vector<Tick>;
using TicksBySymbol = std::map<std::string, TickSeries>;

// Round-trip cost per symbol, expressed as a fraction of price.
// Defaults reflect typical retail spreads on commission-free CFD accounts:
//   EUR/USD : ~1.5 pips / 1.16        = 1.29e-4   -> use 1.5e-4
//   XAU/USD : ~50 cents / 4500        = 1.11e-4   -> use 1.5e-4
//   USO     : ~3 cents  / 141         = 2.13e-4   -> use 2.0e-4
// Unknown symbol -> conservative 3e-4 default. Override these once you have
// observed spreads from the broker feed (Phase 3 of the roadmap).
struct CostModel {
    std::map<std::string, double> round_trip_by_symbol;
    double default_cost = 3.0e-4;

    double costFor(const std::string& sym) const {
        auto it = round_trip_by_symbol.find(sym);
        return it != round_trip_by_symbol.end() ? it->second : default_cost;
    }
};

CostModel defaultCostModel() {
    CostModel cm;
    cm.round_trip_by_symbol["EUR/USD"] = 1.5e-4;
    cm.round_trip_by_symbol["XAU/USD"] = 1.5e-4;
    cm.round_trip_by_symbol["USO"]     = 2.0e-4;
    return cm;
}

struct RawSignal {
    long long   ts;
    std::string symbol;
    std::string momentum;
    std::string vol_regime;
    int         confidence;
    int         stale;
};

TicksBySymbol loadTicks(sqlite3* db) {

    TicksBySymbol out;
    sqlite3_stmt* stmt = nullptr;

    if(sqlite3_prepare_v2(
            db,
            "SELECT ts, symbol, price FROM ticks ORDER BY symbol, ts;",
            -1, &stmt, nullptr) != SQLITE_OK) {

        std::cerr << "loadTicks: prepare failed: " << sqlite3_errmsg(db) << "\n";
        return out;
    }

    while(sqlite3_step(stmt) == SQLITE_ROW) {

        long long ts      = sqlite3_column_int64(stmt, 0);
        std::string sym   = (const char*)sqlite3_column_text(stmt, 1);
        double price      = sqlite3_column_double(stmt, 2);

        out[sym].emplace_back(ts, price);
    }

    sqlite3_finalize(stmt);
    return out;
}

std::vector<RawSignal> loadSignals(sqlite3* db) {

    std::vector<RawSignal> out;
    sqlite3_stmt* stmt = nullptr;

    // Join signals -> quality_scores by exact (ts, symbol).
    // The engine inserts both within microseconds inside processSymbol,
    // so they share the same epoch-second stamp in practice.
    const char* sql =
        "SELECT s.ts, s.symbol, s.momentum, s.vol_regime, "
        "       q.confidence, q.stale "
        "FROM signals s "
        "JOIN quality_scores q "
        "  ON q.symbol = s.symbol AND q.ts = s.ts "
        "ORDER BY s.symbol, s.ts;";

    if(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "loadSignals: prepare failed: " << sqlite3_errmsg(db) << "\n";
        return out;
    }

    while(sqlite3_step(stmt) == SQLITE_ROW) {

        RawSignal r;
        r.ts         = sqlite3_column_int64(stmt, 0);
        r.symbol     = (const char*)sqlite3_column_text(stmt, 1);
        r.momentum   = (const char*)sqlite3_column_text(stmt, 2);
        r.vol_regime = (const char*)sqlite3_column_text(stmt, 3);
        r.confidence = sqlite3_column_int(stmt, 4);
        r.stale      = sqlite3_column_int(stmt, 5);
        out.push_back(std::move(r));
    }

    sqlite3_finalize(stmt);
    return out;
}

// First tick with ts >= target. Rejected if it's later than target+tolerance:
// an engine gap shouldn't manufacture a fake return by pulling a tick from
// days in the future.
std::optional<double> priceAtOrAfter(
    const TickSeries& series,
    long long target_ts,
    long long tolerance_sec
) {

    auto it = std::lower_bound(
        series.begin(), series.end(),
        Tick{target_ts, 0.0},
        [](const Tick& a, const Tick& b) { return a.first < b.first; }
    );

    if(it == series.end())                       return std::nullopt;
    if(it->first - target_ts > tolerance_sec)    return std::nullopt;
    return it->second;
}

// Last tick with ts <= target. Same tolerance rule on the past side: a baseline
// pulled from an hour before the signal would mis-attribute the return.
std::optional<double> priceAtOrBefore(
    const TickSeries& series,
    long long target_ts,
    long long tolerance_sec
) {

    auto it = std::upper_bound(
        series.begin(), series.end(),
        Tick{target_ts, std::numeric_limits<double>::max()},
        [](const Tick& a, const Tick& b) { return a.first < b.first; }
    );

    if(it == series.begin())                     return std::nullopt;
    --it;
    if(target_ts - it->first > tolerance_sec)    return std::nullopt;
    return it->second;
}

std::string fmt_pct(double v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%5.1f%%", v * 100.0);
    return buf;
}

std::string fmt_ret(double v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%+9.5f", v);
    return buf;
}

void printGroupTable(
    const std::string& title,
    const std::string& key_header,
    const std::map<std::string, evaluation::GroupStats>& groups
) {

    std::cout << "=== " << title << " (GROSS — costs not applied) ===\n";

    std::cout
        << std::left  << std::setw(14) << key_header
        << std::right << std::setw(6)  << "N"
        << "   | "
        << std::setw(7) << "bull%"  << " " << std::setw(7) << "bear%"  << " " << std::setw(10) << "ret@60"
        << "  | "
        << std::setw(7) << "bull%"  << " " << std::setw(7) << "bear%"  << " " << std::setw(10) << "ret@300"
        << "  | "
        << std::setw(7) << "bull%"  << " " << std::setw(7) << "bear%"  << " " << std::setw(10) << "ret@900"
        << "\n";

    std::cout << std::string(108, '-') << "\n";

    if(groups.empty()) {
        std::cout << "  (no observations)\n\n";
        return;
    }

    for(const auto& [key, stats] : groups) {

        auto m = evaluation::finalize(stats);

        std::cout
            << std::left  << std::setw(14) << key
            << std::right << std::setw(6)  << stats.n_total
            << "   | "
            << fmt_pct(m.bull_acc_60)  << " " << fmt_pct(m.bear_acc_60)  << " " << fmt_ret(m.mean_ret_60)
            << "  | "
            << fmt_pct(m.bull_acc_300) << " " << fmt_pct(m.bear_acc_300) << " " << fmt_ret(m.mean_ret_300)
            << "  | "
            << fmt_pct(m.bull_acc_900) << " " << fmt_pct(m.bear_acc_900) << " " << fmt_ret(m.mean_ret_900)
            << "\n";
    }
    std::cout << "\n";
}

// Net-expectancy table: gross mean return, applied cost, and net mean return
// per horizon. This is the bottom-line "is there real edge?" view.
void printNetTable(
    const std::string& title,
    const std::string& key_header,
    const std::map<std::string, evaluation::GroupStats>& groups
) {

    std::cout << "=== " << title << " (NET — round-trip cost applied) ===\n";

    std::cout
        << std::left  << std::setw(14) << key_header
        << std::right << std::setw(6)  << "N_dir"
        << "  | "
        << std::setw(10) << "gross@60"  << " " << std::setw(10) << "cost@60"  << " " << std::setw(10) << "net@60"
        << "  | "
        << std::setw(10) << "gross@300" << " " << std::setw(10) << "cost@300" << " " << std::setw(10) << "net@300"
        << "  | "
        << std::setw(10) << "gross@900" << " " << std::setw(10) << "cost@900" << " " << std::setw(10) << "net@900"
        << "\n";

    std::cout << std::string(132, '-') << "\n";

    if(groups.empty()) {
        std::cout << "  (no observations)\n\n";
        return;
    }

    for(const auto& [key, stats] : groups) {

        auto m = evaluation::finalize(stats);

        // n_dir at horizon 300 is the most representative N; gross/net mean
        // return is computed per-horizon, but we display the 300s N as the
        // headline because it's the most decision-relevant horizon.
        std::cout
            << std::left  << std::setw(14) << key
            << std::right << std::setw(6)  << m.n_dir_300
            << "  | "
            << fmt_ret(m.mean_ret_60)  << " " << fmt_ret(-m.mean_cost_60)  << " " << fmt_ret(m.mean_net_ret_60)
            << "  | "
            << fmt_ret(m.mean_ret_300) << " " << fmt_ret(-m.mean_cost_300) << " " << fmt_ret(m.mean_net_ret_300)
            << "  | "
            << fmt_ret(m.mean_ret_900) << " " << fmt_ret(-m.mean_cost_900) << " " << fmt_ret(m.mean_net_ret_900)
            << "\n";
    }
    std::cout << "\n";
}

}

int main(int argc, char** argv) {

    std::string dbPath = (argc > 1) ? argv[1] : "../data/engine.db";

    sqlite3* db = nullptr;
    if(sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::cerr << "Cannot open " << dbPath << ": "
                  << (db ? sqlite3_errmsg(db) : "unknown") << "\n";
        if(db) sqlite3_close(db);
        return 1;
    }

    auto ticks   = loadTicks(db);
    auto signals = loadSignals(db);
    sqlite3_close(db);

    CostModel costs = defaultCostModel();

    std::vector<evaluation::Observation> observations;
    int n_excluded_stale       = 0;
    int n_excluded_no_baseline = 0;
    int n_excluded_no_future   = 0;

    constexpr long long kBaselineTolSec = 120;

    for(const auto& s : signals) {

        if(s.stale) { n_excluded_stale++; continue; }

        auto sit = ticks.find(s.symbol);
        if(sit == ticks.end()) { n_excluded_no_baseline++; continue; }
        const auto& series = sit->second;

        auto p_now = priceAtOrBefore(series, s.ts, kBaselineTolSec);
        if(!p_now || *p_now <= 0.0) { n_excluded_no_baseline++; continue; }

        evaluation::Observation obs;
        obs.symbol           = s.symbol;
        obs.momentum         = s.momentum;
        obs.vol_regime       = s.vol_regime;
        obs.confidence       = s.confidence;
        obs.round_trip_cost  = costs.costFor(s.symbol);

        // Per-horizon tolerance: one cycle of slack. Beyond that, treat
        // the future tick as missing rather than fabricating a return
        // across an engine downtime gap.
        auto p60  = priceAtOrAfter(series, s.ts + 60,  120);
        auto p300 = priceAtOrAfter(series, s.ts + 300, 360);
        auto p900 = priceAtOrAfter(series, s.ts + 900, 960);

        if(p60)  { obs.has_60  = true; obs.ret_60  = (*p60  - *p_now) / *p_now; }
        if(p300) { obs.has_300 = true; obs.ret_300 = (*p300 - *p_now) / *p_now; }
        if(p900) { obs.has_900 = true; obs.ret_900 = (*p900 - *p_now) / *p_now; }

        if(!obs.has_60 && !obs.has_300 && !obs.has_900) {
            n_excluded_no_future++;
            continue;
        }

        observations.push_back(std::move(obs));
    }

    std::map<std::string, evaluation::GroupStats> by_symbol;
    std::map<std::string, evaluation::GroupStats> by_regime;
    std::map<std::string, evaluation::GroupStats> by_band;

    for(const auto& obs : observations) {
        evaluation::accumulate(by_symbol[obs.symbol],                            obs);
        evaluation::accumulate(by_regime[obs.vol_regime],                        obs);
        evaluation::accumulate(by_band[evaluation::confidenceBand(obs.confidence)], obs);
    }

    std::cout << "Database:  " << dbPath        << "\n";
    std::cout << "Signals:   " << signals.size() << "\n";
    std::cout << "Usable:    " << observations.size() << "\n";
    std::cout << "Excluded:  "
              << n_excluded_stale       << " stale, "
              << n_excluded_no_baseline << " no baseline, "
              << n_excluded_no_future   << " no future\n\n";

    // Surface the applied cost table so the reader can audit it.
    std::cout << "Cost model (round-trip, as fraction of price):\n";
    for(const auto& [sym, c] : costs.round_trip_by_symbol) {
        std::cout << "  " << std::left << std::setw(10) << sym
                  << " " << fmt_ret(c) << "\n";
    }
    std::cout << "  " << std::left << std::setw(10) << "(default)"
              << " " << fmt_ret(costs.default_cost) << "\n\n";

    if(observations.empty()) {
        std::cout << "No usable observations. Let the engine run longer "
                     "and re-run the harness.\n";
        return 0;
    }

    printGroupTable("By symbol",            "symbol", by_symbol);
    printGroupTable("By volatility regime", "regime", by_regime);
    printGroupTable("By confidence band",   "band",   by_band);

    printNetTable("By symbol",            "symbol", by_symbol);
    printNetTable("By volatility regime", "regime", by_regime);
    printNetTable("By confidence band",   "band",   by_band);

    // Decision-relevant headline: per-symbol net expectancy at +300s.
    std::cout << "=== Headline: net expectancy at +300s ===\n";
    bool any_positive = false;
    for(const auto& [sym, stats] : by_symbol) {
        auto m = evaluation::finalize(stats);
        std::cout
            << "  " << std::left  << std::setw(14) << sym
            << "N=" << std::right << std::setw(5)  << m.n_dir_300
            << "  gross=" << fmt_ret(m.mean_ret_300)
            << "  cost="  << fmt_ret(-m.mean_cost_300)
            << "  net="   << fmt_ret(m.mean_net_ret_300)
            << (m.mean_net_ret_300 > 0.0 ? "   [+]" : "   [-]")
            << "\n";
        if(m.n_dir_300 >= 50 && m.mean_net_ret_300 > 0.0) any_positive = true;
    }
    std::cout << "\n";

    if(!any_positive) {
        std::cout
            << "VERDICT: no symbol shows positive net expectancy at +300s with N>=50.\n"
            << "Do NOT advance to MT5 demo execution on these signals. Either:\n"
            << "  (a) collect more data and re-evaluate, OR\n"
            << "  (b) rework the signal logic (vol-scaled momentum threshold,\n"
            << "      empirical vol regime cutoffs) before re-running.\n";
    } else {
        std::cout
            << "VERDICT: at least one symbol shows positive net expectancy at +300s.\n"
            << "Required next step: walk-forward validation. In-sample positive\n"
            << "expectancy is necessary but not sufficient — confirm it survives\n"
            << "out-of-sample before any execution.\n";
    }

    return 0;
}
