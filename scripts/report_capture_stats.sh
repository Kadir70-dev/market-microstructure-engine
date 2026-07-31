#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 <wal-directory> [capture-log]" >&2
  exit 2
fi

python3 - "$1" "${2:-}" <<'PY'
import binascii, glob, json, os, re, struct, sys

root, log_path = sys.argv[1], sys.argv[2]
symbols = {0: 'EURUSD', 1: 'GBPUSD', 2: 'USDJPY', 3: 'XAUUSD', 4: 'XTIUSD'}
counts = {key: 0 for key in symbols}
first_ns = last_ns = None
heartbeat_sources = set()

def boundary(path):
    data = open(path + '.meta', 'rb').read(64)
    slots = []
    for off in (0, 32):
        magic, generation, committed, checksum, _ = struct.unpack_from('<8sQQI4s', data, off)
        if magic == b'MMEBND01' and checksum == (binascii.crc32(data[off:off+24]) & 0xffffffff):
            slots.append((generation, committed))
    return max(slots)[1]

for path in sorted(glob.glob(os.path.join(root, '*.wal'))):
    limit = boundary(path)
    with open(path, 'rb') as f:
        f.seek(256); position = 0
        while position < limit:
            prefix = f.read(8); length, _, _ = struct.unpack('<IHH', prefix)
            payload = f.read(length); f.read(4); position += 12 + length
            if length != 128: continue
            local_ns = struct.unpack_from('<Q', payload, 24)[0]
            symbol_id = struct.unpack_from('<I', payload, 44)[0]
            event_type = struct.unpack_from('<H', payload, 50)[0]
            first_ns = local_ns if first_ns is None else min(first_ns, local_ns)
            last_ns = local_ns if last_ns is None else max(last_ns, local_ns)
            if event_type == 11:
                heartbeat_sources.add(payload[56 + 12])
            elif symbol_id in counts:
                counts[symbol_id] += 1

duration = 0 if first_ns is None or last_ns is None else (last_ns - first_ns) / 1e9
rates = {symbols[key]: (counts[key] / duration if duration > 0 else 0.0) for key in symbols}
report = {'duration_seconds': duration, 'event_counts': {symbols[k]: counts[k] for k in symbols}, 'events_per_second': rates, 'heartbeat_book_sources': sorted(heartbeat_sources)}

if log_path:
    text = open(log_path, encoding='utf-8', errors='replace').read()
    handshakes = [line for line in text.splitlines() if line.startswith('HANDSHAKE_ACCEPT')]
    stats = [line for line in text.splitlines() if line.startswith('CAPTURE_STATS')]
    report['handshake_log'] = handshakes[-1] if handshakes else None
    report['latest_runtime_stats'] = stats[-1] if stats else None
    report['recorder_halts'] = len(re.findall(r'^(?:INGRESS_HALT|RECORDER_HALT)', text, re.MULTILINE))

report['all_symbols_at_least_10_per_second'] = duration > 0 and all(rate >= 10.0 for rate in rates.values())
print(json.dumps(report, indent=2, sort_keys=True))
sys.exit(0 if sum(counts.values()) > 0 else 1)
PY
