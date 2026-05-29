#!/usr/bin/env python3
"""Engine health check — read-only.

Usage:
    python3 ops/health_check.py           # exit 0 if all PASS, 1 otherwise
    python3 ops/health_check.py --json    # machine-readable output

Checks:
  1. engine_process  — PID file exists, process alive, named 'engine'
  2. db_present      — engine.db exists and is readable
  3. db_recent_write — newest tick is within (5 × poll_interval) seconds
  4. log_recent      — engine.log modified within STALE_LOG_THRESHOLD_S
  5. api_key_present — config/api_key.txt non-empty OR TWELVEDATA_API_KEY set
  6. price_not_frozen — latest 5 ticks per symbol are not byte-identical
                        (mirrors validation/isStale; flags feed problems)

Exit 0 = all green. Exit 1 = at least one FAIL. Exit 2 = at least one WARN
(non-fatal but worth a look). PASS/WARN/FAIL semantics are surfaced in the
output for both humans and scripts.
"""

from __future__ import annotations

import argparse
import json
import os
import sqlite3
import subprocess
import sys
import time
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
RUN_DIR      = PROJECT_ROOT / "run"
LOG_DIR      = PROJECT_ROOT / "logs"
PID_FILE     = RUN_DIR / "engine.pid"
ENGINE_LOG   = LOG_DIR / "engine.log"
DB_PATH      = PROJECT_ROOT / "data" / "engine.db"
API_KEY_FILE = PROJECT_ROOT / "config" / "api_key.txt"

POLL_INTERVAL_S       = 30   # engine kSleepSeconds
DB_STALENESS_FACTOR   = 5    # tolerate up to 5 missed cycles before failing
LOG_STALENESS_S       = int(os.environ.get("STALE_LOG_THRESHOLD_S", "120"))
FROZEN_LOOKBACK_TICKS = 5    # if last N ticks are identical, flag the feed


class Status:
    PASS = "PASS"
    WARN = "WARN"
    FAIL = "FAIL"


def check(name: str, status: str, detail: str = "") -> dict:
    return {"check": name, "status": status, "detail": detail}


def _process_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except (ProcessLookupError, PermissionError):
        return False
    return True


def check_engine_process() -> dict:
    if not PID_FILE.exists():
        return check("engine_process", Status.FAIL, "no PID file at " + str(PID_FILE))
    try:
        pid = int(PID_FILE.read_text().strip())
    except (ValueError, OSError) as e:
        return check("engine_process", Status.FAIL, f"unreadable PID file: {e}")
    if not _process_alive(pid):
        return check("engine_process", Status.FAIL, f"pid {pid} not alive (stale PID file)")
    # Confirm comm == 'engine' to defend against PID recycling.
    try:
        comm = subprocess.check_output(
            ["ps", "-p", str(pid), "-o", "comm="], text=True
        ).strip()
    except subprocess.CalledProcessError:
        return check("engine_process", Status.FAIL, f"ps could not find pid {pid}")
    if comm != "engine":
        return check(
            "engine_process",
            Status.FAIL,
            f"pid {pid} is '{comm}', not 'engine' (recycled PID?)",
        )
    return check("engine_process", Status.PASS, f"pid={pid}")


def check_db_present() -> dict:
    if not DB_PATH.exists():
        return check("db_present", Status.FAIL, f"missing {DB_PATH}")
    if not os.access(DB_PATH, os.R_OK):
        return check("db_present", Status.FAIL, f"not readable: {DB_PATH}")
    return check("db_present", Status.PASS, f"{DB_PATH.stat().st_size} bytes")


def check_db_recent_write() -> dict:
    if not DB_PATH.exists():
        return check("db_recent_write", Status.FAIL, "DB missing")
    try:
        conn = sqlite3.connect(f"file:{DB_PATH}?mode=ro", uri=True)
        row = conn.execute("SELECT MAX(ts) FROM ticks").fetchone()
        conn.close()
    except sqlite3.Error as e:
        return check("db_recent_write", Status.FAIL, f"sqlite error: {e}")
    if not row or row[0] is None:
        return check("db_recent_write", Status.WARN, "no ticks in DB yet")
    age = int(time.time() - row[0])
    threshold = POLL_INTERVAL_S * DB_STALENESS_FACTOR
    if age > threshold:
        return check(
            "db_recent_write",
            Status.WARN,
            f"newest tick is {age}s old (> {threshold}s threshold)",
        )
    return check("db_recent_write", Status.PASS, f"newest tick {age}s old")


