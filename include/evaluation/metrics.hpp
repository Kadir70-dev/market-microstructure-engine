#pragma once

#include <string>

namespace evaluation {

// A single scored signal paired with its forward returns.
// Filled by main.cpp from the SQLite join; consumed by accumulate().
struct Observation {

    std::string symbol;
    std::string momentum;     // "Bullish" / "Bearish" / "Neutral"
    std::string vol_regime;   // "LOW" / "MEDIUM" / "HIGH"
    int         confidence;   // 0-100

    bool   has_60  = false;
    bool   has_300 = false;
    bool   has_900 = false;

    double ret_60  = 0.0;
    double ret_300 = 0.0;
    double ret_900 = 0.0;

    // Round-trip cost as a fraction of price (e.g. 1.5e-4 == 1.5 pips on EUR/USD).
    // Stamped per observation by main.cpp from the cost table — keeps the
    // metrics module pure-arithmetic and lets the cost differ per symbol
    // without leaking a symbol→cost map into derived groupings.
    double round_trip_cost = 0.0;
};

// Per-horizon counters. Separate bull/bear so accuracy is per-direction.
struct HorizonStats {

    int    n_bull             = 0;
    int    n_bear             = 0;
    int    bull_hits          = 0;   // gross: raw return moved in signal direction
    int    bear_hits          = 0;
    int    bull_hits_net      = 0;   // net:   raw move beat the round-trip cost
    int    bear_hits_net      = 0;
    double sum_dir_return     = 0.0; // sign-adjusted gross
    double sum_dir_cost       = 0.0; // running sum of round-trip costs (positive)
};

// Group rollup (per symbol / regime / band).
struct GroupStats {

    int          n_total = 0;
    HorizonStats h60;
    HorizonStats h300;
    HorizonStats h900;
};

// Derived ratios + means, computed once at print time.
struct GroupMetrics {

    // Gross — no cost applied.
    double bull_acc_60      = 0, bull_acc_300      = 0, bull_acc_900      = 0;
    double bear_acc_60      = 0, bear_acc_300      = 0, bear_acc_900      = 0;
    double mean_ret_60      = 0, mean_ret_300      = 0, mean_ret_900      = 0;

    // Net — round-trip cost subtracted from each directional return.
    // A "net hit" means the move was large enough to overcome the spread.
    // mean_net_ret = mean_ret - mean_cost  (cost is positive).
    double bull_acc_net_60  = 0, bull_acc_net_300  = 0, bull_acc_net_900  = 0;
    double bear_acc_net_60  = 0, bear_acc_net_300  = 0, bear_acc_net_900  = 0;
    double mean_net_ret_60  = 0, mean_net_ret_300  = 0, mean_net_ret_900  = 0;
    double mean_cost_60     = 0, mean_cost_300     = 0, mean_cost_900     = 0;

    int    n_dir_60         = 0, n_dir_300         = 0, n_dir_900         = 0;
};

void accumulate(GroupStats& g, const Observation& obs);
GroupMetrics finalize(const GroupStats& g);

// Confidence band label. Cut points are 30 and 60.
const char* confidenceBand(int conf);

}
