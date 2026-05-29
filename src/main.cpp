#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "market_data/provider.hpp"
#include "market_data/file_provider.hpp"
#include "storage/sqlite_logger.hpp"
#include "indicators/momentum.hpp"
#include "indicators/volatility.hpp"
#include "validation/validation.hpp"

namespace {

struct SymbolDef {
    const char* display;   // logs / DB row key
    const char* mt5;       // MT5 broker symbol name in the EA's CSV
};

// `mt5` must match the symbol names your broker uses (and that the EA exports).
// Common variants for crude: XTIUSD / WTI / USOIL — adjust to your broker.
constexpr SymbolDef kEURUSD = { "EUR/USD", "EURUSD" };
constexpr SymbolDef kGold   = { "XAU/USD", "XAUUSD" };
constexpr SymbolDef kWTI    = { "USO",     "XTIUSD" };

std::string envOr(const char* key, const std::string& dflt) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : dflt;
}

constexpr int kWindowSize   = 10;
constexpr int kSleepSeconds = 30;

// SIGTERM/SIGINT toggle. volatile sig_atomic_t is the only object
// type the C++ standard guarantees safe to touch from a signal handler.
volatile std::sig_atomic_t g_stop_requested = 0;

void handleStopSignal(int) {
    g_stop_requested = 1;
}

// Configure spdlog with a console sink + rotating file sink at
// ../logs/engine.log (10MB x 7 rotations). Falls back silently to the
// default stdout logger if file logging can't be set up — engine
// should never refuse to start because of a logging problem.
void setupLogger() {

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories("../logs", ec);

    try {
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto file    = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "../logs/engine.log",
            10 * 1024 * 1024,
            7
        );

        std::vector<spdlog::sink_ptr> sinks = { console, file };
        auto logger = std::make_shared<spdlog::logger>(
            "engine",
            sinks.begin(),
            sinks.end()
        );

        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::warn);

        spdlog::set_default_logger(logger);
    }
    catch(const std::exception& e) {
        spdlog::warn(
            "setupLogger: file logger unavailable, using stdout only ({})",
            e.what()
        );
    }
}

// Break the 30s sleep into 1s chunks so SIGTERM cuts shutdown latency
// from up-to-30s down to ~1s.
void interruptibleSleep(int seconds) {

    for(int i = 0; i < seconds && !g_stop_requested; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

}

void updatePriceHistory(
    std::vector<double>& prices,
    double newPrice
) {

    prices.push_back(newPrice);

    if((int)prices.size() > kWindowSize) {
        prices.erase(prices.begin());
    }
}

bool processSymbol(
    const SymbolDef& sym,
    std::vector<double>& prices,
    market_data::IMarketDataProvider& provider,
    SqliteLogger& logger
) {

    double price = provider.fetchQuote(sym.mt5).price;

    if(price <= 0) {
        spdlog::warn(
            "Skipping {}: invalid price",
            sym.display
        );
        return false;
    }

    // Capture ts at the moment of observation. Shared across all three
    // persisted rows so the harness JOIN (s.ts = q.ts) never silently
    // drops a signal whose writes crossed a wall-clock second boundary.
    long long ts = SqliteLogger::nowSeconds();

    updatePriceHistory(prices, price);

    std::string momentum = detectMomentum(prices);
    double      volScore = calculateVolatility(prices);
    std::string regime   = classifyVolatility(volScore);

    bool                stale   = validation::isStale(prices);
    validation::Session session = validation::detectSession();
    const char*         sessStr = validation::sessionName(session);

    validation::QualityInputs q{
        stale,
        (int)prices.size(),
        kWindowSize,
        momentum,
        regime,
        session
    };

    int  confidence   = validation::computeConfidence(q);
    int  tradeQuality = validation::computeTradeQuality(q);
    char grade        = validation::qualityGrade(tradeQuality);

    spdlog::info(
        "{}: {} | mom={} | vol={} ({:.5f}) | sess={} | stale={} | conf={} | TQ={} [{}]",
        sym.display,
        price,
        momentum,
        regime,
        volScore,
        sessStr,
        stale ? "Y" : "N",
        confidence,
        tradeQuality,
        grade
    );

    // Atomic per-symbol cycle: either all three rows commit together or
    // none do. Combined with the shared ts above, this closes both the
    // orphan-row case and the silent JOIN-loss case.
    logger.beginCycle();
    logger.recordTick(   ts, sym.display, price);
    logger.recordSignal( ts, sym.display, momentum, volScore, regime);
    logger.recordQuality(ts, sym.display, stale, sessStr, confidence, tradeQuality, grade);
    logger.endCycle();

    return true;
}

int main() {

    // Install handlers BEFORE anything else so even early-startup crashes
    // get the chance to shut down cleanly.
    std::signal(SIGTERM, handleStopSignal);
    std::signal(SIGINT,  handleStopSignal);

    setupLogger();

    try {

        spdlog::info(
            "Market Intelligence Engine Started"
        );

        // Sole data source: the MQL5 file-export EA's CSV snapshot
        // (MT5 under Wine → EA → mme_quotes.csv → here). Read-only, no trading.
        std::string csvPath   = envOr("MME_QUOTES_CSV", "../data/mme_quotes.csv");
        int         staleSecs = std::stoi(envOr("MME_FILE_STALE_S", "120"));
        spdlog::info("Provider: MT5 file-export (csv={}, stale>{}s) — read-only, no trading",
                     csvPath, staleSecs);

        auto fileProv = std::make_unique<market_data::FileProvider>(csvPath, staleSecs);
        if(!fileProv->fresh()) {
            spdlog::warn("Quotes CSV missing or stale at startup ({}). Is the MT5 EA "
                         "running and writing it? Engine will retry every cycle.", csvPath);
        }
        std::unique_ptr<market_data::IMarketDataProvider> provider = std::move(fileProv);

        // DB path is overridable (e.g. a separate DB for a verification run).
        std::string dbPath = envOr("MME_DB_PATH", "../data/engine.db");
        SqliteLogger logger(dbPath);

        std::vector<double>
            eurusdPrices,
            goldPrices,
            crudePrices;

        // Warm-up fetch.
        processSymbol(kEURUSD, eurusdPrices, *provider, logger);
        processSymbol(kGold,   goldPrices,   *provider, logger);
        processSymbol(kWTI,    crudePrices,  *provider, logger);

        while(!g_stop_requested) {

            interruptibleSleep(kSleepSeconds);
            if(g_stop_requested) break;

            spdlog::info(
                "================================="
            );

            processSymbol(kEURUSD, eurusdPrices, *provider, logger);
            processSymbol(kGold,   goldPrices,   *provider, logger);
            processSymbol(kWTI,    crudePrices,  *provider, logger);
        }

        spdlog::info(
            "Engine shutting down cleanly"
        );
    }

    catch(const std::exception& e) {

        spdlog::error(
            "Error: {}",
            e.what()
        );
    }

    // RAII: SqliteLogger destructor finalizes prepared statements and
    // closes the DB. Reachable because we exit via flag, not while(true).
    return 0;
}
