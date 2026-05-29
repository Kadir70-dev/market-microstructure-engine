# CLAUDE.md

Guidance for Claude Code (and any future AI session) working in this repo.
**Read this top-to-bottom once; it is the single source of truth for how the
system is wired, what is real, and what must never happen.**

> ⚠️ **SAFETY RULE #0 — NO REAL-MONEY EXECUTION.** This system does not place
> trades and must not be made to. There is no order-execution code anywhere.
> Do not add one without an explicit, deliberate, human-confirmed Phase 3
> decision. See [Safety rules](#safety-rules-non-negotiable).

---

## 1. What this is (one paragraph)

A C++ market-intelligence engine. Every 30s it polls live quotes (TwelveData
REST by default, or an MT5 read-only bridge), runs them through a
momentum + volatility + validation pipeline, and persists raw ticks + derived
signals + quality scores to SQLite as immutable ground truth. A separate
read-only C++ backtest binary and a read-only Python "Hermes" analyst evaluate
that data with strict look-ahead/staleness gates. **It is research and data-
collection infrastructure — not a trading bot yet, by deliberate design.**

---

## 2. Current maturity level

**Phase 0 (foundation) + Phase 1 (read-only MT5) are implemented. Phase 2
(edge research) is the active frontier.** No money has ever been at risk and
none can be without new, gated code.

Maturity by dimension (1–10):

| Dimension | Score | Note |
|---|---:|---|
| Architecture & data model | 8 | Immutable ground truth, clean module split, provider abstraction |
| Data integrity / look-ahead safety | 9 | Structurally prevented, not just avoided |
| Ops / survivability | 8 | Idempotent lifecycle, health board, recovery, cron+systemd |
| Documentation | 8 | README + this file + 5 ops docs + protocol spec |
| Safety (no-trade firewall) | 9 | Execution path absent + double demo tripwires |
| Test coverage | 5 | Pure logic unit-tested (66 C++ checks + 8 Hermes tests) in CI; I/O edges not yet covered |
| Demonstrated alpha | 1 | Intentionally unproven; not enough data yet |
| **Portfolio readiness** | **8** | Strong, honest, recruiter-ready after Week 0 |

---

## 3. Build & run (exact commands)

CMake out-of-source build. Two executables: `engine` (live loop) and
`engine_backtest` (read-only eval). **Both must run from `build/`** — they open
`../config/api_key.txt` and `../data/engine.db` relative to CWD.

```bash
# deps (Ubuntu)
sudo apt install build-essential cmake libspdlog-dev libcurl4-openssl-dev \
                 libsqlite3-dev nlohmann-json3-dev sqlite3 python3

# build
cmake -S . -B build
cmake --build build

# run
cd build && ./engine                          # live 30s loop
cd build && ./engine_backtest                 # eval vs accumulated data
cd build && ./engine_backtest /path/other.db  # alt DB

# provider selection (env)
MME_PROVIDER=mt5 MME_MT5_HOST=127.0.0.1 MME_MT5_PORT=7777 ./engine
MME_DB_PATH=../data/mt5.db ./engine           # write to a separate DB

# Hermes daily report (from repo root)
python3 -m agent.hermes.daily_report --date 2026-05-24

# managed ops
./ops/start_engine.sh ; ./ops/status_engine.sh ; ./ops/stop_engine.sh
python3 ops/health_check.py [--json] [--skip-process]
./ops/run_eod_pipeline.sh [YYYY-MM-DD]        # health + backtest + Hermes snapshots
crontab ops/crontab.example                   # session schedule + health probe

# tests
ctest --test-dir build --output-on-failure    # C++ unit tests + Hermes tests
./build/unit_tests                             # C++ only (66 checks)
python3 -m unittest agent.hermes.tests.test_daily_report -v   # Hermes only
```

Tests run in CI (`.github/workflows/ci.yml`) on every push/PR to `main`. No
linter config yet.

---

## 4. Folder-by-folder

```
market-microstructure-engine/
├── CMakeLists.txt          Two targets; no glob — add new .cpp to add_executable manually.
├── README.md               Recruiter-facing overview + Mermaid architecture.
├── CLAUDE.md               This file — engineering source of truth.
│
├── include/                Public headers, mirror of src/ layout.
│   ├── market_data/        provider.hpp (interface), twelvedata_provider, mt5_provider, market_fetcher
│   ├── indicators/         momentum.hpp, volatility.hpp
│   ├── validation/         validation.hpp (staleness, session, confidence, quality)
│   ├── storage/            sqlite_logger.hpp
│   └── evaluation/         metrics.hpp (pure, testable metric math)
│
├── src/
│   ├── main.cpp            Control loop, signal handling, log setup, provider selection, processSymbol().
│   ├── market_data/        HTTP fetch (libcurl) + provider impls. NOTE: market_fetcher logs raw API
│   │                       response at info — can leak the key-bearing URL. Scrub if you touch it.
│   ├── indicators/         momentum.cpp, volatility.cpp
│   ├── validation/         validation.cpp
│   ├── storage/            sqlite_logger.cpp (best-effort; never crashes the engine)
│   └── evaluation/         main.cpp (SQL I/O + table printing), metrics.cpp (pure math)
│
├── tests/                  unit_tests.cpp — zero-dep C++ tests (validation + metrics), CTest-registered.
├── .github/workflows/      ci.yml — build + CTest on every push/PR to main.
│
├── dashboard/              Next.js read-only observability UI (TS/Tailwind/Recharts).
│                           Opens engine.db READ-ONLY via better-sqlite3 (lazy, optional);
│                           DEMO data by default, DASHBOARD_DB_PATH for live. lib/analytics.ts
│                           mirrors the backtest's look-ahead gates + cost model. Writes NOTHING.
│
├── agent/
│   ├── hermes/             Read-only Python analyst. daily_report.py + db.py. Writes reports/*.md.
│   ├── mt5_bridge/         MT5 feed. LINUX/WINE: use mt5_file_export.mq5 (EA writes CSV the
│   │                       engine reads — Python MetaTrader5 pkg is Windows-only). mt5_bridge.py
│   │                       (NDJSON/TCP) is the Windows-host-only alternative. Both read-only/demo-gated.
│   └── research/           Predictive baseline (LogisticRegression, walk-forward). train.py +
│                           features.py + synthetic.py + quant.py. Outputs results/model_results.json
│                           (read by dashboard). Own .venv (gitignored). --source db|synthetic|auto.
│                           HONEST: refuses to train on untrainable live data; says so.
│
├── ops/                    Production shell + python. See §10. common.sh is sourced by all *.sh.
│   ├── .env / .env.example secrets + schedule (.env is gitignored)
│   ├── crontab.example     session start/stop + 10-min health probe
│   ├── systemd/            24/5 stack: engine (Restart=always+boot) + alert(OnFailure) +
│   │                       health.timer + cleanup.timer. See ops/systemd/README.md.
│   ├── install_systemd.sh  one-shot installer (sudo) for the 24/5 stack
│   └── mme_cleanup.sh      weekly journald vacuum + WAL checkpoint + log prune
│
├── docs/                   OPERATIONS.md, RUNBOOK.md, RECOVERY.md, MONDAY_CHECKLIST.md, MT5_BRIDGE.md
├── reports/snapshots/      Committed sample backtest/health snapshots (showcase)
│
├── config/api_key.txt      SECRET — gitignored. TwelveData key, one line.
├── data/engine.db          SQLite ground truth — gitignored.
├── logs/                   engine.log (rotating 10MB×7), engine.out/.err, ops.log — gitignored.
└── run/                    engine.pid — gitignored.
```

---

## 5. Data flow

**Hot path (live, 30s):**
```
provider.fetchQuote() ──► price>0? ──► capture single ts
   ├─ updatePriceHistory (window=10)
   ├─ momentum / volatility / staleness / session
   ├─ confidence / tradeQuality / grade
   └─ logger: beginCycle → recordTick + recordSignal + recordQuality → endCycle  (one transaction)
```
All three rows share one `ts` and commit atomically. Partial-failure: if one
symbol returns price ≤ 0 it is skipped; the others still record.

**Cold path (read-only, on demand / EOD):**
```
engine.db ──(read-only)──► engine_backtest  → stdout / reports/snapshots/*.txt
          └─(read-only)──► Hermes           → agent/hermes/reports/<date>.md → Telegram summary
```
The cold path opens the DB read-only. Analysis can never corrupt collected data.

---

## 6. Database schema (`data/engine.db`)

Three tables, each indexed on `(symbol, ts)`:

- **`ticks(ts, symbol, price)`** — raw mid prices. Ground truth, never derived.
- **`signals(ts, symbol, momentum, vol_score, vol_regime)`** — indicator output per cycle.
- **`quality_scores(ts, symbol, stale, session, confidence, trade_quality, grade)`** — validation output.

The split is deliberate: signals & quality are **re-derivable** from `ticks`.
Change a formula → re-run history, no re-collection. The backtest reconstructs
returns by joining `signals ⋈ quality_scores ⋈ ticks` on `(ts, symbol)`.

> Historical note: `ticks` contains a few legacy `WTI` rows from before the
> switch to the `USO` proxy. Harmless; just don't be surprised by a 4th symbol.

---

## 7. Indicator & validation semantics

- **Volatility** (`indicators/volatility.cpp`): stddev of log-returns over the
  window. Scale-invariant — same formula for EUR/USD (~1.08), gold (~2300),
  USO (~140). Thresholds `>0.001 = HIGH`, `>0.0003 = MEDIUM` are **hardcoded**
  for ~30s-tick log returns. *TODO: replace with empirical percentiles from
  `signals` history once enough data exists.*
- **Momentum** (`indicators/momentum.cpp`): last-vs-previous with ±1e-7
  dead-zone. Fixed threshold across all instruments — a known weakness; should
  scale with recent volatility.
- **Staleness** (`validation.cpp::isStale`): last N prices byte-equal within
  tolerance (lookback=3). Catches weekend closes, cached API responses,
  symbol-resolution bugs.
- **Session**: UTC-based, no DST. `Closed` over FX weekend (Fri 22:00 → Sun 22:00 UTC).
- **Confidence (0–100)**: hard-zero if stale; else 100 × warm-up fraction ×
  vol-regime (HIGH 0.5 / MEDIUM 0.8 / LOW 1.0) × momentum (Neutral 0.3).
- **Trade quality (0–100)**: confidence × session multiplier (Closed 0 / Asia
  0.6 / London 1.0 / LondonNY 1.0 / NewYork 0.95). Grade: A≥75, B≥50, C≥25, else D.

---

## 8. Backtest invariants (`engine_backtest`)

Separate binary, links only sqlite3. Joins the three tables, reports bull/bear
accuracy + mean directional return by symbol / vol-regime / confidence-band at
+60s / +300s / +900s, **gross and net of a per-symbol round-trip cost model**.

Hard gates (do not weaken these — they are the whole point):
- **Baseline** = last tick at/before signal, within 120s. No baseline from a
  dead-engine gap.
- **Future** = first tick at/after `ts+horizon`, within one-cycle tolerance
  (120/360/960s). Outside → **excluded**, never fabricated.
- **Stale** signals excluded entirely.
- **Neutral** momentum → counted in N, contributes to no accuracy/return.

Pure math is in `evaluation/metrics.{hpp,cpp}` (testable in isolation); SQL I/O
+ printing in `evaluation/main.cpp`. Hermes mirrors these exact rules in Python.

---

## 9. Hermes (read-only analyst)

`agent/hermes/daily_report.py` — reads `engine.db`, writes
`agent/hermes/reports/<date>.md`. Computes signal mix, per-symbol confidence/
quality, forward-60s accuracy by confidence band (same exclusion rules as the
backtest), session/regime distributions, an auto **Problems Found** section,
and file-specific **Recommendations**. `agent/hermes/db.py` holds the read-only
connection helpers and the at-or-before / at-or-after price lookups.

**Hermes never places trades, never connects to a broker, never bypasses risk.**
It has no capability to — keep it that way.

---

## 10. MT5 bridge (read-only, demo-gated)

**Linux/Wine note:** the `MetaTrader5` Python pkg is Windows-only, so on this box
the MT5 path is `mt5_file_export.mq5` (an MQL5 EA that writes a CSV the engine
reads). A `FileProvider` to consume it is deferred until MT5-under-Wine is live.
Until then the engine collects via TwelveData (default). The Python bridge below
is the Windows-host-only alternative.

`agent/mt5_bridge/mt5_bridge.py` — NDJSON-over-TCP sidecar (default
`127.0.0.1:7777`), runs on a Windows MT5 host. **Only `ping` and `quote` ops
exist. No order ops in the code.** Select it from the engine with
`MME_PROVIDER=mt5`.

Two independent demo tripwires:
1. Bridge refuses to start against a non-DEMO account unless **both**
   `ENABLE_LIVE_TRADING=true` and `confirm_live_trading.txt` exist.
2. Bridge `ping` returns `demo_only`; the **C++ engine refuses to start** if it
   sees `demo_only=false`.

`MT5_BRIDGE_MOCK=true` serves a synthetic random walk (Linux dev, no MT5
install). Probe it: `ops/probe_mt5_bridge.sh` (exit 0 ok / 1 unreachable / 2
reachable-but-not-demo). Spec: `agent/mt5_bridge/protocol.md`.

---

## 11. Ops layer (production-style)

All `ops/*.sh` source `ops/common.sh` (repo-root resolution, PID/log paths,
`.env` load, `ops_log`, liveness checks, `notify`). All are run from repo root.

| Script | Purpose |
|---|---|
| `start_engine.sh` | Background start, **idempotent**, materializes key from `.env`, verifies it stayed up 2s, cleans stale PID. |
| `stop_engine.sh` | Graceful SIGTERM, 15s wait, escalate to SIGKILL. |
| `restart_engine.sh` | stop → start (recovery flows). |
| `status_engine.sh` | RUNNING/NOT, pid/uptime/cpu/mem, last log line. Exit 0/1. |
| `health_check.py` | 6-point read-only board: process / db present / recent write / log fresh / api key / not-frozen. Exit 0 PASS, 1 FAIL, 2 WARN. `--skip-process` for EOD. |
| `log_health.sh` | Log size, rotations, error counts, staleness. |
| `run_session_start.sh` / `run_session_end.sh` | Cron entry points; end-of-session stops engine + runs Hermes + Telegram summary. |
| `run_health_probe.sh` | Cron health; pages Telegram only on FAIL (WARN logged silently). |
| `run_eod_pipeline.sh` | health + backtest snapshot + Hermes report + Telegram. Idempotent, read-only on the engine. |
| `probe_mt5_bridge.sh` | One-shot bridge ping + demo_only check. |
| `telegram_notify.sh` | Infra alerts only. No-op if creds unset. **Never carries a trading action.** |
| `systemd/` | **24/5 production stack** (preferred over cron): `mme-engine` (Restart=always, boot-start, crash-loop guard), `mme-alert` (Telegram on failed state), `mme-health.timer` (10-min read-only probe, skips PID check), `mme-cleanup.timer` (weekly journald vacuum + WAL checkpoint + log prune). Install: `sudo ops/install_systemd.sh`. |
| `mme_cleanup.sh` | Disk-cleanup policy; safe to run live. |

Liveness is PID-recycling-aware: it checks `kill -0` **and** `comm == engine`.

---

## 12. Safety rules (non-negotiable)

1. **Do not add an order-execution code path.** Not to the engine, not to
   Hermes, not to the bridge — without an explicit human Phase 3 decision.
2. **Telegram is infra-only.** Never route a trade through `notify`/
   `telegram_notify.sh`. The comment contract in that file is binding.
3. **Demo tripwires stay.** Don't remove or weaken the `demo_only` checks in
   `main.cpp` or `mt5_bridge.py`.
4. **Backtest gates stay.** Don't relax the look-ahead/staleness exclusions to
   make numbers look better.
5. **Never commit secrets.** `config/api_key.txt` and `ops/.env` are gitignored.
   The fetcher logs raw API responses at info — scrub the key-bearing URL if you
   edit `market_fetcher.cpp` before sharing logs.
6. **The DB is ground truth.** Cold-path tools open it read-only; keep it that way.

---

## 13. Coding conventions

- **C++17**, one purpose per file. Adding an indicator = new
  `include/indicators/*.hpp` + `src/indicators/*.cpp` + one line in
  `processSymbol` + one line in `CMakeLists.txt` (no glob).
- **Keep modules dependency-light.** `engine_backtest` links only sqlite3 — keep
  it that way (pure metric math has no engine deps).
- **SQLite logger is best-effort:** every `record*` is a no-op when the prepared
  statement is null; a broken DB must never crash the engine.
- **Ops scripts:** `set -euo pipefail` (or `set -u` for read-only status
  scripts), source `common.sh`, resolve paths from the script location not CWD,
  degrade gracefully when creds/files are missing.
- **Python:** stdlib-first, read-only against the DB, `from __future__ import
  annotations`, type hints. No heavyweight deps.
- **Comments explain *why*, not *what*** — match the existing dense-rationale
  style in `main.cpp` and the ops scripts.

---

## 14. Common debugging steps

- **Engine won't start:** `cat logs/engine.err`; check `config/api_key.txt`
  non-empty; confirm you're in `build/` (paths are relative).
- **"NOT RUNNING" but you started it:** stale PID — `status_engine.sh` reports
  it; `start_engine.sh` auto-cleans stale PIDs.
- **Feed looks frozen:** `health_check.py` `price_not_frozen` WARN. Could be
  legitimately closed market, rate-limit, or symbol resolution. Check
  `log_health.sh` error counts and TwelveData rate-limit headers in the log.
- **Backtest shows N=0 / all neutral:** expected on small/low-vol data — not a
  bug. Momentum dead-zone (±1e-7) + LOW vol → mostly Neutral. Collect more,
  session-aligned.
- **Hermes "No signals":** engine wasn't running that UTC day, or wrong
  `--date`. Check tick date range:
  `sqlite3 data/engine.db "SELECT datetime(MIN(ts),'unixepoch'),datetime(MAX(ts),'unixepoch') FROM ticks"`.
- **MT5 bridge:** `ops/probe_mt5_bridge.sh`; on Linux dev use
  `MT5_BRIDGE_MOCK=true python3 agent/mt5_bridge/mt5_bridge.py`.

---

## 15. Known issues / technical debt

1. **Tests cover pure logic only.** `validation.cpp`, `evaluation/metrics.cpp`,
   and Hermes report generation are unit-tested in CI. The HTTP fetcher,
   SQLite writer, and MT5 socket path are NOT yet covered — add integration
   tests (mock provider + temp DB) next. No linter config yet either.
2. **Volatility thresholds hardcoded** — calibrate from `signals` percentiles.
3. **Momentum dead-zone is a fixed ±1e-7** across all instruments — should scale
   with each symbol's recent volatility.
4. **`market_fetcher.cpp` logs raw API responses at info** — potential key leak
   in shared logs.
5. **Free-tier rate ceiling** (8 req/min) leaves no room for a 4th symbol at 30s.
6. **No demonstrated edge** — current net return ~0 (correct, honest, but
   unproven).
7. **No dashboard** — roadmap Phase 4.

---

## 16. Roadmap

`0 Foundation ✅ → 1 Read-only MT5 ✅(code) → 2 Observability dashboard ✅ →
3 Edge research ⏳ (honest predictive baseline + more data) → 4 Paper/demo
execution (gated) → 5 Sized live (only if edge survives demo)`. Prove
survivability & honesty before alpha; prove alpha before risk. Full table in
`README.md`.

**Dashboard safety note:** the dashboard is read-only by construction
(`better-sqlite3` opened with `readonly:true`; no write path exists). Keep it
that way — it is an observability surface, never a control surface.

---

## 17. "Do not overengineer" principle

This codebase earns its keep by being **simple, honest, and operationally
real** — not by being clever. Before adding anything, ask:

- Does this serve data integrity, survivability, safety, or honest evaluation?
  If not, it probably doesn't belong yet.
- Can it be a new single-purpose file + one wiring line, instead of a framework?
  Prefer that.
- Am I adding an abstraction for a second use case that doesn't exist yet? Don't.
- Am I about to add a dependency to `engine_backtest`? Almost certainly don't.

The strength of this project is restraint: it builds the boring foundation
correctly and refuses to pretend it has an edge it hasn't measured. Keep that.
