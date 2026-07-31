#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <wal-directory>" >&2
  exit 2
fi

python3 - "$1" <<'PY'
import binascii, glob, json, os, struct, sys

root = sys.argv[1]
last_global = None
last_source = {}
global_gaps = source_gaps = records = 0
current_epoch = None
session_transitions = 0
reported_gaps = 0
max_reported_gaps = 100

def committed(path):
    data = open(path + '.meta', 'rb').read(64)
    slots = []
    for off in (0, 32):
        magic, generation, boundary, checksum, _ = struct.unpack_from('<8sQQI4s', data, off)
        if magic == b'MMEBND01' and checksum == (binascii.crc32(data[off:off+24]) & 0xffffffff):
            slots.append((generation, boundary))
    if not slots:
        raise ValueError('invalid committed-boundary metadata')
    return max(slots)[1]

for path in sorted(glob.glob(os.path.join(root, '*.wal'))):
    boundary = committed(path)
    with open(path, 'rb') as f:
        header = f.read(256)
        epoch = struct.unpack_from('<Q', header, 44)[0]
        if current_epoch is not None and epoch != current_epoch:
            session_transitions += 1
            last_global = None
            last_source.clear()
        current_epoch = epoch
        position = 0
        while position < boundary:
            prefix = f.read(8)
            length, _, _ = struct.unpack('<IHH', prefix)
            payload = f.read(length)
            crc = f.read(4)
            if length != 128 or len(payload) != 128:
                position += 12 + length
                continue
            if (binascii.crc32(prefix + payload) & 0xffffffff) != struct.unpack('<I', crc)[0]:
                raise ValueError('CRC mismatch before sequence analysis')
            seq_global = struct.unpack_from('<Q', payload, 0)[0]
            seq_source = struct.unpack_from('<I', payload, 40)[0]
            source_id = struct.unpack_from('<H', payload, 48)[0]
            if last_global is not None and seq_global != last_global + 1:
                global_gaps += 1
                if reported_gaps < max_reported_gaps:
                    print(json.dumps({'gap': 'global', 'expected': last_global + 1, 'received': seq_global, 'file': os.path.basename(path)}))
                    reported_gaps += 1
            previous = last_source.get(source_id)
            expected = None if previous is None else ((previous + 1) & 0xffffffff)
            if expected is not None and seq_source != expected:
                source_gaps += 1
                if reported_gaps < max_reported_gaps:
                    print(json.dumps({'gap': 'source', 'source_id': source_id, 'expected': expected, 'received': seq_source, 'file': os.path.basename(path)}))
                    reported_gaps += 1
            last_global = seq_global
            last_source[source_id] = seq_source
            records += 1
            position += 12 + length

status = 'PASS' if records > 0 and global_gaps == 0 and source_gaps == 0 else 'FAIL'
print(json.dumps({'summary': status, 'records': records, 'global_gaps': global_gaps, 'source_gaps': source_gaps, 'session_transitions': session_transitions, 'gap_details_emitted': reported_gaps, 'gap_details_limit': max_reported_gaps}, sort_keys=True))
sys.exit(0 if status == 'PASS' else 1)
PY
