# Architecture Version 1.0 — Approved Errata

This errata is authoritative for the Phase 2c WAL implementation and resolves
the corresponding ambiguities in `ARCHITECTURE_V1.md`. All other architecture
requirements remain frozen and unchanged.

## WAL frame layout

Frames are one-byte packed and little-endian:

| Field | Type | Size |
|---|---:|---:|
| `payload_length` | `uint32` | 4 |
| `type` | `uint16` | 2 |
| `flags` | `uint16` | 2 |
| `payload` | bytes | `payload_length` |
| `crc32` | `uint32` | 4 |

CRC32 covers `payload_length | type | flags | payload`; the CRC field is
excluded. `payload_length` counts payload bytes only. A framed 128-byte
`FixedEvent` is exactly 140 bytes. The Architecture Version 1.0 statement
“Framed event record | 136 B fixed” is an erratum and must not be used.

## WAL file header

Every WAL segment begins with this 256-byte packed little-endian header:

| Offset | Field |
|---:|---|
| 0 | `magic[8] = "MMEWAL01"` |
| 8 | `uint16 format_version = 1` |
| 10 | `uint16 header_size = 256` |
| 12 | `uint32 byte_order_marker = 0x01020304` |
| 16 | `uint32 flags` |
| 20 | `uint8 run_id[16]` |
| 36 | `uint64 run_seed` |
| 44 | `uint64 session_epoch` |
| 52 | `uint8 effective_limits_hash[32]` |
| 84 | `uint8 params_hash[32]` |
| 116 | `uint8 build_hash[32]` |
| 148 | `char toolchain_version[32]`, UTF-8 and null-padded |
| 180 | `uint8 clock_calibration_record[64]` |
| 244 | `uint8 reserved[8]`, all zero |
| 252 | `uint32 header_crc32` |

`header_crc32` covers bytes 0 through 251.

## Durability semantics

“Every ingress event, verbatim, before processing” means constructing the
complete frame, copying it into pre-faulted mapped WAL storage, publishing the
committed write boundary, and only then releasing the event for downstream
processing. Per-record `fsync`, `FlushFileBuffers`, `msync`, and
`FlushViewOfFile` are prohibited. Configurable grouped flushing occurs outside
the append hot path, periodically, at rotation, and at controlled shutdown.
The configured interval is the documented crash-loss window. The WAL append
benchmark measures pre-faulted append and commit only and excludes durability
flush time.

## Recovery boundary

Preallocated zero-filled space is not WAL data. A deterministic CRC-protected
segment metadata mechanism persists the committed-data boundary. Recovery
scans only within that boundary and stops at the first incomplete prefix,
invalid or out-of-range payload length, incomplete payload or CRC, or CRC
mismatch. It never searches forward for another apparent frame. Recovery
truncates to the last completely validated frame and is idempotent.

## Payload bounds

Payload lengths are bounded by the existing 128-byte fixed event and 1,040-byte
slab-backed snapshot capacities. Lengths above the frozen maximum are rejected
before payload bytes are read.

## Rotation and compression

Segments rotate at 256 MiB of logically committed uncompressed WAL data or one
hour, whichever occurs first. zstd is the sole approved Phase 2c dependency.
Only closed, durably flushed segments are compressed, outside the recorder hot
path. Compression writes to a temporary path, verifies completion and
integrity, atomically renames the result, and retains the source until
verification succeeds. Compressed data is never the primary torn-write
recovery format.

## MSan

MSan runs only when an existing compatible Clang/MSan environment is
available. No compiler is installed or replaced for this phase.
