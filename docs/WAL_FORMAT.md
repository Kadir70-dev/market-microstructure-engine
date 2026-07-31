# WAL Format — Version 1

The authoritative binary format is Architecture Version 1.0 together with
`ARCHITECTURE_V1_ERRATA.md`. All integers are one-byte packed little-endian.

## Segment files

An uncompressed segment is `<20-digit-index>.wal`; its committed-boundary
sidecar is `<segment>.meta`. The WAL begins with the frozen 256-byte
`WalFileHeader`, followed immediately by frames. Logical data capacity defaults
to 256 MiB, excluding the header. Rotation occurs at that boundary or one hour,
whichever comes first.

The header layout and every offset are asserted in `persist/framing.hpp`.
Header CRC32 covers bytes 0–251.

## Frames

`payload_length:uint32 | type:uint16 | flags:uint16 | payload | crc32:uint32`

CRC32 covers the eight-byte prefix and payload. Payload length counts only the
payload and is restricted to 1–1,040 bytes. A verbatim 128-byte `FixedEvent`
occupies 140 framed bytes.

## Commit and durability

Append constructs a complete bounded frame on the stack, copies it into the
preallocated and prefaulted mapping, then publishes the new boundary to an
alternating two-slot metadata mapping. Each 32-byte slot contains magic,
generation, committed byte count, and CRC32. The highest valid generation is
authoritative. This logical commit occurs before an event is returned for
downstream processing.

Mapped data is durably flushed before metadata during configured grouped
flushes, rotation, and controlled shutdown. The default periodic flush interval
is 100 ms, so up to 100 ms of logically committed data may be absent after an
unclean power or kernel failure. Process failure without loss of the operating
system page cache normally has a smaller window. The append benchmark excludes
flush time.

Linux uses `ftruncate` and `mmap(MAP_SHARED | MAP_POPULATE)`. Windows uses
`SetEndOfFile`, `CreateFileMapping`, `MapViewOfFile`, and explicit page touching
because Windows has no `MAP_POPULATE` equivalent.

## Recovery

Recovery selects the highest-generation CRC-valid metadata slot and scans only
up to that committed boundary. It stops at the first incomplete prefix,
out-of-range length, incomplete payload/CRC, or CRC mismatch and never searches
forward. The WAL and metadata boundary are truncated to the last valid frame.
Repeating recovery is idempotent. Zero-filled preallocated space is never data.

## Compression

Only closed and durably flushed segments are compressed, using zstd level 3 on
a background worker. Output is written to `.zst.tmp`, decompressed and checked
against source size and CRC32, then atomically renamed to `.zst`. The source WAL
is retained. Compressed files are never used for torn-write recovery.
