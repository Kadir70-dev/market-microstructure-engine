#include "storage/sqlite_logger.hpp"

#include <chrono>
#include <filesystem>

#include <sqlite3.h>
#include <spdlog/spdlog.h>

namespace {

constexpr const char* kSchema =
    "CREATE TABLE IF NOT EXISTS ticks ("
    "  id     INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts     INTEGER NOT NULL,"
    "  symbol TEXT    NOT NULL,"
    "  price  REAL    NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS signals ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts         INTEGER NOT NULL,"
    "  symbol     TEXT    NOT NULL,"
    "  momentum   TEXT    NOT NULL,"
    "  vol_score  REAL    NOT NULL,"
    "  vol_regime TEXT    NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS quality_scores ("
    "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts            INTEGER NOT NULL,"
    "  symbol        TEXT    NOT NULL,"
    "  stale         INTEGER NOT NULL,"
    "  session       TEXT    NOT NULL,"
    "  confidence    INTEGER NOT NULL,"
    "  trade_quality INTEGER NOT NULL,"
    "  grade         TEXT    NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_ticks_symbol_ts   ON ticks(symbol, ts);"
    "CREATE INDEX IF NOT EXISTS idx_signals_symbol_ts ON signals(symbol, ts);"
    "CREATE INDEX IF NOT EXISTS idx_quality_symbol_ts ON quality_scores(symbol, ts);";

constexpr const char* kInsertTick =
    "INSERT INTO ticks(ts, symbol, price) "
    "VALUES(?, ?, ?);";

constexpr const char* kInsertSignal =
    "INSERT INTO signals(ts, symbol, momentum, vol_score, vol_regime) "
    "VALUES(?, ?, ?, ?, ?);";

constexpr const char* kInsertQuality =
    "INSERT INTO quality_scores"
    "(ts, symbol, stale, session, confidence, trade_quality, grade) "
    "VALUES(?, ?, ?, ?, ?, ?, ?);";

}

long long SqliteLogger::nowSeconds() {

    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

SqliteLogger::SqliteLogger(
    const std::string& dbPath
) {

    std::filesystem::path p(dbPath);

    if(p.has_parent_path()) {

        std::error_code ec;

        std::filesystem::create_directories(
            p.parent_path(),
            ec
        );

        if(ec) {

            spdlog::warn(
                "SqliteLogger: could not create directory {}: {}",
                p.parent_path().string(),
                ec.message()
            );
        }
    }

    if(sqlite3_open(dbPath.c_str(), &db_) != SQLITE_OK) {

        spdlog::warn(
            "SqliteLogger: could not open {}: {}",
            dbPath,
            db_ ? sqlite3_errmsg(db_) : "unknown"
        );

        if(db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }

        return;
    }

    // WAL: lets engine_backtest read while engine is writing, and gives
    // cleaner crash recovery. Ignored failure: rollback-journal mode is
    // still functional, just lock-contended.
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

    char* errMsg = nullptr;

    if(sqlite3_exec(db_, kSchema, nullptr, nullptr, &errMsg) != SQLITE_OK) {

        spdlog::warn(
            "SqliteLogger: schema init failed: {}",
            errMsg ? errMsg : "unknown"
        );

        sqlite3_free(errMsg);
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }

    if(sqlite3_prepare_v2(db_, kInsertTick,    -1, &tickStmt_,    nullptr) != SQLITE_OK
        || sqlite3_prepare_v2(db_, kInsertSignal,  -1, &signalStmt_,  nullptr) != SQLITE_OK
        || sqlite3_prepare_v2(db_, kInsertQuality, -1, &qualityStmt_, nullptr) != SQLITE_OK) {

        spdlog::warn(
            "SqliteLogger: prepare failed: {}",
            sqlite3_errmsg(db_)
        );

        sqlite3_finalize(tickStmt_);
        sqlite3_finalize(signalStmt_);
        sqlite3_finalize(qualityStmt_);
        sqlite3_close(db_);
        db_ = nullptr;
        tickStmt_ = nullptr;
        signalStmt_ = nullptr;
        qualityStmt_ = nullptr;
        return;
    }

    spdlog::info(
        "SqliteLogger: opened {}",
        dbPath
    );
}

SqliteLogger::~SqliteLogger() {

    if(tickStmt_)    sqlite3_finalize(tickStmt_);
    if(signalStmt_)  sqlite3_finalize(signalStmt_);
    if(qualityStmt_) sqlite3_finalize(qualityStmt_);
    if(db_)          sqlite3_close(db_);
}

void SqliteLogger::beginCycle() {

    if(!db_) return;

    char* err = nullptr;

    if(sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &err) != SQLITE_OK) {

        spdlog::warn(
            "SqliteLogger: beginCycle failed: {}",
            err ? err : "?"
        );

        sqlite3_free(err);
    }
}

