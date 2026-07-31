# Phase 2a Transport Decision

## Status

**DECISION PROPOSED — AWAITING HUMAN APPROVAL**

Named pipes remain the default MT5 transport. The fence-free shared-memory
candidate is rejected because publication safety cannot be proven with an MQL5
writer, even though the native benchmark shows a material speed advantage.

Live trading remains disabled. This spike contains no order or execution code.

## Frozen protocol

Protocol version: `1`
Magic: `0x31454D4D` (`MME1` in little-endian byte order)
Byte order: little-endian only
Packing: one byte

| Record | Size | Fields and offsets |
|---|---:|---|
| `RecordPrefix` | 24 B | magic 0:4, version 4:2, type 6:2, session_epoch 8:8, sequence 16:8 |
| `HelloRecord` | 64 B | prefix 0:24, account_expected 24:8, symbol_set_hash 32:32 |
| `HelloAckRecord` | 108 B | prefix 0:24, account_actual 24:8, server_hash 32:32, account_margin_mode 64:4, book_source 68:5, reserved 73:3, symbol_meta_hash 76:32 |
| `SpikeMessage` | 128 B | prefix 0:24, payload_sequence 24:8, payload 32:88, checksum 120:8 |

The C++ layouts are compile-time asserted and byte-order tested. The MQL5
mirror uses the same field order, widths, offsets, and `#pragma pack(push, 1)`.
Handshake tests reject protocol-version, epoch, account, server-hash, and
margin-mode mismatches.

## Environment

- Native Windows 10 Pro, display version 25H2, build 26200.8973
- AMD64, 4 logical processors
- MSVC 19.44.35228, Release `/O2 /fp:strict`
- WSL2 kernel 6.18.33.2, x86_64
- GCC 15.2.0 for Release, ASan, UBSan, and TSan verification
- Wine: not installed in the available WSL distribution
- MT5/MetaEditor: not available to this automated environment

## Methodology

- 50,000 request/acknowledgement round trips per transport
- Fixed 128-byte `SpikeMessage`
- Same-process producer and consumer threads to isolate transport overhead
- Named pipe: Windows `CreateNamedPipe` + blocking `ReadFile`/`WriteFile`
- Shared memory: Windows file mapping with a deliberately fence-free MQL-like
  plain publication flag; no `Interlocked` operation and no C++ atomic fence
- Every message carried monotonic sequence, patterned payload, and FNV-1a
  checksum; every reply was checked for sequence, corruption, and torn reads
- Latency measured with `steady_clock`; CPU is aggregate process CPU divided by
  wall time and can exceed 100% when both threads consume a core

## Results

| Transport | Samples | Throughput | p50 | p95 | p99 | CPU | Corruption |
|---|---:|---:|---:|---:|---:|---:|---:|
| Named pipe | 50,000 | 19,868.4 round trips/s | 40.0 us | 88.5 us | 229.4 us | 67.7% | 0 |
| Fence-free shared memory | 50,000 | 361,399 round trips/s | 1.5 us | 4.4 us | 12.9 us | 180.7% | 0 |

Shared memory was approximately 18.2 times faster by throughput and 17.8 times
lower at p99. The benefit is material, but speed is not the controlling gate.

## Correctness and safety

- Named-pipe framing, full-size reads/writes, checksum, and sequence checks:
  **PASS**.
- Named-pipe publication safety: **PASS**; kernel transport supplies the
  synchronization contract.
- Shared-memory empirical corruption/torn-read counter: **0 in 50,000**.
- Shared-memory publication proof: **FAIL**. MQL5 supplies neither a native
  atomic store nor a memory-fence primitive for the mapped record. `volatile`
  is not a cross-language or cross-process synchronization contract.
- Shared-memory TSan proof: **NOT POSSIBLE** for the actual MQL5 writer. Linux
  sanitizer builds compile a non-Windows boundary stub; they cannot establish
  safety for MQL5/Wine mapped memory.

An empirical zero-corruption run cannot convert an undefined publication
contract into a proven one. The architecture requires proof, so the candidate
is rejected.

## Verification

- Native Windows Release build: PASS
- Focused Windows protocol/layout test: PASS
- Focused Windows named-pipe/shared-memory correctness test: PASS
- Linux Release: 10/10 tests PASS
- Linux ASan: 10/10 tests PASS
- Linux UBSan: 10/10 tests PASS
- Linux TSan: 10/10 tests PASS

## Limitations

- Wine and MT5 were unavailable, so no MQL5 process participated in the
  measurements.
- MQL5 struct sizes are mirrored and hand-checked but were not compiled by
  MetaEditor in this environment.
- Same-process native Windows measurements omit Wine scheduling and MT5
  `FileOpen` overhead; they are comparative spike results, not production
  latency claims.
- Results are one run on a four-logical-processor host and are not a capacity
  characterization.
- The standalone spike executable was blocked by Windows Application Control;
  identical benchmark code ran successfully through the signed/allowed CTest
  transport-test target.

## Decision

**Retain named pipes as the Phase 2 production default. Do not enable shared
memory or DLL imports.**

This follows the frozen architecture: shared memory may be selected only when
both material benefit and a safe fence-free publication protocol are proven.
Only the performance condition passed.
