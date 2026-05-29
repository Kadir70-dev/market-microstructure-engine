#!/usr/bin/env bash
# Cron entry-point: run health_check.py and notify on FAIL.
# WARN status (e.g., frozen feed during a quiet hour) is logged but not paged.

set -u
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

output=$(python3 "$OPS_DIR/health_check.py" 2>&1)
rc=$?

case "$rc" in
    0) ;;  # all PASS, silent
    1)
        ops_log "Health probe: FAIL"
        ops_log "$output"
        notify "🚨 health FAIL:%0A${output}"
        ;;
    2)
        # WARN — log only, no page. Repeat WARNs become noise.
        ops_log "Health probe: WARN (no notify)"
        ops_log "$output"
        ;;
    *)
        ops_log "Health probe: unexpected exit $rc"
        ops_log "$output"
        notify "⚠️ health probe crashed (exit $rc)"
        ;;
esac

exit "$rc"
