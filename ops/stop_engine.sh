#!/usr/bin/env bash
# Stop the engine gracefully. Sends SIGTERM (handler in main.cpp), waits up
# to 15s, then escalates to SIGKILL only as a last resort.

set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

pid="$(read_pid)"

if ! is_engine_alive "$pid"; then
    ops_log "Engine not running (no live pid). Cleaning state."
    rm -f "$PID_FILE"
    exit 0
fi

ops_log "Stopping engine (pid=$pid) via SIGTERM"
kill -TERM "$pid" 2>/dev/null || true

# Wait up to 15s for clean exit. The engine's interruptibleSleep means
# shutdown latency is ~1s in normal operation; 15s is generous for the
# case where it's mid-HTTP-fetch.
for _ in $(seq 1 15); do
    if ! is_engine_alive "$pid"; then
        break
    fi
    sleep 1
done

if is_engine_alive "$pid"; then
    ops_log "Engine did not exit in 15s — escalating to SIGKILL"
    kill -KILL "$pid" 2>/dev/null || true
    sleep 1
    if is_engine_alive "$pid"; then
        ops_log "FATAL: engine still alive after SIGKILL (pid=$pid). Investigate."
        notify "🚨 engine failed to stop (pid=$pid)"
        exit 5
    fi
    notify "⚠️ engine force-killed (pid=$pid)"
else
    notify "🛑 engine stopped cleanly (pid=$pid)"
fi

rm -f "$PID_FILE"
ops_log "Engine stopped"
