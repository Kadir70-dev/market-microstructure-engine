// Zero-dependency unit tests for the pure logic in the engine.
//
// Deliberately NO external test framework (no Catch2/doctest/gtest) — those
// are real system dependencies that complicate the build and CI. A ~30-line
// macro harness is enough to test pure functions, keeps `cmake --build` self
// contained, and is registered with CTest via add_test() in CMakeLists.txt.
//
// Covers:
//   - validation.cpp : isStale, detectSession, computeConfidence,
//                       computeTradeQuality, qualityGrade
//   - metrics.cpp    : accumulate, finalize (gross + net), confidenceBand

#include <chrono>
#include <cmath>
#include <ctime>
#include <iostream>
#include <vector>

#include "validation/validation.hpp"
#include "evaluation/metrics.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

bool approx(double a, double b, double eps = 1e-9) {
    return std::abs(a - b) < eps;
}

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if(!(cond)) {                                                        \
            ++g_failures;                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__              \
                      << "  " #cond "\n";                                    \
        }                                                                    \
    } while(0)

#define CHECK_APPROX(a, b)                                                   \
    do {                                                                     \
        ++g_checks;                                                          \
        if(!approx((a), (b))) {                                              \
            ++g_failures;                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__              \
                      << "  " #a " ~= " #b "  (" << (a) << " vs " << (b)     \
                      << ")\n";                                              \
        }                                                                    \
    } while(0)

// Build a UTC time_point from explicit calendar fields (timegm = UTC mktime).
std::chrono::system_clock::time_point utc_tp(
    int y, int mon, int d, int h, int mi = 0, int s = 0) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mon - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = s;
    std::time_t t = timegm(&tm);
    return std::chrono::system_clock::from_time_t(t);
}

// ---- validation: isStale ------------------------------------------------
void test_isStale() {
    using validation::isStale;

    CHECK(isStale({}) == false);                 // empty < lookback
    CHECK(isStale({1.0, 1.0}) == false);         // 2 samples < lookback 3
    CHECK(isStale({1.0, 1.0, 1.0}) == true);     // 3 identical → frozen
    CHECK(isStale({1.0, 1.0, 2.0}) == false);    // last differs
    CHECK(isStale({1.0, 2.0, 2.0}) == false);    // earlier-in-window differs
    CHECK(isStale({5.0, 5.0, 5.0, 5.0}) == true);

    // tolerance: tiny wiggle below tolerance is still "frozen"
    CHECK(isStale({1.0, 1.0 + 1e-12, 1.0}, 3, 1e-9) == true);
    CHECK(isStale({1.0, 1.0 + 1e-6, 1.0}, 3, 1e-9) == false);
}

// ---- validation: detectSession (UTC, no DST) ----------------------------
void test_detectSession() {
    using validation::Session;
    using validation::detectSession;

    // 2026-01-07 is a Wednesday.
    CHECK(detectSession(utc_tp(2026, 1, 7, 3))  == Session::Asia);     // 00-08
    CHECK(detectSession(utc_tp(2026, 1, 7, 10)) == Session::London);   // 08-13
    CHECK(detectSession(utc_tp(2026, 1, 7, 14)) == Session::LondonNY); // 13-17
    CHECK(detectSession(utc_tp(2026, 1, 7, 18)) == Session::NewYork);  // 17-22
    CHECK(detectSession(utc_tp(2026, 1, 7, 23)) == Session::Asia);     // >=22 wraps to Asia

    // Weekend boundaries: Fri 2026-01-09, Sat 10, Sun 11.
    CHECK(detectSession(utc_tp(2026, 1, 9, 23)) == Session::Closed);   // Fri >=22
    CHECK(detectSession(utc_tp(2026, 1, 9, 21)) == Session::NewYork);  // Fri before 22 still open
    CHECK(detectSession(utc_tp(2026, 1, 10, 12)) == Session::Closed);  // Saturday
    CHECK(detectSession(utc_tp(2026, 1, 11, 12)) == Session::Closed);  // Sun before 22
    CHECK(detectSession(utc_tp(2026, 1, 11, 23)) == Session::Asia);    // Sun 22:00 reopen
}

// ---- validation: confidence / trade quality / grade ---------------------
validation::QualityInputs qi(bool stale, int count, const std::string& mom,
                             const std::string& regime, validation::Session s) {
    return validation::QualityInputs{stale, count, 10, mom, regime, s};
}

void test_confidence() {
    using namespace validation;

    // Stale → hard zero regardless of everything else.
    CHECK(computeConfidence(qi(true, 10, "Bullish", "LOW", Session::London)) == 0);

    // Warm window, LOW vol, directional → full 100.
    CHECK(computeConfidence(qi(false, 10, "Bullish", "LOW", Session::London)) == 100);

    // Vol-regime penalties.
    CHECK(computeConfidence(qi(false, 10, "Bullish", "MEDIUM", Session::London)) == 80);
    CHECK(computeConfidence(qi(false, 10, "Bullish", "HIGH", Session::London)) == 50);

    // Neutral momentum penalty (×0.3).
    CHECK(computeConfidence(qi(false, 10, "Neutral", "LOW", Session::London)) == 30);

    // Cold window scales linearly (5/10 → 50).
    CHECK(computeConfidence(qi(false, 5, "Bullish", "LOW", Session::London)) == 50);
}

