cmake_minimum_required(VERSION 3.16)

project(MarketMicrostructureEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(spdlog REQUIRED)
find_package(SQLite3 REQUIRED)

# MT5-only via file-export: quotes come from a local CSV written by the MQL5 EA
# (read by FileProvider). No HTTP client and no JSON parser are needed anymore —
# CURL and nlohmann_json were TwelveData / Python-bridge dependencies.
add_executable(engine
    src/main.cpp
    src/market_data/file_provider.cpp
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
    SQLite::SQLite3
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

# ── Unit tests ────────────────────────────────────────────────────────────
# Zero external test framework. Tests the pure logic (validation + metrics) by
# compiling those sources directly into a small test binary, registered with
# CTest. Run with `ctest --test-dir build --output-on-failure` (or `ctest -V`).
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
    enable_testing()

    add_executable(unit_tests
        tests/unit_tests.cpp
        src/validation/validation.cpp
        src/evaluation/metrics.cpp
    )
    target_include_directories(unit_tests PRIVATE include)

    add_test(NAME unit_tests COMMAND unit_tests)

    # Hermes (Python) tests — run via the stdlib unittest runner from repo root.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME hermes_tests
            COMMAND ${Python3_EXECUTABLE} -m unittest
                    agent.hermes.tests.test_daily_report -v
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )
    endif()
endif()