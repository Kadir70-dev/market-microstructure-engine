#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace validation {

// ---- Staleness --------------------------------------------------------
// True when the last `lookback` prices are byte-equal (within tolerance).
// Catches: weekend markets, cached API responses, symbol resolving to
// a constant. Returns false until the window has `lookback` samples.
bool isStale(
    const std::vector<double>& prices,
    int lookback = 3,
    double tolerance = 1e-9
);

// ---- Session detection ------------------------------------------------
enum class Session {
    Closed,
    Asia,
    London,
    LondonNY,
    NewYork
};

// UTC-based; DST ignored (sessions defined by UTC clock boundaries).
// Weekend (Sat all day, Sun before 22:00 UTC) returns Closed.
Session detectSession(
    std::chrono::system_clock::time_point now
);

Session detectSession();

const char* sessionName(Session s);

// ---- Quality scoring --------------------------------------------------
struct QualityInputs {
    bool        stale;
    int         sample_count;
    int         window_target;
    std::string momentum;
    std::string vol_regime;
    Session     session;
};

// 0-100. Signal-trustworthiness independent of trading viability.
// Penalises: stale data (→0), neutral momentum, HIGH-vol chop, cold window.
int computeConfidence(const QualityInputs& q);

// 0-100. Confidence weighted by session liquidity. Closed → 0 regardless.
int computeTradeQuality(const QualityInputs& q);

// Letter grade for any 0-100 score.
char qualityGrade(int score);

}
