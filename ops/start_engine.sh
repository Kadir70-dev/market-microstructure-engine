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

# Engine reads ../config/api_key.txt relative to CWD, so we must cd to build/.
cd "$PROJECT_ROOT/build"

# If .env supplied a key and the file is missing, materialize it.
# Env stays the source of truth; the file is a runtime artifact.
if [[ -n "${TWELVEDATA_API_KEY:-}" ]]; then
    printf '%s\n' "$TWELVEDATA_API_KEY" > "$PROJECT_ROOT/config/api_key.txt"
    chmod 600 "$PROJECT_ROOT/config/api_key.txt"
fi

if [[ ! -s "$PROJECT_ROOT/config/api_key.txt" ]]; then
    ops_log "FATAL: no API key (neither TWELVEDATA_API_KEY env nor config/api_key.txt)"
    exit 3
fi

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
