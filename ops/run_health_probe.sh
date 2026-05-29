#!/usr/bin/env bash
# Entry-point (cron OR systemd timer): run health_check.py and notify on FAIL.
# WARN status (e.g., frozen feed during a quiet hour) is logged but not paged.
#
# Set MME_SKIP_PROCESS=1 to skip the PID-file process check. Under systemd the
# engine's liveness is owned by the unit (Restart=always + OnFailure alert), and
# there is no run/engine.pid — so the process check would false-FAIL there.

set -u
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

skip_arg=""
[[ "${MME_SKIP_PROCESS:-0}" == "1" ]] && skip_arg="--skip-process"

output=$(python3 "$OPS_DIR/health_check.py" $skip_arg 2>&1)
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
