#pragma once

#include <string>

struct sqlite3;
struct sqlite3_stmt;

class SqliteLogger {
public:

    explicit SqliteLogger(
        const std::string& dbPath
    );

    ~SqliteLogger();

    SqliteLogger(const SqliteLogger&) = delete;
    SqliteLogger& operator=(const SqliteLogger&) = delete;

    bool isOpen() const {
        return db_ != nullptr;
    }

    // Seconds since the Unix epoch. Sample once per cycle and pass it
    // to all three record* calls so tick/signal/quality rows share an
    // exact ts (the harness JOIN depends on this).
    static long long nowSeconds();

    // Wrap the three record* calls in BEGIN IMMEDIATE / COMMIT so a
    // cycle commits atomically. Best-effort: failure of BEGIN/COMMIT
    // degrades to per-statement autocommit, preserving the prior behavior.
    void beginCycle();
    void endCycle();

    void recordTick(
        long long ts,
        const std::string& symbol,
        double price
    );

    void recordSignal(
        long long ts,
        const std::string& symbol,
        const std::string& momentum,
        double volScore,
        const std::string& volRegime
    );

    void recordQuality(
        long long ts,
        const std::string& symbol,
        bool stale,
        const std::string& session,
        int confidence,
        int tradeQuality,
        char grade
    );

private:

    sqlite3* db_ = nullptr;
    sqlite3_stmt* tickStmt_ = nullptr;
    sqlite3_stmt* signalStmt_ = nullptr;
    sqlite3_stmt* qualityStmt_ = nullptr;
};
