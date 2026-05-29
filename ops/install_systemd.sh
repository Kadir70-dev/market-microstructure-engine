#!/usr/bin/env bash
# Install the 24/5 data-collection stack as systemd system units:
#   mme-engine.service     - the read-only collector (Restart=always, boot-start)
#   mme-alert.service      - Telegram alert on engine failure (OnFailure)
#   mme-health.{service,timer}  - read-only health probe every 10 min
#   mme-cleanup.{service,timer} - weekly journald vacuum + WAL checkpoint + log prune
#
# Run with sudo:  sudo ops/install_systemd.sh
#
# Idempotent. Does NOT install the cron lifecycle — use systemd OR cron, not both.

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Run as root: sudo $0" >&2
    exit 1
fi

OPS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$OPS_DIR/.." && pwd)"
UNIT_SRC="$OPS_DIR/systemd"
UNIT_DST="/etc/systemd/system"

# --- Preflight checks ----------------------------------------------------
echo "==> Preflight"
[[ -x "$PROJECT_ROOT/build/engine" ]] || { echo "FATAL: build/engine missing — run 'cmake --build build' first"; exit 2; }
# MT5-only: the data source is the MT5 bridge, not an API key. Warn (don't fail)
# if it's unreachable now — the engine retries each cycle, and the bridge may be
# started after this installer.
host="${MME_MT5_HOST:-127.0.0.1}"; port="${MME_MT5_PORT:-7777}"
if ! python3 -c "import socket;socket.create_connection(('$host',$port),timeout=2).close()" 2>/dev/null; then
    echo "WARN: MT5 bridge not reachable at $host:$port — start it (under Wine) before/after install."
fi
if ! grep -qE '^TELEGRAM_BOT_TOKEN=.+' "$PROJECT_ROOT/ops/.env" 2>/dev/null; then
    echo "WARN: TELEGRAM_BOT_TOKEN not set in ops/.env — death alerts will be silent."
    echo "      Fill ops/.env (copy from ops/.env.example) to enable Telegram paging."
fi
echo "    build/engine: OK"

# --- Install units -------------------------------------------------------
echo "==> Installing units to $UNIT_DST"
install -m 0644 "$UNIT_SRC"/mme-*.service "$UNIT_DST"/
install -m 0644 "$UNIT_SRC"/mme-*.timer   "$UNIT_DST"/
chmod +x "$PROJECT_ROOT/ops/mme_cleanup.sh" "$PROJECT_ROOT/ops/run_health_probe.sh" \
         "$PROJECT_ROOT/ops/telegram_notify.sh" 2>/dev/null || true

systemctl daemon-reload

# --- Verify syntax before enabling --------------------------------------
echo "==> Verifying units"
systemd-analyze verify "$UNIT_DST/mme-engine.service" || true

# --- Enable + start ------------------------------------------------------
echo "==> Enabling engine + timers (start now + on boot)"
systemctl enable --now mme-engine.service
systemctl enable --now mme-health.timer
systemctl enable --now mme-cleanup.timer

echo "==> Done. Status:"
systemctl --no-pager status mme-engine.service | head -n 8 || true
echo
echo "Watch logs:   journalctl -u mme-engine -f"
echo "Timers:       systemctl list-timers 'mme-*'"
echo "Stop:         sudo systemctl disable --now mme-engine mme-health.timer mme-cleanup.timer"
