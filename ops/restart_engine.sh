#!/usr/bin/env bash
# Stop (if running) then start. Used by recovery flows and cron-driven restarts.

set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

"$OPS_DIR/stop_engine.sh"
"$OPS_DIR/start_engine.sh"
