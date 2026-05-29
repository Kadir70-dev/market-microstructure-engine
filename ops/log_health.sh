#!/usr/bin/env bash
# Summarize log health: rotation status, error rates, recent activity.
# Read-only. Designed for cron + on-demand inspection.

set -u
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

WINDOW_LINES=${1:-2000}  # lines to scan from tail for error counting

if [[ ! -f "$ENGINE_LOG" ]]; then
    echo "engine.log: ABSENT — engine has never logged here"
    exit 1
fi

age_seconds=$(( $(date +%s) - $(stat -c %Y "$ENGINE_LOG") ))
size_bytes=$(stat -c %s "$ENGINE_LOG")
rotations=$(ls -1 "$LOG_DIR"/engine.*.log 2>/dev/null | wc -l)

echo "=== Log health ==="
echo "  path:           $ENGINE_LOG"
echo "  size:           $(numfmt --to=iec --suffix=B "$size_bytes")"
echo "  last_modified:  ${age_seconds}s ago"
echo "  rotations:      $rotations file(s) in $LOG_DIR"

# Counts over the recent tail window
tail_lines=$(tail -n "$WINDOW_LINES" "$ENGINE_LOG")
n_info=$(echo "$tail_lines" | grep -c '\[info\]' || true)
n_warn=$(echo "$tail_lines" | grep -c '\[warning\]' || true)
n_err=$(echo  "$tail_lines" | grep -c '\[error\]'   || true)
echo
echo "=== Last $WINDOW_LINES lines ==="
echo "  info:           $n_info"
echo "  warnings:       $n_warn"
echo "  errors:         $n_err"

if (( n_err > 0 )); then
    echo
    echo "=== Recent errors (up to 5) ==="
    echo "$tail_lines" | grep '\[error\]' | tail -n 5
fi

# Engine should write at least one log line every 30s (the poll cycle).
# Anything older than 120s suggests it's wedged or stopped.
if (( age_seconds > 120 )); then
    echo
    echo "WARN: log is stale (${age_seconds}s). Engine may not be running."
    exit 2
fi

exit 0