def check_log_recent() -> dict:
    if not ENGINE_LOG.exists():
        return check("log_recent", Status.FAIL, "engine.log missing")
    age = int(time.time() - ENGINE_LOG.stat().st_mtime)
    if age > LOG_STALENESS_S:
        return check(
            "log_recent",
            Status.WARN,
            f"log untouched for {age}s (> {LOG_STALENESS_S}s)",
        )
    return check("log_recent", Status.PASS, f"last write {age}s ago")


def check_api_key_present() -> dict:
    if os.environ.get("TWELVEDATA_API_KEY"):
        return check("api_key_present", Status.PASS, "from env")
    if API_KEY_FILE.exists() and API_KEY_FILE.stat().st_size > 0:
        return check("api_key_present", Status.PASS, f"from {API_KEY_FILE}")
    return check(
        "api_key_present",
        Status.FAIL,
        "no TWELVEDATA_API_KEY env and config/api_key.txt missing/empty",
    )


def check_price_not_frozen() -> dict:
    if not DB_PATH.exists():
        return check("price_not_frozen", Status.WARN, "DB missing")
    try:
        conn = sqlite3.connect(f"file:{DB_PATH}?mode=ro", uri=True)
        symbols = [r[0] for r in conn.execute(
            "SELECT DISTINCT symbol FROM ticks").fetchall()]
        frozen = []
        for sym in symbols:
            prices = [r[0] for r in conn.execute(
                "SELECT price FROM ticks WHERE symbol=? ORDER BY ts DESC LIMIT ?",
                (sym, FROZEN_LOOKBACK_TICKS),
            ).fetchall()]
            if len(prices) >= FROZEN_LOOKBACK_TICKS and len(set(prices)) == 1:
                frozen.append(f"{sym}={prices[0]}")
        conn.close()
    except sqlite3.Error as e:
        return check("price_not_frozen", Status.WARN, f"sqlite error: {e}")
    if frozen:
        # WARN, not FAIL — markets ARE closed sometimes. Caller decides.
        return check("price_not_frozen", Status.WARN,
                     "feed appears frozen: " + ", ".join(frozen))
    return check("price_not_frozen", Status.PASS,
                 f"{len(symbols)} symbol(s), no frozen tail")


CHECKS = [
    check_engine_process,
    check_db_present,
    check_db_recent_write,
    check_log_recent,
    check_api_key_present,
    check_price_not_frozen,
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", action="store_true", help="machine output")
    parser.add_argument("--skip-process", action="store_true",
                        help="skip engine_process check (useful at EOD report time "
                             "when engine has been intentionally stopped)")
    args = parser.parse_args()

    results = []
    for fn in CHECKS:
        if args.skip_process and fn is check_engine_process:
            continue
        try:
            results.append(fn())
        except Exception as e:  # don't let one broken check kill the whole report
            results.append(check(fn.__name__, Status.WARN, f"check raised: {e}"))

    n_fail = sum(1 for r in results if r["status"] == Status.FAIL)
    n_warn = sum(1 for r in results if r["status"] == Status.WARN)
    overall = Status.FAIL if n_fail else (Status.WARN if n_warn else Status.PASS)

    if args.json:
        print(json.dumps({"overall": overall, "checks": results}, indent=2))
    else:
        print(f"Overall: {overall}  ({n_fail} fail, {n_warn} warn, "
              f"{len(results) - n_fail - n_warn} pass)")
        print()
        for r in results:
            print(f"  [{r['status']:4}] {r['check']:18}  {r['detail']}")

    if n_fail:
        return 1
    if n_warn:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
