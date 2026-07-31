#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <wal-directory>" >&2
  exit 2
fi

python3 - "$1" <<'PY'
import binascii, glob, json, os, struct, sys

root = sys.argv[1]
failures = 0
records_total = 0
bytes_total = 0

def boundary_for(path):
    with open(path + '.meta', 'rb') as f:
        data = f.read(64)
    if len(data) != 64:
        raise ValueError('metadata_size')
    valid = []
    for offset in (0, 32):
        magic, generation, committed, checksum, reserved = struct.unpack_from('<8sQQI4s', data, offset)
        calculated = binascii.crc32(data[offset:offset + 24]) & 0xffffffff
        if magic == b'MMEBND01' and checksum == calculated:
            valid.append((generation, committed))
    if not valid:
        raise ValueError('metadata_crc')
    return max(valid)[1]

for path in sorted(glob.glob(os.path.join(root, '*.wal'))):
    result = {'file': os.path.basename(path), 'status': 'PASS', 'records': 0}
    try:
        boundary = boundary_for(path)
        size = os.path.getsize(path)
        if size < 256 or boundary > size - 256:
            raise ValueError('boundary_out_of_file')
        with open(path, 'rb') as f:
            header = f.read(256)
            if len(header) != 256 or header[:8] != b'MMEWAL01':
                raise ValueError('header_magic')
            if struct.unpack_from('<H', header, 8)[0] != 1 or struct.unpack_from('<H', header, 10)[0] != 256:
                raise ValueError('header_version')
            if struct.unpack_from('<I', header, 12)[0] != 0x01020304:
                raise ValueError('byte_order')
            if (binascii.crc32(header[:252]) & 0xffffffff) != struct.unpack_from('<I', header, 252)[0]:
                raise ValueError('header_crc')
            position = 0
            while position < boundary:
                prefix = f.read(8)
                if len(prefix) != 8:
                    raise ValueError('torn_prefix')
                length, record_type, flags = struct.unpack('<IHH', prefix)
                if length < 1 or length > 1040:
                    raise ValueError('payload_length')
                if position + 12 + length > boundary:
                    raise ValueError('frame_beyond_boundary')
                payload = f.read(length)
                crc_bytes = f.read(4)
                if len(payload) != length or len(crc_bytes) != 4:
                    raise ValueError('torn_payload_or_crc')
                expected = struct.unpack('<I', crc_bytes)[0]
                if (binascii.crc32(prefix + payload) & 0xffffffff) != expected:
                    raise ValueError('frame_crc')
                position += 12 + length
                result['records'] += 1
            result['committed_bytes'] = boundary
            result['preallocated_bytes_ignored'] = size - 256 - boundary
        records_total += result['records']
        bytes_total += boundary
    except Exception as error:
        failures += 1
        result['status'] = 'FAIL'
        result['reason'] = str(error)
    print(json.dumps(result, sort_keys=True))

temporary = sorted(glob.glob(os.path.join(root, '*.zst.tmp')))
if temporary:
    failures += len(temporary)
    for path in temporary:
        print(json.dumps({'file': os.path.basename(path), 'status': 'FAIL', 'reason': 'incomplete_compression'}))
print(json.dumps({'summary': 'PASS' if failures == 0 else 'FAIL', 'wal_files': len(glob.glob(os.path.join(root, '*.wal'))), 'records': records_total, 'committed_bytes': bytes_total, 'failures': failures}, sort_keys=True))
sys.exit(0 if failures == 0 and glob.glob(os.path.join(root, '*.wal')) else 1)
PY
