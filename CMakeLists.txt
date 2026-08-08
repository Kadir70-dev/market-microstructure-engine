cmake_minimum_required(VERSION 3.16)

project(MarketMicrostructureEngine VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Architecture v1.0 requires reproducible floating-point semantics. Live
# trading remains disabled: this build topology adds no live-order target or
# compile definition.
if(MSVC)
    if(MSVC_VERSION LESS 1930)
        message(FATAL_ERROR "Pinned Windows toolchain requires MSVC 19.30 or newer")
    endif()
    add_compile_options(/fp:strict /permissive-)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11.0)
        message(FATAL_ERROR "Pinned Linux toolchain requires GCC 11 or newer")
    endif()
    add_compile_options(-ffp-contract=off -fno-fast-math)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14.0)
        message(FATAL_ERROR "Pinned Clang toolchain requires Clang 14 or newer")
    endif()
    add_compile_options(-ffp-contract=off -fno-fast-math)
else()
    message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

# Named configurations for Phase 0 verification. Sanitizers are supported by
# Clang/GCC only; MSVC builds fail closed if one is requested.
set(MME_SANITIZER "none" CACHE STRING "Sanitizer: none, address, undefined, or thread")
set_property(CACHE MME_SANITIZER PROPERTY STRINGS none address undefined thread)

if(NOT CMAKE_CONFIGURATION_TYPES)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel Bench ASan UBSan TSan)
    if(CMAKE_BUILD_TYPE STREQUAL "Bench")
        set(MME_BENCHMARK_BUILD ON CACHE BOOL "Enable benchmark-optimized build settings" FORCE)
    elseif(CMAKE_BUILD_TYPE STREQUAL "ASan")
        set(MME_SANITIZER address CACHE STRING "Sanitizer selection" FORCE)
    elseif(CMAKE_BUILD_TYPE STREQUAL "UBSan")
        set(MME_SANITIZER undefined CACHE STRING "Sanitizer selection" FORCE)
    elseif(CMAKE_BUILD_TYPE STREQUAL "TSan")
        set(MME_SANITIZER thread CACHE STRING "Sanitizer selection" FORCE)
    endif()
endif()

if(NOT MME_SANITIZER STREQUAL "none")
    if(MSVC)
        message(FATAL_ERROR "The selected sanitizer configuration requires Clang or GCC")
    endif()
    if(MME_SANITIZER STREQUAL "address")
        set(MME_SANITIZER_FLAG -fsanitize=address)
    elseif(MME_SANITIZER STREQUAL "undefined")
        set(MME_SANITIZER_FLAG -fsanitize=undefined)
    elseif(MME_SANITIZER STREQUAL "thread")
        set(MME_SANITIZER_FLAG -fsanitize=thread)
    else()
        message(FATAL_ERROR "Unknown MME_SANITIZER=${MME_SANITIZER}")
    endif()
    add_compile_options(${MME_SANITIZER_FLAG} -fno-omit-frame-pointer)
    add_link_options(${MME_SANITIZER_FLAG})
endif()

option(MME_BENCHMARK_BUILD "Enable benchmark-optimized build settings" OFF)
if(MME_BENCHMARK_BUILD)
    if(MSVC)
        add_compile_options(/O2)
    else()
        add_compile_options(-O3)
    endif()
endif()

