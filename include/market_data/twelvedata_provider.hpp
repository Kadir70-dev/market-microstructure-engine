#pragma once

#include <string>

#include "provider.hpp"

namespace market_data {

class TwelveDataProvider : public IMarketDataProvider {
public:
    explicit TwelveDataProvider(std::string apiKey);

    Quote fetchQuote(const std::string& symbol) override;

private:
    std::string apiKey_;
};

}
