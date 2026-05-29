#pragma once

#include <map>
#include <string>

#include "provider.hpp"

namespace market_data {

// Reads quotes from a CSV snapshot written by the MQL5 file-export EA
// (agent/mt5_bridge/mt5_file_export.mq5). One line per symbol:
//
//     symbol,epoch_seconds,bid,ask,mid
//
// The EA OVERWRITES the file every interval, so the file's mtime is the
// freshness signal. This is the only data source — MT5 (under Wine) → EA → CSV
// → this reader → SQLite. No network, no Python, no trading.
//
// Freshness: if the file is missing or its mtime is older than `stale_seconds`,
// every quote is reported as a failure (Quote{price<=0}); the engine then skips
// the symbol, which surfaces as a feed warning and downstream staleness.
class FileProvider : public IMarketDataProvider {
public:
    explicit FileProvider(std::string path, int stale_seconds = 120);

    // Returns the symbol's mid price, or Quote{-1} if the file is
    // missing/stale or the symbol is absent/unparseable.
    Quote fetchQuote(const std::string& symbol) override;

    // True iff the file exists and its mtime is within `stale_seconds`.
    // Re-checks the file each call. Used for startup + health reporting.
    bool fresh();

private:
    void reloadIfChanged();

    std::string                   path_;
    int                           stale_seconds_;
    long long                     last_mtime_ = 0;
    bool                          last_fresh_ = false;
    std::map<std::string, double> mids_;  // symbol -> mid price
};

}
