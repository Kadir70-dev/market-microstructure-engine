#!/usr/bin/env bash
# Start the engine in the background. Idempotent: refuses to start a
# second instance if one is already running.

set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

if [[ ! -x "$ENGINE_BIN" ]]; then
    ops_log "FATAL: engine binary missing at $ENGINE_BIN — build first"
    exit 2
fi

existing="$(read_pid)"
if is_engine_alive "$existing"; then
    ops_log "Engine already running (pid=$existing). No-op."
    exit 0
fi

# Stale PID file from a previous crash — clean up before starting.
if [[ -f "$PID_FILE" ]]; then
    ops_log "Removing stale PID file (pid=$existing not alive)"
    rm -f "$PID_FILE"
fi

# Engine writes ../data/engine.db relative to CWD, so we must cd to build/.
cd "$PROJECT_ROOT/build"

# MT5-only via file-export: the engine reads quotes from the CSV written by the
# MQL5 EA (MME_QUOTES_CSV, default ../data/mme_quotes.csv). No API key. The EA
# (under Wine) must be running and writing that file; if it isn't, the engine
# logs a warning and retries each cycle — it does not exit.

# Append (don't truncate) so we can see history across restarts.
nohup ./engine >> "$ENGINE_OUT" 2>> "$ENGINE_ERR" &
pid=$!

echo "$pid" > "$PID_FILE"

# Verify it actually stayed up. Engine could die immediately on bad config,
# missing DB dir, etc. Catch that here instead of leaving a phantom PID file.
sleep 2
if ! is_engine_alive "$pid"; then
    ops_log "FATAL: engine died within 2s of start. See $ENGINE_ERR"
    rm -f "$PID_FILE"
    notify "🚨 engine FAILED to start — see $ENGINE_ERR"
    exit 4
fi

ops_log "Engine started (pid=$pid)"
notify "✅ engine started (pid=$pid)"
