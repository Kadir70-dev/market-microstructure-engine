#include "validation/validation.hpp"

#include <cmath>
#include <ctime>

namespace validation {

bool isStale(
    const std::vector<double>& prices,
    int lookback,
    double tolerance
) {

    if((int)prices.size() < lookback)
        return false;

    double last = prices.back();

    for(int i = 1; i < lookback; ++i) {

        double p = prices[prices.size() - 1 - i];

        if(std::abs(p - last) > tolerance)
            return false;
    }

    return true;
}

Session detectSession(
    std::chrono::system_clock::time_point now
) {

    std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm utc{};
    gmtime_r(&t, &utc);

    int wday = utc.tm_wday;
    int hour = utc.tm_hour;

    // Forex weekend: Fri 22:00 UTC -> Sun 22:00 UTC
    if(wday == 6)                 return Session::Closed;
    if(wday == 5 && hour >= 22)   return Session::Closed;
    if(wday == 0 && hour < 22)    return Session::Closed;

    // Active sessions (UTC):
    //   Asia      00-08 (and 22-24)
    //   London    08-13
    //   LondonNY  13-17  (overlap = peak liquidity)
    //   NewYork   17-22
    if(hour >= 22) return Session::Asia;
    if(hour < 8)   return Session::Asia;
    if(hour < 13)  return Session::London;
    if(hour < 17)  return Session::LondonNY;
    return Session::NewYork;
}

Session detectSession() {
    return detectSession(
        std::chrono::system_clock::now()
    );
}

const char* sessionName(Session s) {

    switch(s) {
        case Session::Closed:   return "Closed";
        case Session::Asia:     return "Asia";
        case Session::London:   return "London";
        case Session::LondonNY: return "London+NY";
        case Session::NewYork:  return "NewYork";
    }

    return "Closed";
}

int computeConfidence(const QualityInputs& q) {

    if(q.stale)
        return 0;

    double base = 100.0;

    if(q.sample_count < q.window_target && q.window_target > 0) {
        base *= (double)q.sample_count / (double)q.window_target;
    }

    if(q.vol_regime == "HIGH")        base *= 0.5;
    else if(q.vol_regime == "MEDIUM") base *= 0.8;

    if(q.momentum == "Neutral")       base *= 0.3;

    int score = (int)std::round(base);

    if(score < 0)   score = 0;
    if(score > 100) score = 100;

    return score;
}

int computeTradeQuality(const QualityInputs& q) {

    int confidence = computeConfidence(q);

    double mult = 0.0;

    switch(q.session) {
        case Session::Closed:   mult = 0.0;  break;
        case Session::Asia:     mult = 0.6;  break;
        case Session::London:   mult = 1.0;  break;
        case Session::LondonNY: mult = 1.0;  break;
        case Session::NewYork:  mult = 0.95; break;
    }

    int q_score = (int)std::round(confidence * mult);

    if(q_score < 0)   q_score = 0;
    if(q_score > 100) q_score = 100;

    return q_score;
}

char qualityGrade(int score) {

    if(score >= 75) return 'A';
    if(score >= 50) return 'B';
    if(score >= 25) return 'C';
    return 'D';
}

}
