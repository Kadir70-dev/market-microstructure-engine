# Recovery — when things break

Ordered by frequency of cause. Stop at the first symptom that matches.

## 1. Engine won't start

```bash
ops/start_engine.sh                  # check the exit code
tail -n 50 logs/engine.err           # crashes before spdlog init land here
tail -n 50 logs/engine.log           # crashes after init land here
```

| Exit | Meaning | Fix |
|---|---|---|
| 2 | Binary missing | `cmake --build build` |
| 4 | Engine died in <2s | Read `logs/engine.err`. Typical causes: missing `data/` dir, SQLite permission issue. (A down MT5 bridge does NOT kill the engine — it warns and retries.) |

## 2. Engine running, but no new data

```bash
ops/health_check.py
sqlite3 data/engine.db "SELECT MAX(ts) FROM ticks;"   # compare to date +%s
tail -n 20 logs/engine.log
```

Diagnoses:
- **Frozen feed (weekend/holiday):** `health_check.py` flags `price_not_frozen: WARN`. Not a fault — the market is closed. Engine will resume signal variation when prices update.
- **MT5 bridge down/unreachable:** `logs/engine.log` shows `MT5 bridge ping failed`. Start the bridge (under Wine) and check `ops/probe_mt5_bridge.sh`; the engine auto-recovers on the next cycle.
- **Bridge reports `demo_only=false`:** the engine refuses to start (tripwire). Ensure MT5 is logged into a DEMO account; never run live.
- **No quotes for a symbol:** the broker may not offer it under that name — check the MT5 symbol mapping (`EURUSD`/`XAUUSD`/`XTIUSD`) and the bridge log.

## 3. Engine alive but logs are silent

`health_check.py` reports `log_recent: WARN`.

```bash
ops/status_engine.sh                 # confirm pid + comm=engine
strace -p $(cat run/engine.pid)      # see what syscall it's blocked on
                                      # (needs ptrace_scope=0 or sudo)
```

If blocked on an MT5 bridge socket read, the engine is fine — just slow (the socket has a timeout). If it's truly hung, `ops/restart_engine.sh`.

## 4. Stale PID file (engine listed as running but isn't)

```bash
ops/status_engine.sh
# "engine: NOT RUNNING   stale pid file present"
rm run/engine.pid
ops/start_engine.sh
```

`start_engine.sh` and `stop_engine.sh` both clean up stale PID files automatically — this only needs manual intervention if something else (e.g., an SSH disconnect during start) interrupted the script.

## 5. DB corruption (very rare)

SQLite is durable across crashes via WAL. Corruption is essentially limited to: full disk during a write, or storage hardware failure.

```bash
sqlite3 data/engine.db "PRAGMA integrity_check;"
```

If not `ok`:

```bash
# Preserve current state
mv data/engine.db data/engine.db.broken.$(date +%s)

# Recover what we can
sqlite3 data/engine.db.broken.$(date +%s) ".recover" | sqlite3 data/engine.db
```

If recovery loses ticks, the engine recreates the schema on next start and resumes fresh. Hermes reports for past dates will be incomplete; that's the price of corruption.

## 6. Kill switch (planned, not in this phase)

There is no kill switch yet because **there are no orders to kill**. When MT5 demo execution lands (Phase 3 of the roadmap), the planned mechanism is:

```bash
touch run/engine.halt   # engine refuses new orders next cycle
```

For now, "kill switch" is just `ops/stop_engine.sh` — there is nothing trading.

## 7. Reports missing for a previous day

The EOD pipeline is idempotent and reads only what's already in SQLite. Re-run for any past date:

```bash
ops/run_eod_pipeline.sh 2026-05-25
```

This regenerates `agent/hermes/reports/2026-05-25.md` and `reports/snapshots/*_2026-05-25.txt` without touching the engine.
