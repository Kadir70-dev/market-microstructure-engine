#!/usr/bin/env bash
# Print engine status — exits 0 if running, 1 if not. Cron- and human-friendly.

set -u
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

pid="$(read_pid)"

if is_engine_alive "$pid"; then
    # ps gives us start time and CPU/mem at a glance.
    info=$(ps -p "$pid" -o pid=,etime=,%cpu=,%mem= 2>/dev/null | sed 's/^ *//')
    echo "engine: RUNNING"
    echo "  $info  (pid etime %cpu %mem)"
    echo "  pid_file=$PID_FILE"
    echo "  log=$ENGINE_LOG"
    [[ -f "$ENGINE_LOG" ]] && echo "  last_log_line: $(tail -n 1 "$ENGINE_LOG")"
    exit 0
fi

echo "engine: NOT RUNNING"
if [[ -f "$PID_FILE" ]]; then
    echo "  stale pid file present at $PID_FILE (pid=$pid not alive)"
fi
[[ -f "$ENGINE_LOG" ]] && echo "  last_log_line: $(tail -n 1 "$ENGINE_LOG")"
exit 1
