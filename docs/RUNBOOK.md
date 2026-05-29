# Runbook — daily operations

Cheat sheet for routine actions. Assumes you're at the repo root.

## Build

```bash
cmake -S . -B build && cmake --build build
```

The `engine` and `engine_backtest` binaries land in `build/`.

## Engine lifecycle

| Action | Command | Exit code |
|---|---|---|
| Start (background) | `ops/start_engine.sh` | 0 ok, 2 no binary, 3 no key, 4 died on launch |
| Stop (graceful) | `ops/stop_engine.sh` | 0 ok, 5 SIGKILL also failed |
| Restart | `ops/restart_engine.sh` | same as start |
| Status | `ops/status_engine.sh` | 0 running, 1 not running |

`start_engine.sh` is idempotent — second call is a no-op if engine is already alive.

## Health

```bash
ops/health_check.py            # human-readable, exit 0/1/2 (pass/fail/warn)
ops/health_check.py --json     # for monitoring integration
ops/log_health.sh              # log volume + recent error counts
```

## Reports

```bash
# Hermes report for today
python3 -m agent.hermes.daily_report

# Hermes report for a specific UTC date
python3 -m agent.hermes.daily_report --date 2026-05-25

# Full EOD pipeline (health + backtest + Hermes + telegram summary)
ops/run_eod_pipeline.sh                  # today
ops/run_eod_pipeline.sh 2026-05-25       # backfill a specific day
```

Snapshots go to `reports/snapshots/<kind>_<date>.txt`; the Hermes report goes to `agent/hermes/reports/<date>.md`.

## Cost-aware backtest standalone

```bash
cd build && ./engine_backtest
cd build && ./engine_backtest /path/to/other.db
```

## Inspecting the DB directly

```bash
DB=data/engine.db

# Tail of signals
sqlite3 -header -column "$DB" \
  "SELECT datetime(ts,'unixepoch') AS utc, symbol, momentum, vol_regime
   FROM signals ORDER BY ts DESC LIMIT 20;"

# Today's signal count
sqlite3 "$DB" "SELECT COUNT(*) FROM signals
               WHERE ts >= strftime('%s','now','start of day');"
```

## Environment

Copy `ops/.env.example` to `ops/.env` and fill in. `ops/.env` is gitignored.

| Variable | Effect |
|---|---|
| `MME_MT5_HOST`, `MME_MT5_PORT` | MT5 bridge endpoint (default `127.0.0.1:7777`). MT5 is the only data source. |
| `TELEGRAM_BOT_TOKEN`, `TELEGRAM_CHAT_ID` | Enables Telegram notifications. No-op if unset. |
| `STALE_LOG_THRESHOLD_S` | health_check WARN if `engine.log` is silent longer than this (default 120) |
| `LONDON_OPEN_UTC`, `NY_CLOSE_UTC` | Reference values for crontab scheduling |

## Logs

| File | Contents |
|---|---|
| `logs/engine.log` (rotating, 10MB × 7) | spdlog output from the running engine |
| `logs/engine.out` | stdout from launches (appended across restarts) |
| `logs/engine.err` | stderr from launches — first place to look on `[FATAL]` start failures |
| `logs/ops.log` | every ops/*.sh action and outcome |
