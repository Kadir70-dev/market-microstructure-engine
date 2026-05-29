#!/usr/bin/env bash
# Probe the MT5 sidecar with a single `ping`. Exit 0 if alive and demo_only=true.
# Exit 1 if unreachable, 2 if reachable but reports demo_only=false (tripwire).
#
# Usage:
#   ops/probe_mt5_bridge.sh                       # 127.0.0.1:7777
#   ops/probe_mt5_bridge.sh 192.168.1.50 7777     # remote bridge host

set -euo pipefail

HOST="${1:-${MME_MT5_HOST:-127.0.0.1}}"
PORT="${2:-${MME_MT5_PORT:-7777}}"

reply="$(
  python3 - <<EOF
import json, socket, sys
try:
    s = socket.create_connection(("$HOST", $PORT), timeout=3)
except OSError as e:
    print(json.dumps({"ok": False, "error": str(e)}))
    sys.exit(0)
try:
    s.sendall(b'{"op":"ping"}\n')
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
        if len(buf) > 65536:
            break
    print(buf.decode("utf-8", "replace").strip())
finally:
    s.close()
EOF
)"

echo "$reply"

ok="$(printf '%s' "$reply" | python3 -c 'import json,sys; d=json.loads(sys.stdin.read() or "{}"); print(d.get("ok"))' || echo False)"
demo_only="$(printf '%s' "$reply" | python3 -c 'import json,sys; d=json.loads(sys.stdin.read() or "{}"); print(d.get("demo_only"))' || echo False)"

if [[ "$ok" != "True" ]]; then
    echo "PROBE FAIL: bridge unreachable or returned error"
    exit 1
fi

if [[ "$demo_only" != "True" ]]; then
    echo "PROBE WARN: bridge reachable but demo_only=False"
    exit 2
fi

echo "PROBE OK: bridge alive, demo_only=true"
exit 0
