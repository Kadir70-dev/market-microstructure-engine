#include "market_data/twelvedata_provider.hpp"
#include "market_data/market_fetcher.hpp"

#include <utility>

namespace market_data {

TwelveDataProvider::TwelveDataProvider(std::string apiKey)
    : apiKey_(std::move(apiKey)) {}

Quote TwelveDataProvider::fetchQuote(const std::string& symbol) {
    return Quote{ ::fetchMarketPrice(symbol, apiKey_) };
}

}
