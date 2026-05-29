#!/usr/bin/env bash
# End-of-day analysis pipeline. Idempotent — runs the cold-path agents
# against whatever is in engine.db right now. Safe to re-run manually for
# backfill. Does NOT touch the engine process; assumes it's already stopped.
#
# Pipeline:
#   1. (optional) health check skipping the engine-alive check
#   2. cost-aware backtest -> stdout snapshot in reports/
#   3. Hermes daily report -> agent/hermes/reports/<date>.md
#   4. Telegram summary    -> first 25 lines of Hermes report
#
# No trading actions. No broker calls. Read-only against engine.db.

set -u
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

date_utc="${1:-$(date -u +%Y-%m-%d)}"
snapshot_dir="$PROJECT_ROOT/reports/snapshots"
mkdir -p "$snapshot_dir"

ops_log "EOD pipeline for $date_utc"

# 1. Health summary (process check skipped — engine is stopped by design).
python3 "$OPS_DIR/health_check.py" --skip-process > "$snapshot_dir/health_${date_utc}.txt" 2>&1 || true

# 2. Cost-aware backtest. Captures the verdict line we wrote into main.cpp.
backtest_out="$snapshot_dir/backtest_${date_utc}.txt"
if (cd "$PROJECT_ROOT/build" && ./engine_backtest) > "$backtest_out" 2>&1; then
    ops_log "Backtest snapshot: $backtest_out"
else
    ops_log "Backtest snapshot failed (engine.db too small?)"
fi

# 3. Hermes daily report.
hermes_report="$PROJECT_ROOT/agent/hermes/reports/${date_utc}.md"
if (cd "$PROJECT_ROOT" && python3 -m agent.hermes.daily_report --date "$date_utc"); then
    ops_log "Hermes report: $hermes_report"
else
    ops_log "Hermes report FAILED"
    notify "🚨 Hermes report FAILED for $date_utc"
    exit 3
fi

# 4. Telegram summary.
if [[ -f "$hermes_report" ]]; then
    summary=$(head -n 25 "$hermes_report")
    notify "📊 EOD $date_utc%0A${summary}"
fi

ops_log "EOD pipeline complete"
