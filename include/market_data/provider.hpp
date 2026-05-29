#pragma once

#include <string>

namespace market_data {

// Provider-returned quote. Phase 0 carries only `price` for parity with the
// existing engine; Phase 1 (MT5) extends this with bid/ask without changing
// the interface signature.
struct Quote {
    double price = -1.0;   // <= 0 indicates fetch failure
};

class IMarketDataProvider {
public:
    virtual ~IMarketDataProvider() = default;

    // Returns Quote{price<=0} on failure. `symbol` is the provider-native
    // ticker — caller is responsible for any display->api mapping.
    virtual Quote fetchQuote(const std::string& symbol) = 0;
};

}
