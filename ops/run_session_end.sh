#!/usr/bin/env bash
# Cron entry-point: stop engine, generate Hermes daily report, summarize.

set -u
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

ops_log "Session end triggered"
"$OPS_DIR/stop_engine.sh"

today_utc="$(date -u +%Y-%m-%d)"

ops_log "Generating Hermes report for $today_utc"
if cd "$PROJECT_ROOT" && python3 -m agent.hermes.daily_report --date "$today_utc" >> "$OPS_LOG" 2>&1; then
    report="$PROJECT_ROOT/agent/hermes/reports/${today_utc}.md"
    ops_log "Hermes report ready: $report"

    if [[ -x "$OPS_DIR/telegram_notify.sh" ]]; then
        # First 25 lines are the headline metrics + problems found.
        summary=$(head -n 25 "$report" 2>/dev/null || echo "report empty")
        notify "📊 EOD report ${today_utc}:%0A${summary}"
    fi
else
    ops_log "Hermes report FAILED for $today_utc"
    notify "🚨 Hermes report FAILED for $today_utc"
fi