find_package(spdlog REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(zstd CONFIG QUIET)
if(TARGET zstd::libzstd_static)
    set(MME_ZSTD_TARGET zstd::libzstd_static)
elseif(TARGET zstd::libzstd_shared)
    set(MME_ZSTD_TARGET zstd::libzstd_shared)
else()
    find_path(MME_ZSTD_INCLUDE_DIR zstd.h)
    find_library(MME_ZSTD_LIBRARY NAMES zstd libzstd)
    if(NOT MME_ZSTD_INCLUDE_DIR OR NOT MME_ZSTD_LIBRARY)
        message(FATAL_ERROR "Phase 2c requires the approved zstd development library")
    endif()
    add_library(mme_zstd UNKNOWN IMPORTED)
    set_target_properties(mme_zstd PROPERTIES
        IMPORTED_LOCATION "${MME_ZSTD_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MME_ZSTD_INCLUDE_DIR}")
    set(MME_ZSTD_TARGET mme_zstd)
endif()

add_library(phase1_core STATIC
    src/core/tsc_calibration.cpp
)
target_include_directories(phase1_core PUBLIC include)

add_library(phase2a_transport STATIC
    src/feed/transport_spike.cpp
)
target_include_directories(phase2a_transport PUBLIC include)
target_link_libraries(phase2a_transport PUBLIC Threads::Threads)

add_library(phase2b_feed STATIC
    src/feed/mt5_pipe_adapter.cpp
)
target_include_directories(phase2b_feed PUBLIC include)

add_library(phase2c_persist STATIC
    src/persist/wal_writer.cpp
    src/feed/recorder.cpp
)
target_include_directories(phase2c_persist PUBLIC include)
target_link_libraries(phase2c_persist PUBLIC ${MME_ZSTD_TARGET} Threads::Threads)

# Replay R1: read-only WAL access. Deliberately a separate target from
# phase2c_persist so nothing on the read path can link the writer by accident.
add_library(replay_r1_read STATIC
    src/persist/segment_stream.cpp
    src/persist/segment_catalog.cpp
    src/persist/wal_reader.cpp
)
target_include_directories(replay_r1_read PUBLIC include)

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

    foreach(phase1_test IN ITEMS
            fixed_point ring_buffer backpressure slab_pool no_alloc tsc_calibration)
        add_executable(phase1_${phase1_test} tests/cpp/${phase1_test}.cpp)
        target_include_directories(phase1_${phase1_test} PRIVATE include)
        target_link_libraries(phase1_${phase1_test} PRIVATE phase1_core Threads::Threads)
        add_test(NAME phase1_${phase1_test} COMMAND phase1_${phase1_test})
    endforeach()
    foreach(crash_boundary RANGE 1 20)
        if(crash_boundary LESS 10)
            set(crash_suffix "0${crash_boundary}")
        else()
            set(crash_suffix "${crash_boundary}")
        endif()
        add_executable(phase6_crash_${crash_suffix} tests/cpp/phase6_crash_${crash_suffix}.cpp)
        target_include_directories(phase6_crash_${crash_suffix} PRIVATE include tests/cpp)
        if(MSVC)
            target_link_options(phase6_crash_${crash_suffix} PRIVATE /STACK:8388608)
        endif()
        add_test(NAME phase6_crash_${crash_suffix} COMMAND phase6_crash_${crash_suffix})
    endforeach()

    add_executable(phase2a_protocol_layout tests/cpp/protocol_layout.cpp)
    target_include_directories(phase2a_protocol_layout PRIVATE include)
    add_test(NAME phase2a_protocol_layout COMMAND phase2a_protocol_layout)

    add_executable(phase2a_transport_test tests/cpp/transport_spike.cpp)
    target_link_libraries(phase2a_transport_test PRIVATE phase2a_transport)
    add_test(NAME phase2a_transport_test COMMAND phase2a_transport_test)

    add_executable(phase2b_feed_bridge tests/cpp/feed_bridge.cpp)
    target_link_libraries(phase2b_feed_bridge PRIVATE phase2b_feed)
    add_test(NAME phase2b_feed_bridge COMMAND phase2b_feed_bridge)

    # Replay R0: IClock / IFeedAdapter seams. No replay behaviour yet.
    add_executable(replay_r0_clock_abstractions tests/cpp/clock_abstractions.cpp)
    target_link_libraries(replay_r0_clock_abstractions
        PRIVATE phase2b_feed phase1_core Threads::Threads)
    add_test(NAME replay_r0_clock_abstractions COMMAND replay_r0_clock_abstractions)

    # Replay R1: read-only WAL access. Fixtures are produced by the real
    # WalWriter, so these read recorder-generated files rather than synthetic bytes.
    add_executable(replay_r1_segment_catalog tests/cpp/segment_catalog.cpp)
    target_link_libraries(replay_r1_segment_catalog PRIVATE replay_r1_read)
    add_test(NAME replay_r1_segment_catalog COMMAND replay_r1_segment_catalog)

    add_executable(replay_r1_wal_reader tests/cpp/wal_reader.cpp)
    target_link_libraries(replay_r1_wal_reader PRIVATE replay_r1_read phase2c_persist)
    add_test(NAME replay_r1_wal_reader COMMAND replay_r1_wal_reader)

    add_executable(phase2b_feed_ea_safety tests/cpp/feed_ea_safety.cpp)
    target_compile_definitions(phase2b_feed_ea_safety PRIVATE
        MME_FEED_EA_PATH="${CMAKE_SOURCE_DIR}/agent/mt5_bridge/mme_feed.mq5")
    add_test(NAME phase2b_feed_ea_safety COMMAND phase2b_feed_ea_safety)

    add_executable(phase2c_framing tests/cpp/framing.cpp)
    target_link_libraries(phase2c_framing PRIVATE phase2c_persist)
    add_test(NAME phase2c_framing COMMAND phase2c_framing)

    foreach(phase2c_test IN ITEMS wal tie_break wal_no_alloc)
        add_executable(phase2c_${phase2c_test} tests/cpp/${phase2c_test}.cpp)
        target_link_libraries(phase2c_${phase2c_test} PRIVATE phase2c_persist)
        add_test(NAME phase2c_${phase2c_test} COMMAND phase2c_${phase2c_test})
    endforeach()

    # Phase 3 — Order Book & Feature Engine. Header-only (include/book,
    # include/features), so the tests need no link library beyond the headers.
    # Test names mirror the Part 18 Phase 3 test list one-for-one.
    foreach(phase3_test IN ITEMS
            book_l1 book_l2 rebase crossed checksum welford warm_mask features book_no_alloc)
        add_executable(phase3_${phase3_test} tests/cpp/${phase3_test}.cpp)
        target_include_directories(phase3_${phase3_test} PRIVATE include)
        add_test(NAME phase3_${phase3_test} COMMAND phase3_${phase3_test})
    endforeach()

    # Phase 4 — Deterministic Replay. Composes the R1 read-only WAL stack with
    # the Phase 3 engines through the IFeedAdapter / IClock seams; no replay
    # variant of the book or feature code exists, by design.
    foreach(phase4_test IN ITEMS
            replay_determinism replay_virtual_clock replay_speed_independence
            replay_tie_break replay_no_alloc replay_fail_closed)
        add_executable(phase4_${phase4_test} tests/cpp/${phase4_test}.cpp)
        target_include_directories(phase4_${phase4_test} PRIVATE include tests/cpp)
        target_link_libraries(phase4_${phase4_test} PRIVATE replay_r1_read phase2c_persist)
        add_test(NAME phase4_${phase4_test} COMMAND phase4_${phase4_test})
    endforeach()

    # Phase 5 — Paper Execution Simulator. Header-only under include/exec.
    # These tests need no WAL and therefore no persistence library.
    foreach(phase5_test IN ITEMS
            philox_determinism market_order limit_order no_mid_fill cancel_replace
            partial_fill partial_fill_property_1m_cases queue_position latency_model
            slippage_model commission_model margin_rejection duplicate_order_id
            impossible_fill_rejection position_and_pnl hedging_positions reduce_only
            execution_no_alloc)
        add_executable(phase5_${phase5_test} tests/cpp/${phase5_test}.cpp)
        target_include_directories(phase5_${phase5_test} PRIVATE include tests/cpp)
        if(MSVC)
            # PaperBroker deliberately embeds its fixed-capacity journal so the
            # simulator hot path never allocates.  Its 1.3 MiB object exceeds
            # the PE default 1 MiB stack when tests construct it by value.
            target_link_options(phase5_${phase5_test} PRIVATE /STACK:8388608)
        endif()
        add_test(NAME phase5_${phase5_test} COMMAND phase5_${phase5_test})
    endforeach()

    # Simulator-in-the-loop tests replay a real WAL, so they link the read stack.
    foreach(phase5_replay_test IN ITEMS determinism_with_sim restart_recovery)
        add_executable(phase5_${phase5_replay_test} tests/cpp/${phase5_replay_test}.cpp)
        target_include_directories(phase5_${phase5_replay_test} PRIVATE include tests/cpp)
        target_link_libraries(phase5_${phase5_replay_test} PRIVATE replay_r1_read phase2c_persist)
        if(MSVC)
            target_link_options(phase5_${phase5_replay_test} PRIVATE /STACK:8388608)
        endif()
        add_test(NAME phase5_${phase5_replay_test} COMMAND phase5_${phase5_replay_test})
    endforeach()

    # Phase 6 — Risk, OMS/PMS, portfolio and single-instance enforcement.
    foreach(phase6_test IN ITEMS risk limits_verifier kill_switch margin_projection
            self_trade oms_states position_states idempotency recovery recovery_cases
            reconciliation portfolio instance_lock supervisor oms_capacity)
        add_executable(phase6_${phase6_test} tests/cpp/phase6_${phase6_test}.cpp)
        target_include_directories(phase6_${phase6_test} PRIVATE include tests/cpp)
        if(phase6_test STREQUAL "limits_verifier")
            target_compile_definitions(phase6_${phase6_test} PRIVATE MME_LIMITS_PATH="${CMAKE_SOURCE_DIR}/config/limits.json")
        endif()
        # exec::Journal is a 1.3 MiB by-value buffer, so recovery tests that
        # build several journals exceed MSVC's 1 MiB default stack.
        if(MSVC AND (phase6_test STREQUAL "recovery" OR phase6_test STREQUAL "recovery_cases"))
            target_link_options(phase6_${phase6_test} PRIVATE /STACK:16777216)
        endif()
        add_test(NAME phase6_${phase6_test} COMMAND phase6_${phase6_test})
    endforeach()

    # Phase 7 — baseline strategy. Header-only under include/strategy. Paper
    # only: nothing here links a broker, a socket or an order transport.
    foreach(phase7_test IN ITEMS signals strategy metrics l1_scalper)
        add_executable(phase7_${phase7_test} tests/cpp/phase7_${phase7_test}.cpp)
        target_include_directories(phase7_${phase7_test} PRIVATE include tests/cpp)
        add_test(NAME phase7_${phase7_test} COMMAND phase7_${phase7_test})
    endforeach()

    # Strategy-in-the-loop replay needs the read-only WAL stack.
    foreach(phase7_replay_test IN ITEMS replay_determinism wal_tailer)
        add_executable(phase7_${phase7_replay_test} tests/cpp/phase7_${phase7_replay_test}.cpp)
        target_include_directories(phase7_${phase7_replay_test} PRIVATE include tests/cpp)
        target_link_libraries(phase7_${phase7_replay_test} PRIVATE replay_r1_read phase2c_persist)
        if(MSVC)
            target_link_options(phase7_${phase7_replay_test} PRIVATE /STACK:16777216)
        endif()
        add_test(NAME phase7_${phase7_replay_test} COMMAND phase7_${phase7_replay_test})
    endforeach()

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

add_executable(phase1_bench src/apps/bench_main.cpp)
target_include_directories(phase1_bench PRIVATE include)
target_link_libraries(phase1_bench PRIVATE phase1_core Threads::Threads)

add_executable(phase2a_transport_spike src/apps/spike_transport_main.cpp)
target_link_libraries(phase2a_transport_spike PRIVATE phase2a_transport)

add_executable(phase2b_feed_latency src/apps/feed_latency_main.cpp)
target_link_libraries(phase2b_feed_latency PRIVATE phase2b_feed)

add_executable(phase2c_recorder src/apps/record_main.cpp)
target_link_libraries(phase2c_recorder PRIVATE phase2b_feed phase2c_persist Threads::Threads)

add_executable(phase2c_wal_bench src/apps/wal_bench_main.cpp)
target_link_libraries(phase2c_wal_bench PRIVATE phase2c_persist)

add_executable(phase3_book_bench src/apps/book_bench_main.cpp)
target_include_directories(phase3_book_bench PRIVATE include)
target_link_libraries(phase3_book_bench PRIVATE phase1_core Threads::Threads)

add_executable(phase4_replay src/apps/replay_main.cpp)
target_include_directories(phase4_replay PRIVATE include)
target_link_libraries(phase4_replay PRIVATE replay_r1_read phase2c_persist phase1_core Threads::Threads)

add_executable(phase5_exec_bench src/apps/exec_bench_main.cpp)
target_include_directories(phase5_exec_bench PRIVATE include)
target_link_libraries(phase5_exec_bench PRIVATE phase1_core Threads::Threads)

# Strict paper-trading runtime. Links the read-only WAL stack only: no writer,
# no transport, no broker API. Live trading is a compile-time false.
add_executable(paper_trade src/apps/paper_trade_main.cpp)
target_include_directories(paper_trade PRIVATE include)
target_link_libraries(paper_trade PRIVATE replay_r1_read phase2c_persist phase1_core Threads::Threads)
# Runtime holds the bounded trade log by value; MSVC's 1 MiB default stack is
# too tight. A native Windows build is required to observe the recorder's
# memory-mapped writes through the shared cache manager.
if(MSVC)
    target_link_options(paper_trade PRIVATE /STACK:16777216)
endif()

add_executable(throughput_bench src/apps/throughput_bench_main.cpp)
target_include_directories(throughput_bench PRIVATE include)
target_link_libraries(throughput_bench PRIVATE replay_r1_read phase2c_persist phase1_core Threads::Threads)
# Same reason as paper_trade: broker journal and trade ledger are held by value.
if(MSVC)
    target_link_options(throughput_bench PRIVATE /STACK:16777216)
endif()

add_executable(l1_tune src/apps/l1_tune_main.cpp)
target_include_directories(l1_tune PRIVATE include)
target_link_libraries(l1_tune PRIVATE replay_r1_read phase2c_persist phase1_core Threads::Threads)

add_executable(phase7_strategy_bench src/apps/strategy_bench_main.cpp)
target_include_directories(phase7_strategy_bench PRIVATE include)
target_link_libraries(phase7_strategy_bench PRIVATE phase1_core Threads::Threads)

add_executable(phase6_risk_bench src/apps/risk_bench_main.cpp)
target_include_directories(phase6_risk_bench PRIVATE include)

# Phase A (audit baseline): the harness already encodes a pass/fail gate in
# its exit code (risk p99 < 1000 ns, OMS transition p99 < 500 ns) but was
# never registered with CTest, so a latency regression here could not fail
# CI. Registering it is the entire fix -- the harness itself is untouched.
if(BUILD_TESTING)
    add_test(NAME phase6_risk_bench COMMAND phase6_risk_bench)
endif()

# Phase B: occupancy-scaling benchmark (1 / 1K / 100K / 500K / 1M live
# orders) for the redesigned Oms. Self-contained (no external fixture data,
# unlike throughput_bench) and has a real correctness signal in its exit
# code -- every create/find/transition is checked to actually succeed at
# each occupancy -- so, unlike throughput_bench, it is safe to register.
add_executable(oms_bench src/apps/oms_bench_main.cpp)
target_include_directories(oms_bench PRIVATE include)
if(BUILD_TESTING)
    add_test(NAME oms_bench COMMAND oms_bench)
endif()

# Phase C: sharded, single-writer-per-shard concurrent OMS (include/oms/sharded_oms.hpp).
add_executable(phase6_oms_concurrent tests/cpp/phase6_oms_concurrent.cpp)
target_include_directories(phase6_oms_concurrent PRIVATE include tests/cpp)
target_link_libraries(phase6_oms_concurrent PRIVATE Threads::Threads)
if(MSVC)
    target_link_options(phase6_oms_concurrent PRIVATE /STACK:16777216)
endif()
if(BUILD_TESTING)
    add_test(NAME phase6_oms_concurrent COMMAND phase6_oms_concurrent)
endif()

# Phase C benchmark: 3 thread counts x 4 load levels x 3 op phases, up to 1M
# orders per combination -- multiple minutes end to end. Not registered with
# CTest for the same reason throughput_bench (Phase A) wasn't: a benchmark
# this long does not belong in the default `ctest` run. Run manually.
add_executable(oms_concurrent_bench src/apps/oms_concurrent_bench_main.cpp)
target_include_directories(oms_concurrent_bench PRIVATE include)
target_link_libraries(oms_concurrent_bench PRIVATE Threads::Threads)

# Phase D: execution-report sequencing, dedup, deterministic reconciliation
# (include/oms/report_sequencer.hpp).
add_executable(phase6_report_sequencer tests/cpp/phase6_report_sequencer.cpp)
target_include_directories(phase6_report_sequencer PRIVATE include tests/cpp)
target_link_libraries(phase6_report_sequencer PRIVATE Threads::Threads)
if(BUILD_TESTING)
    add_test(NAME phase6_report_sequencer COMMAND phase6_report_sequencer)
endif()

# Phase D benchmark: report-processing latency/throughput at 1K/100K/1M live
# orders. Not CTest-registered -- same reasoning as throughput_bench and
# oms_concurrent_bench (long-running, informational, not a correctness gate).
add_executable(report_sequencer_bench src/apps/report_sequencer_bench_main.cpp)
target_include_directories(report_sequencer_bench PRIVATE include)
target_link_libraries(report_sequencer_bench PRIVATE Threads::Threads)

# Phase E: durable execution WAL (include/persist/execution_wal.hpp), reusing
# WalWriter/WalReader/SegmentCatalog. Links both persist libraries, same
# pattern as phase5_restart_recovery.
add_executable(phase7_execution_wal_smoke tests/cpp/phase7_execution_wal_smoke.cpp)
target_include_directories(phase7_execution_wal_smoke PRIVATE include tests/cpp)
target_link_libraries(phase7_execution_wal_smoke PRIVATE replay_r1_read phase2c_persist)
if(BUILD_TESTING)
    add_test(NAME phase7_execution_wal_smoke COMMAND phase7_execution_wal_smoke)
endif()

# Phase E: venue reconciliation (include/oms/venue_reconciliation.hpp) and
# the CONNECTED/DISCONNECTED/RECOVERING/RECONCILING/READY workflow
# (include/oms/recovery_workflow.hpp), fault-injection tests for all 13
# named scenarios.
add_executable(phase7_recovery_fault_injection tests/cpp/phase7_recovery_fault_injection.cpp)
target_include_directories(phase7_recovery_fault_injection PRIVATE include tests/cpp)
target_link_libraries(phase7_recovery_fault_injection PRIVATE replay_r1_read phase2c_persist)
if(MSVC)
    target_link_options(phase7_recovery_fault_injection PRIVATE /STACK:8388608)
endif()
if(BUILD_TESTING)
    add_test(NAME phase7_recovery_fault_injection COMMAND phase7_recovery_fault_injection)
endif()

# Phase E benchmark: WAL append/read/tail latency, restart recovery time,
# reconciliation time at 1K/100K/1M, memory, catch-up throughput,
# time-to-READY. Not CTest-registered -- same reasoning as the other
# long-running benchmarks in this file.
add_executable(recovery_bench src/apps/recovery_bench_main.cpp)
target_include_directories(recovery_bench PRIVATE include)
target_link_libraries(recovery_bench PRIVATE replay_r1_read phase2c_persist)
if(MSVC)
    target_link_options(recovery_bench PRIVATE /STACK:16777216)
endif()

# Phase F: deterministic multi-exchange architecture (include/oms/multi_venue.hpp),
# built entirely on Phase C's ShardedOms (extended with OpKind::snapshot for
# safe cross-thread shard reads) + Phase D/E's ReportSequencer/VenueAdapter/
# OrderReconciler, unmodified.
add_executable(phase7_multi_venue tests/cpp/phase7_multi_venue.cpp)
target_include_directories(phase7_multi_venue PRIVATE include tests/cpp)
target_link_libraries(phase7_multi_venue PRIVATE Threads::Threads)
if(MSVC)
    target_link_options(phase7_multi_venue PRIVATE /STACK:16777216)
endif()
if(BUILD_TESTING)
    add_test(NAME phase7_multi_venue COMMAND phase7_multi_venue)
endif()

# Phase F benchmark: 2 venues at 1K/100K/1M aggregate orders. Not
# CTest-registered -- same reasoning as every other long-running benchmark in
# this file.
add_executable(multi_venue_bench src/apps/multi_venue_bench_main.cpp)
target_include_directories(multi_venue_bench PRIVATE include)
target_link_libraries(multi_venue_bench PRIVATE Threads::Threads)
if(MSVC)
    target_link_options(multi_venue_bench PRIVATE /STACK:16777216)
endif()

# Phase G: deterministic per-exchange rate limiting + session constraints
# (include/oms/rate_limiter.hpp), layered on Phase F's VenueConnection
# (include/oms/multi_venue.hpp's new route_*()/configure_rate_limit()/
# set_session() methods) without modifying any Phase B-F component.
add_executable(phase7_rate_limiting tests/cpp/phase7_rate_limiting.cpp)
target_include_directories(phase7_rate_limiting PRIVATE include tests/cpp)
target_link_libraries(phase7_rate_limiting PRIVATE Threads::Threads)
if(MSVC)
    target_link_options(phase7_rate_limiting PRIVATE /STACK:16777216)
endif()
if(BUILD_TESTING)
    add_test(NAME phase7_rate_limiting COMMAND phase7_rate_limiting)
endif()

# Phase G benchmark: 2 venues at 1K/100K/1M aggregate orders. Not
# CTest-registered -- same reasoning as every other long-running benchmark in
# this file.
add_executable(rate_limiter_bench src/apps/rate_limiter_bench_main.cpp)
target_include_directories(rate_limiter_bench PRIVATE include)
target_link_libraries(rate_limiter_bench PRIVATE Threads::Threads)
if(MSVC)
    target_link_options(rate_limiter_bench PRIVATE /STACK:16777216)
endif()

# Phase H: deterministic multi-strategy scheduling and fair arbitration
# (include/oms/strategy_scheduler.hpp), layered strictly in front of Phase
# F/G's VenueConnection::route_*() without modifying Phase B-G.
add_executable(phase7_strategy_scheduler tests/cpp/phase7_strategy_scheduler.cpp)
target_include_directories(phase7_strategy_scheduler PRIVATE include tests/cpp)
target_link_libraries(phase7_strategy_scheduler PRIVATE Threads::Threads)
if(MSVC)
    target_link_options(phase7_strategy_scheduler PRIVATE /STACK:16777216)
endif()
if(BUILD_TESTING)
    add_test(NAME phase7_strategy_scheduler COMMAND phase7_strategy_scheduler)
endif()

# Phase H benchmark: 2/4/8 strategies x 2 venues at 1K/100K/1M aggregate
# requests. Not CTest-registered -- same reasoning as every other
# long-running benchmark in this file.
add_executable(strategy_scheduler_bench src/apps/strategy_scheduler_bench_main.cpp)
target_include_directories(strategy_scheduler_bench PRIVATE include)
target_link_libraries(strategy_scheduler_bench PRIVATE Threads::Threads)
if(MSVC)
    target_link_options(strategy_scheduler_bench PRIVATE /STACK:16777216)
endif()

# Phase I: concurrent risk/credit/exposure ledger (include/risk/risk_ledger.hpp)
# + the mandatory-risk-checked entry point (include/oms/risk_gated_router.hpp),
# layered on Phase F/G/H without modifying them (Phase G's VenueConnection
# gains one small, additive, opt-in RiskGate hook -- see risk_gate.hpp).
add_executable(phase7_risk_ledger tests/cpp/phase7_risk_ledger.cpp)
target_include_directories(phase7_risk_ledger PRIVATE include tests/cpp)
target_link_libraries(phase7_risk_ledger PRIVATE Threads::Threads)
if(MSVC)
    target_link_options(phase7_risk_ledger PRIVATE /STACK:16777216)
endif()
if(BUILD_TESTING)
    add_test(NAME phase7_risk_ledger COMMAND phase7_risk_ledger)
endif()

# Phase I benchmark: worker-count scaling (1/2/4/max) x 1K/100K/1M, plus a
# strategy-count sweep. Not CTest-registered -- same reasoning as every
# other long-running benchmark in this file.
add_executable(risk_ledger_bench src/apps/risk_ledger_bench_main.cpp)
target_include_directories(risk_ledger_bench PRIVATE include)
target_link_libraries(risk_ledger_bench PRIVATE Threads::Threads)
if(MSVC)
    target_link_options(risk_ledger_bench PRIVATE /STACK:16777216)
endif()

# Phase J (blocker fix): streaming/segmented recovery removing the
# 16384-record ceiling (include/oms/recovery_streaming.hpp), additive to
# oms::recover() (include/oms/recovery.hpp, itself unmodified apart from
# RecoveryState's backward-compatible capacity constructor parameter).
add_executable(phase7_recovery_streaming tests/cpp/phase7_recovery_streaming.cpp)
target_include_directories(phase7_recovery_streaming PRIVATE include tests/cpp)
target_link_libraries(phase7_recovery_streaming PRIVATE Threads::Threads)
if(MSVC)
    target_link_options(phase7_recovery_streaming PRIVATE /STACK:16777216)
endif()
if(BUILD_TESTING)
    add_test(NAME phase7_recovery_streaming COMMAND phase7_recovery_streaming)
endif()

# Phase J (blocker fix): live WAL wiring (ShardedOms::apply()'s emit_wal(),
# include/persist/live_wal_recorder.hpp) + crash-recovery correctness proof.
add_executable(phase7_wal_crash_recovery tests/cpp/phase7_wal_crash_recovery.cpp)
target_include_directories(phase7_wal_crash_recovery PRIVATE include tests/cpp)
target_link_libraries(phase7_wal_crash_recovery PRIVATE replay_r1_read phase2c_persist Threads::Threads)
if(MSVC)
    target_link_options(phase7_wal_crash_recovery PRIVATE /STACK:16777216)
endif()
if(BUILD_TESTING)
    add_test(NAME phase7_wal_crash_recovery COMMAND phase7_wal_crash_recovery)
endif()

# Phase J (blocker fix): restart-READY gate (include/oms/restart_recovery.hpp)
# -- proves no new exposure is admitted until WAL replay + risk
# reconstruction + venue reconciliation together reach READY, including at
# 1,000,000-order scale.
add_executable(phase7_restart_ready_gate tests/cpp/phase7_restart_ready_gate.cpp)
target_include_directories(phase7_restart_ready_gate PRIVATE include tests/cpp)
target_link_libraries(phase7_restart_ready_gate PRIVATE replay_r1_read phase2c_persist Threads::Threads)
if(MSVC)
    target_link_options(phase7_restart_ready_gate PRIVATE /STACK:16777216)
endif()
if(BUILD_TESTING)
    add_test(NAME phase7_restart_ready_gate COMMAND phase7_restart_ready_gate)
endif()

# Phase J (blocker fix) benchmark: WAL write overhead, replay throughput,
# 1K/100K/1M recovery timing, and a full crash-with-1M-live-orders scenario.
# Not CTest-registered -- same reasoning as every other long-running
# benchmark in this file.
add_executable(restart_recovery_bench src/apps/restart_recovery_bench_main.cpp)
target_include_directories(restart_recovery_bench PRIVATE include)
target_link_libraries(restart_recovery_bench PRIVATE replay_r1_read phase2c_persist Threads::Threads)
if(MSVC)
    target_link_options(restart_recovery_bench PRIVATE /STACK:16777216)
endif()

# Phase J: final validation/hardening. End-to-end integration proof for the
# complete A-I pipeline as one composed system, including the regression
# test for the one real defect this phase's audit found and fixed (self-trade
# prevention existed since Phase H but was never wired into Phase I's
# mandatory admission path -- now closed in include/oms/risk_gated_router.hpp).
add_executable(phase7_integration_end_to_end tests/cpp/phase7_integration_end_to_end.cpp)
target_include_directories(phase7_integration_end_to_end PRIVATE include tests/cpp)
target_link_libraries(phase7_integration_end_to_end PRIVATE Threads::Threads)
if(MSVC)
    target_link_options(phase7_integration_end_to_end PRIVATE /STACK:16777216)
endif()
if(BUILD_TESTING)
    add_test(NAME phase7_integration_end_to_end COMMAND phase7_integration_end_to_end)
endif()

# Phase J torture-test binary: max-scale load (up to 1M orders), soak,
# adversarial combined fault-injection, determinism-at-scale, and
# recovery-at-scale, all in one long-running executable (they share the same
# venue/strategy/risk-ledger setup code). Not CTest-registered -- run
# manually, same reasoning as every other long-running binary in this file.
add_executable(phase_j_torture src/apps/phase_j_torture_main.cpp)
target_include_directories(phase_j_torture PRIVATE include)
target_link_libraries(phase_j_torture PRIVATE replay_r1_read phase2c_persist Threads::Threads)
if(MSVC)
    target_link_options(phase_j_torture PRIVATE /STACK:16777216)
endif()
