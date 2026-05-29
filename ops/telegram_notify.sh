#!/usr/bin/env bash
# Send a Telegram message via Bot API. Infrastructure notifications only:
# engine started/stopped/crashed, DB issues, stale feed warnings, EOD report.
# NEVER used for trading actions — there are no trading actions in this
# system right now, and there must not be one routed through this notifier.
#
# Gracefully no-ops if creds are unset, so scripts can always call it without
# branching.

set -u
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

msg="${1:-}"
if [[ -z "$msg" ]]; then
    echo "usage: telegram_notify.sh <message>" >&2
    exit 1
fi

if [[ -z "${TELEGRAM_BOT_TOKEN:-}" ]] || [[ -z "${TELEGRAM_CHAT_ID:-}" ]]; then
    # Silent no-op: not configured. Log so the operator can see if expected.
    ops_log "telegram_notify: skipped (creds unset)"
    exit 0
fi

# Telegram caps messages at 4096 chars. Truncate defensively.
if (( ${#msg} > 4000 )); then
    msg="${msg:0:4000}…(truncated)"
fi

# --fail returns non-zero on HTTP 4xx/5xx so we can detect API errors.
# --max-time keeps a hung Telegram from blocking the engine lifecycle.
if ! curl -sS --fail --max-time 10 \
        -X POST "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/sendMessage" \
        --data-urlencode "chat_id=${TELEGRAM_CHAT_ID}" \
        --data-urlencode "text=${msg}" \
        --data-urlencode "disable_web_page_preview=true" \
        > /dev/null 2>>"$OPS_LOG"; then
    ops_log "telegram_notify: send failed"
    exit 2
fi
