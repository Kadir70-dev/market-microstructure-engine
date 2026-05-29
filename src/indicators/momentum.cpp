#include "indicators/momentum.hpp"

std::string detectMomentum(
    const std::vector<double>& prices
) {

    if(prices.size() < 2)
        return "Neutral";

    double latest =
        prices.back();

    double previous =
        prices[prices.size() - 2];

    double diff =
        latest - previous;

    if(diff > 0.0000001)
        return "Bullish";

    if(diff < -0.0000001)
        return "Bearish";

    return "Neutral";
}
