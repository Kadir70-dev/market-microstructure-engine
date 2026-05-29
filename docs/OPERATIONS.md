# Operations architecture

How the infra layer is organized. Read this once, then use `RUNBOOK.md` daily.

## Directory layout

```
market-microstructure-engine/
├── build/                  # cmake out-of-source build (engine, engine_backtest)
├── data/engine.db          # SQLite — single source of truth for all derived data
├── logs/                   # engine.log (rotating), engine.out/err, ops.log
├── run/engine.pid          # written by start_engine.sh, removed by stop_engine.sh
├── reports/snapshots/      # EOD pipeline outputs (health, backtest text dumps)
├── agent/hermes/           # analysis-only intelligence layer (read-only DB access)
│   ├── db.py               # mode=ro SQLite reader
│   ├── daily_report.py     # CLI: python -m agent.hermes.daily_report
│   └── reports/<date>.md
├── ops/                    # this layer
│   ├── common.sh           # path resolution, .env loading, PID helpers, notify()
│   ├── .env.example        # operator-supplied secrets (copy to .env; gitignored)
│   ├── start/stop/status/restart_engine.sh
│   ├── health_check.py
│   ├── log_health.sh
│   ├── telegram_notify.sh
│   ├── run_session_start.sh, run_session_end.sh
│   ├── run_health_probe.sh
│   ├── run_eod_pipeline.sh
│   └── crontab.example
└── docs/
    ├── RUNBOOK.md
    ├── OPERATIONS.md         (this file)
    ├── RECOVERY.md
    └── MONDAY_CHECKLIST.md
```

## What runs when (per crontab.example)

| Time (UTC) | Days | Script | Effect |
|---|---|---|---|
| 06:30 | Mon-Fri | `run_session_start.sh` | Start engine 30 min before London open |
| 07:00–21:00 every 10 min | Mon-Fri | `run_health_probe.sh` | Page on FAIL, log WARN |
| 21:30 | Mon-Fri | `run_session_end.sh` | Stop engine, run Hermes, Telegram summary |

The schedule is documentation — `crontab.example` is not auto-installed. Install with `crontab ops/crontab.example` when you're ready.

## What does what (one-line summaries)

| Script | Purpose | Side effects |
|---|---|---|
| `common.sh` | Path constants, env loading, PID helpers, `notify()` | none (sourced only) |
| `start_engine.sh` | Launch engine in background, write PID, verify it stayed up | writes PID file |
| `stop_engine.sh` | SIGTERM → 15s wait → SIGKILL fallback | removes PID file |
| `status_engine.sh` | Print state + last log line | none |
| `restart_engine.sh` | stop then start | as above |
| `health_check.py` | 6 checks: process, DB, DB-write-recency, log, key, frozen-feed | none (read-only) |
| `log_health.sh` | spdlog rotation status + error counts in tail | none |
| `telegram_notify.sh` | Post one message to a Telegram chat | external HTTP only |
| `run_session_start.sh` | Cron wrapper; skips Saturday | calls start_engine.sh |
| `run_session_end.sh` | Stop engine, run Hermes for today | stops engine, writes report |
| `run_health_probe.sh` | Cron wrapper; notifies on FAIL only | calls health_check.py |
| `run_eod_pipeline.sh` | Backfill-safe: health + backtest snapshot + Hermes + telegram | writes to `reports/snapshots/`, `agent/hermes/reports/` |

## Boundaries

1. **Engine and Hermes never share a DB connection.** Engine writes; Hermes opens `mode=ro` and SQLite rejects writes at the driver layer.
2. **No trading touchpoint exists anywhere in this layer.** Telegram is used only for engine lifecycle and EOD report summary. No script knows how to place an order. The engine reads quotes from the local MT5 bridge over a TCP socket; the only external HTTP call in the whole system is Telegram (from `telegram_notify.sh`).
3. **The C++ engine is the only writer to `engine.db`.** Hermes and the backtest harness are read-only by URI mode.
4. **Process state is single-file.** One engine, one PID file at `run/engine.pid`. Multi-instance is not supported and not needed.

## Telemetry boundaries

`telegram_notify.sh` is gracefully optional. Every caller invokes `notify "..."`; if credentials are unset, it logs `telegram_notify: skipped (creds unset)` and returns 0. This means scripts never branch on "is telegram configured" — they just call it.
