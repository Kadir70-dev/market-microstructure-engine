#!/usr/bin/env bash
# Shared helpers sourced by every ops/*.sh script.
# Keep this file small and dependency-free.

set -u

# Resolve the repo root from this file's location, NOT from CWD,
# so the scripts work regardless of where the operator invokes them.
OPS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$OPS_DIR/.." && pwd)"

ENGINE_BIN="$PROJECT_ROOT/build/engine"
RUN_DIR="$PROJECT_ROOT/run"
PID_FILE="$RUN_DIR/engine.pid"
LOG_DIR="$PROJECT_ROOT/logs"
ENGINE_OUT="$LOG_DIR/engine.out"     # stdout (spdlog console sink)
ENGINE_ERR="$LOG_DIR/engine.err"     # stderr only
ENGINE_LOG="$LOG_DIR/engine.log"     # rotating spdlog file sink
OPS_LOG="$LOG_DIR/ops.log"
DB_PATH="$PROJECT_ROOT/data/engine.db"

mkdir -p "$RUN_DIR" "$LOG_DIR"

# Load .env if present. Never fail if missing.
ENV_FILE="$PROJECT_ROOT/ops/.env"
if [[ -f "$ENV_FILE" ]]; then
    # shellcheck disable=SC1090
    set -a; . "$ENV_FILE"; set +a
fi

ops_log() {
    local msg="$*"
    printf '%s [ops] %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')" "$msg" \
        | tee -a "$OPS_LOG" >&2
}

# Read PID from file. Echoes the PID or empty string. Never errors.
read_pid() {
    [[ -f "$PID_FILE" ]] || { echo ""; return; }
    local pid
    pid=$(cat "$PID_FILE" 2>/dev/null | tr -dc '0-9')
    echo "$pid"
}

# Is the recorded PID a running process AND named 'engine'? Belt-and-braces:
# a stale PID file can point at a recycled, unrelated PID.
is_engine_alive() {
    local pid="$1"
    [[ -n "$pid" ]] || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    local cmd
    cmd=$(ps -p "$pid" -o comm= 2>/dev/null | tr -d ' ')
    [[ "$cmd" == "engine" ]]
}

# Optional telegram notification. No-op if creds are unset.
notify() {
    local msg="$*"
    if [[ -x "$OPS_DIR/telegram_notify.sh" ]]; then
        "$OPS_DIR/telegram_notify.sh" "$msg" || true
    fi
}
