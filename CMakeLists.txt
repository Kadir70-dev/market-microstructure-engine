cmake_minimum_required(VERSION 3.16)

project(MarketMicrostructureEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(spdlog REQUIRED)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json REQUIRED)

add_executable(engine
    src/main.cpp
    src/market_data/market_fetcher.cpp
    src/market_data/twelvedata_provider.cpp
    src/market_data/mt5_provider.cpp
    src/storage/sqlite_logger.cpp
    src/indicators/momentum.cpp
    src/indicators/volatility.cpp
    src/validation/validation.cpp
)

target_include_directories(
    engine PRIVATE include
)

target_link_libraries(
    engine PRIVATE
    spdlog::spdlog
    CURL::libcurl
    SQLite::SQLite3
    nlohmann_json::nlohmann_json
)

# Evaluation harness: separate read-only binary.
# Links only sqlite3 — no curl, no spdlog, no live engine code.
add_executable(engine_backtest
    src/evaluation/main.cpp
    src/evaluation/metrics.cpp
)

target_include_directories(
    engine_backtest PRIVATE include
)

target_link_libraries(
    engine_backtest PRIVATE
    SQLite::SQLite3
)