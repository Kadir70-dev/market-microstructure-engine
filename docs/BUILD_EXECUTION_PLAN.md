# Build and Execution Plan

Architecture Version 1.0 is frozen and is the single source of truth.

## Safety baseline

- Safety Rule #0 remains in force: no real-money execution.
- Live trading is disabled. Phase 0 creates no live executable, live compile
  definition, arm mechanism, or order path.
- Phases execute strictly in order. A phase may start only after the previous
  checkpoint is complete and supported by evidence.

## Phase checkpoints

| Phase | Checkpoint | Status |
|---|---|---|
| 0 | `phase0-foundations` | INCOMPLETE |
| 1 | `phase1-core-types` | NOT STARTED |
| 2 | `phase2-feed-recorder` | NOT STARTED |
| 3 | `phase3-book-features` | NOT STARTED |
| 4 | `phase4-deterministic-replay` | NOT STARTED |
| 5 | `phase5-paper-simulator` | NOT STARTED |
| 6 | `phase6-risk-oms` | NOT STARTED |
| 7 | `phase7-strategies` | NOT STARTED |
| 8 | `phase8-research-gates` | NOT STARTED |
| 9 | `phase9-mt5-paper` | NOT STARTED |
| 10 | `phase10-live-path-disabled` | NOT STARTED |
| 11 | withheld | NOT AUTHORIZED |

## Phase 0 build profiles

- Normal: default CMake configuration.
- Benchmark: configure a single-config generator with `-DCMAKE_BUILD_TYPE=Bench`
  or use `-DMME_BENCHMARK_BUILD=ON` with a multi-config generator.
- Sanitizers on supported Clang/GCC hosts: configure `MME_SANITIZER` as
  `address`, `undefined`, or `thread`; single-config generators also expose
  the named `ASan`, `UBSan`, and `TSan` build types.
- Windows uses strict floating-point semantics through `/fp:strict`.
- GCC/Clang use `-ffp-contract=off -fno-fast-math`; `-march=native` is not used.

Phase 0 requires green Linux and Windows builds, documentation lint, and a
measured clean build below 60 seconds. Results belong in
`CURRENT_CHECKPOINT.md`; configuration alone is not evidence.