void SqliteLogger::endCycle() {

    if(!db_) return;

    char* err = nullptr;

    if(sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &err) != SQLITE_OK) {

        spdlog::warn(
            "SqliteLogger: endCycle commit failed: {}",
            err ? err : "?"
        );

        sqlite3_free(err);

        // Best-effort rollback. If no transaction is active (BEGIN earlier
        // failed) ROLLBACK is a no-op and its error is ignored.
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
}

void SqliteLogger::recordTick(
    long long ts,
    const std::string& symbol,
    double price
) {

    if(!tickStmt_) return;

    sqlite3_reset(tickStmt_);
    sqlite3_bind_int64 (tickStmt_, 1, ts);
    sqlite3_bind_text  (tickStmt_, 2, symbol.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(tickStmt_, 3, price);

    if(sqlite3_step(tickStmt_) != SQLITE_DONE) {

        spdlog::warn(
            "SqliteLogger: tick insert failed for {}: {}",
            symbol,
            sqlite3_errmsg(db_)
        );
    }
}

void SqliteLogger::recordSignal(
    long long ts,
    const std::string& symbol,
    const std::string& momentum,
    double volScore,
    const std::string& volRegime
) {

    if(!signalStmt_) return;

    sqlite3_reset(signalStmt_);
    sqlite3_bind_int64 (signalStmt_, 1, ts);
    sqlite3_bind_text  (signalStmt_, 2, symbol.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (signalStmt_, 3, momentum.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(signalStmt_, 4, volScore);
    sqlite3_bind_text  (signalStmt_, 5, volRegime.c_str(), -1, SQLITE_TRANSIENT);

    if(sqlite3_step(signalStmt_) != SQLITE_DONE) {

        spdlog::warn(
            "SqliteLogger: signal insert failed for {}: {}",
            symbol,
            sqlite3_errmsg(db_)
        );
    }
}

void SqliteLogger::recordQuality(
    long long ts,
    const std::string& symbol,
    bool stale,
    const std::string& session,
    int confidence,
    int tradeQuality,
    char grade
) {

    if(!qualityStmt_) return;

    char gradeStr[2] = { grade, '\0' };

    sqlite3_reset(qualityStmt_);
    sqlite3_bind_int64 (qualityStmt_, 1, ts);
    sqlite3_bind_text  (qualityStmt_, 2, symbol.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_int   (qualityStmt_, 3, stale ? 1 : 0);
    sqlite3_bind_text  (qualityStmt_, 4, session.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int   (qualityStmt_, 5, confidence);
    sqlite3_bind_int   (qualityStmt_, 6, tradeQuality);
    sqlite3_bind_text  (qualityStmt_, 7, gradeStr,        -1, SQLITE_TRANSIENT);

    if(sqlite3_step(qualityStmt_) != SQLITE_DONE) {

        spdlog::warn(
            "SqliteLogger: quality insert failed for {}: {}",
            symbol,
            sqlite3_errmsg(db_)
        );
    }
}