void test_tradeQuality() {
    using namespace validation;

    // Closed session → 0 regardless of confidence.
    CHECK(computeTradeQuality(qi(false, 10, "Bullish", "LOW", Session::Closed)) == 0);

    // London/LondonNY multiplier 1.0 → equals confidence.
    CHECK(computeTradeQuality(qi(false, 10, "Bullish", "LOW", Session::London)) == 100);
    CHECK(computeTradeQuality(qi(false, 10, "Bullish", "LOW", Session::LondonNY)) == 100);

    // NewYork 0.95, Asia 0.6.
    CHECK(computeTradeQuality(qi(false, 10, "Bullish", "LOW", Session::NewYork)) == 95);
    CHECK(computeTradeQuality(qi(false, 10, "Bullish", "LOW", Session::Asia)) == 60);
}

void test_qualityGrade() {
    using validation::qualityGrade;
    CHECK(qualityGrade(100) == 'A');
    CHECK(qualityGrade(75)  == 'A');
    CHECK(qualityGrade(74)  == 'B');
    CHECK(qualityGrade(50)  == 'B');
    CHECK(qualityGrade(49)  == 'C');
    CHECK(qualityGrade(25)  == 'C');
    CHECK(qualityGrade(24)  == 'D');
    CHECK(qualityGrade(0)   == 'D');
}

// ---- metrics: accumulate / finalize -------------------------------------
evaluation::Observation obs(const std::string& mom, double ret60, double cost) {
    evaluation::Observation o;
    o.momentum         = mom;
    o.has_60           = true;
    o.ret_60           = ret60;
    o.round_trip_cost  = cost;
    return o;
}

void test_metrics_bullish_beats_cost() {
    evaluation::GroupStats g;
    evaluation::accumulate(g, obs("Bullish", 0.001, 0.0002));  // up, beats cost

    auto m = evaluation::finalize(g);
    CHECK(g.n_total == 1);
    CHECK(m.n_dir_60 == 1);
    CHECK_APPROX(m.bull_acc_60, 1.0);          // moved up
    CHECK_APPROX(m.bull_acc_net_60, 1.0);      // and beat the spread
    CHECK_APPROX(m.mean_ret_60, 0.001);
    CHECK_APPROX(m.mean_cost_60, 0.0002);
    CHECK_APPROX(m.mean_net_ret_60, 0.0008);
}

void test_metrics_bullish_below_cost() {
    evaluation::GroupStats g;
    evaluation::accumulate(g, obs("Bullish", 0.0001, 0.0002));  // up, but < cost

    auto m = evaluation::finalize(g);
    CHECK_APPROX(m.bull_acc_60, 1.0);          // gross: still moved up
    CHECK_APPROX(m.bull_acc_net_60, 0.0);      // net: did NOT beat the spread
    CHECK_APPROX(m.mean_net_ret_60, -0.0001);  // negative after costs
}

void test_metrics_bearish() {
    evaluation::GroupStats g;
    evaluation::accumulate(g, obs("Bearish", -0.001, 0.0002));  // down, correct for bear

    auto m = evaluation::finalize(g);
    CHECK(m.n_dir_60 == 1);
    CHECK_APPROX(m.bear_acc_60, 1.0);
    CHECK_APPROX(m.bear_acc_net_60, 1.0);
    CHECK_APPROX(m.mean_ret_60, 0.001);        // direction-adjusted → positive
}

void test_metrics_neutral_is_noncontributing() {
    evaluation::GroupStats g;
    evaluation::accumulate(g, obs("Neutral", 0.005, 0.0002));

    auto m = evaluation::finalize(g);
    CHECK(g.n_total == 1);                      // counted in total...
    CHECK(m.n_dir_60 == 0);                     // ...but contributes to no direction
    CHECK_APPROX(m.mean_ret_60, 0.0);
}

void test_metrics_empty_no_divzero() {
    evaluation::GroupStats g;
    auto m = evaluation::finalize(g);           // must not divide by zero
    CHECK(m.n_dir_60 == 0);
    CHECK_APPROX(m.bull_acc_60, 0.0);
    CHECK_APPROX(m.mean_ret_60, 0.0);
}

void test_metrics_aggregate() {
    evaluation::GroupStats g;
    evaluation::accumulate(g, obs("Bullish", 0.002, 0.0));   // hit
    evaluation::accumulate(g, obs("Bullish", -0.001, 0.0));  // miss
    auto m = evaluation::finalize(g);
    CHECK(m.n_dir_60 == 2);
    CHECK_APPROX(m.bull_acc_60, 0.5);                        // 1 of 2 up
    CHECK_APPROX(m.mean_ret_60, (0.002 + (-0.001)) / 2.0);
}

void test_confidenceBand() {
    using evaluation::confidenceBand;
    CHECK(std::string(confidenceBand(0))   == "0-29");
    CHECK(std::string(confidenceBand(29))  == "0-29");
    CHECK(std::string(confidenceBand(30))  == "30-59");
    CHECK(std::string(confidenceBand(59))  == "30-59");
    CHECK(std::string(confidenceBand(60))  == "60-100");
    CHECK(std::string(confidenceBand(100)) == "60-100");
}

}  // namespace

int main() {
    test_isStale();
    test_detectSession();
    test_confidence();
    test_tradeQuality();
    test_qualityGrade();

    test_metrics_bullish_beats_cost();
    test_metrics_bullish_below_cost();
    test_metrics_bearish();
    test_metrics_neutral_is_noncontributing();
    test_metrics_empty_no_divzero();
    test_metrics_aggregate();
    test_confidenceBand();

    std::cout << (g_checks - g_failures) << "/" << g_checks
              << " checks passed.\n";
    if(g_failures) {
        std::cerr << g_failures << " CHECK(s) FAILED.\n";
        return 1;
    }
    std::cout << "All unit tests passed.\n";
    return 0;
}
