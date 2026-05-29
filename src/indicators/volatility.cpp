#include "indicators/volatility.hpp"

#include <cmath>

double calculateVolatility(
    const std::vector<double>& prices
) {

    if(prices.size() < 2)
        return 0.0;

    std::vector<double> returns;
    returns.reserve(prices.size() - 1);

    for(size_t i = 1; i < prices.size(); ++i) {

        if(prices[i-1] <= 0.0 || prices[i] <= 0.0)
            continue;

        returns.push_back(
            std::log(prices[i] / prices[i-1])
        );
    }

    if(returns.size() < 2)
        return 0.0;

    double mean = 0.0;

    for(double r : returns)
        mean += r;

    mean /= returns.size();

    double variance = 0.0;

    for(double r : returns)
        variance += (r - mean) * (r - mean);

    variance /= returns.size();

    return std::sqrt(variance);
}

std::string classifyVolatility(
    double score
) {

    if(score > 0.001)  return "HIGH";
    if(score > 0.0003) return "MEDIUM";
    return "LOW";
}
