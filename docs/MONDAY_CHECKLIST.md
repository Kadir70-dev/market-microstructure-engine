# Monday checklist

The first market-open run. Do these in order. None of them place trades — this is data collection plus infra validation.

## Sunday night (≥ 22:00 UTC, before FX reopens)

- [ ] Pull latest code, rebuild
  ```bash
  cmake -S . -B build && cmake --build build
  ```
- [ ] Confirm both binaries exist
  ```bash
  ls -lh build/engine build/engine_backtest
  ```
- [ ] Configure environment
  ```bash
  cp ops/.env.example ops/.env
  # Edit ops/.env, populate at minimum TWELVEDATA_API_KEY
  # (Telegram is optional — leave blank to skip notifications)
  chmod 600 ops/.env
  ```
- [ ] Sanity-run all scripts (no engine, no API calls)
  ```bash
  ops/status_engine.sh        # should report NOT RUNNING (exit 1)
  ops/health_check.py         # process FAIL is expected; everything else PASS/WARN
  bash -n ops/*.sh && echo OK   # syntax check
  ```
- [ ] Decide on schedule. Either:
  - install the example crontab: `crontab ops/crontab.example` (recommended), OR
  - plan to invoke `ops/run_session_start.sh` manually Monday morning

## Monday 06:25 UTC (5 minutes before scheduled start)

- [ ] Confirm clock is UTC or you know the local offset
  ```bash
  date -u
  ```
- [ ] Confirm disk has space (engine.db grows ~50 KB/hour at 30s cycle × 3 symbols)
  ```bash
  df -h .
  ```
- [ ] Pre-warm: one-shot test fetch to confirm the API key works
  ```bash
  KEY=$(cat config/api_key.txt)
  curl -s "https://api.twelvedata.com/price?symbol=EUR/USD&apikey=$KEY"
  # expect: {"price":"1.xxxxx"}
  ```

## Monday 06:30 UTC (London pre-open)

If you installed the crontab, this happens automatically. To do it manually:

- [ ] Start engine
  ```bash
  ops/start_engine.sh
  ```
- [ ] Verify it stayed up
  ```bash
  ops/status_engine.sh
  # expect: engine: RUNNING ... etime small ...
  ```
- [ ] After 2 minutes, confirm new ticks are landing
  ```bash
  sqlite3 data/engine.db \
    "SELECT datetime(ts,'unixepoch'), symbol, price FROM ticks
     ORDER BY ts DESC LIMIT 6;"
  # expect: 6 rows with ts within the last 60s, prices DIFFERENT from
  # Sunday's frozen values (EUR/USD ≠ 1.1607, XAU ≠ 4505.68, USO ≠ 140.96)
  ```

## Monday 08:00 UTC (90 min into the session)

- [ ] Health check should be all PASS / no WARN
  ```bash
  ops/health_check.py
  ```
- [ ] Spot-check signal distribution — confirm Bullish/Bearish exist (not 100% Neutral)
  ```bash
  sqlite3 -header -column data/engine.db \
    "SELECT momentum, COUNT(*) FROM signals
     WHERE ts > strftime('%s','now','-1 hour')
     GROUP BY momentum;"
  ```

## Monday 12:00 UTC (London/NY overlap, peak signal density)

- [ ] Re-check health, especially `price_not_frozen` should be PASS for all symbols
- [ ] Check session classification has flipped off `Closed`
  ```bash
  sqlite3 data/engine.db \
    "SELECT DISTINCT session FROM quality_scores
     WHERE ts > strftime('%s','now','-1 hour');"
  ```

## Monday 21:30 UTC (end of session)

If crontab is installed, EOD pipeline runs automatically. To do it manually:

- [ ] Stop engine
  ```bash
  ops/stop_engine.sh
  ```
- [ ] Run EOD pipeline
  ```bash
  ops/run_eod_pipeline.sh
  ```
- [ ] Read the report
  ```bash
  cat agent/hermes/reports/$(date -u +%Y-%m-%d).md
  cat reports/snapshots/backtest_$(date -u +%Y-%m-%d).txt
  ```

## Acceptance criteria for "Monday was a success"

Not "did the strategy make money." Did the **infrastructure** behave?

- [ ] Engine ran the full window with no crashes (uptime ≥ 14h)
- [ ] `logs/engine.err` is empty or contains only known benign messages
- [ ] DB grew monotonically; no gaps > 5 min
- [ ] `health_check.py` was all-PASS during 08:00–20:00 UTC
- [ ] Total signals ≥ 800 across the day
- [ ] Hermes report generated and reads coherently
- [ ] Backtest snapshot has non-zero `N_dir` for at least one symbol — meaning **directional signals exist** at all (the answer to the question Saturday's data couldn't tell us)

If all green, you've validated the infrastructure. The next decision (signal rework vs MT5 demo) lives in the backtest output, not in the infra itself.
