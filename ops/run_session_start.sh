#!/usr/bin/env bash
# Cron entry-point: start engine for the trading session.
# Idempotent — safe to invoke multiple times (start_engine.sh handles that).

set -u
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

dow=$(date -u +%u)  # 1=Mon, 7=Sun. FX is closed Sat (6) and Sun until 22:00.
if [[ "$dow" == "6" ]]; then
    ops_log "Skip start: Saturday (FX market closed)"
    exit 0
fi

ops_log "Session start triggered"
"$OPS_DIR/start_engine.sh"
