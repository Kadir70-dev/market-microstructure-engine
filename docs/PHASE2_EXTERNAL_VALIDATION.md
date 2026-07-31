# Phase 2 External MT5/Exness Validation

## Status and authority

Phase 2 is implemented and locally verified. This runbook closes only its
external gates under `ARCHITECTURE_V1.md` and `ARCHITECTURE_V1_ERRATA.md`.
Named Pipes are the only transport. Safety Rule #0 remains in force: the feed
EA contains no order code, `trade_allowed` is emitted as zero, and live trading
must remain disabled.

The external gates are:

- all five symbols sustain at least 10 events/second during a representative
  London/New York validation window;
- `BookSource` and `AccountMode` are classified and logged;
- zero sequence gaps over one continuous 24-hour run;
- zero recorder crashes and zero dropped events during that run;
- production WAL recovery succeeds after a forced recorder interruption;
- 14 consecutive UTC days produce valid WAL.

Do not create the `phase2-feed-recorder` checkpoint until every gate has signed
evidence. Do not start Phase 3.

## Symbols and frozen values

The frozen symbol order is:

| ID | Broker symbol (MT5 bridge) | Canonical instrument (engine) |
|---:|---|---|
| 0 | EURUSDm | EURUSD |
| 1 | GBPUSDm | GBPUSD |
| 2 | USDJPYm | USDJPY |
| 3 | XAUUSDm | XAUUSD |
| 4 | USOILm | OIL_WTI |

Broker suffixes belong to the MT5 bridge only. Canonical instrument identity
travels the wire as `MdRecord.symbol_id`, so the ID column is the frozen
contract and the broker column may differ per account. No C++ source, protocol
field, wire format, or WAL format depends on the broker string.

Margin modes are `0=NETTING`, `1=EXCHANGE`, `2=HEDGING`. Book sources are
`0=L1_ONLY`, `1=DOM_AGGREGATED`, `2=DOM_SYNTHETIC`, `3=L2_EXCHANGE`, and
`4=L3_MBO`. MT5 CFD instruments are expected to classify as `L1_ONLY` or a DOM
classification; lack of DOM is not a failure.

## Before starting

- [ ] Use an MT5 demo account unless written live approval exists. Do not
      enable live trading for this validation.
- [ ] Confirm at least 20 GB free and preferably the architecture provision of
      200 GB.
- [ ] Synchronize the Windows host clock using normal slew discipline.
- [ ] Build `phase2c_recorder.exe` in Release using the pinned vcpkg toolchain.
- [ ] Record the executable SHA-256, git working-tree identifier, Windows
      version, MT5 build, broker server, account number, and margin mode.
- [ ] Create a new evidence directory for the campaign. Preserve raw WAL,
      `.meta`, `.zst`, logs, health reports, and sequence reports.
- [ ] Confirm DLL imports are disabled. Named Pipes do not require them.
- [ ] Confirm no `mme_exec` EA is attached.

Generate the hashes with the exact UTF-8 strings used by the EA:

```powershell
$symbols = 'EURUSDm,GBPUSDm,USDJPYm,XAUUSDm,USOILm'  # must equal the EA's HashText literal
$server = '<exact AccountInfoString(ACCOUNT_SERVER) value>'
$sha = [Security.Cryptography.SHA256]
$utf8 = [Text.Encoding]::UTF8
$symbolHash = [Convert]::ToHexString($sha::HashData($utf8.GetBytes($symbols))).ToLowerInvariant()
$serverHash = [Convert]::ToHexString($sha::HashData($utf8.GetBytes($server))).ToLowerInvariant()
```

`session_epoch` must be monotonic and must match on both sides. Use a new epoch
after each deliberate bridge restart. Never reuse an older value after a newer
one has operated.

## Phase A — EA compilation and handshake

### Compile in MetaEditor

1. Copy `mme_feed.mq5`, `mme_protocol.mqh`, and `mme_pipe.mqh` into one
   `MQL5/Experts/MME/` directory.
2. Open `mme_feed.mq5` in the MetaEditor belonging to the validation terminal.
3. Compile it in the default feed-only configuration.
4. Require zero errors. Archive the compiler output and generated EX5 hash.
5. Search the compilation unit for `OrderSend`, `OrderSendAsync`, `CTrade`,
   `PositionOpen`, and `PositionClose`; every count must be zero.

### Start and attach

Set the launcher environment in a dedicated Command Prompt:

```bat
set MME_RECORDER=C:\path\to\build-phase2c-win-release\Release\phase2c_recorder.exe
set MME_PIPE=\\.\pipe\mme_md
set MME_WAL_DIR=D:\mme-capture\campaign-001
set MME_ACCOUNT=<demo-login>
set MME_EPOCH=<monotonic-uint64>
set MME_MARGIN_MODE=<0-or-1-or-2>
set MME_SERVER_HASH=<64-lowercase-hex>
set MME_SYMBOL_HASH=<64-lowercase-hex>
set MME_RUN_SEED=<fixed-uint64>
scripts\run_feed_capture.bat
```

The recorder creates the pipe server and waits. Then attach `mme_feed` to one
chart, set `InpExpectedAccount` and `InpExpectedMarginMode` identically, and
confirm its pipe input is `\\.\pipe\mme_md`. Leave terminal live trading and
DLL imports disabled.

Expected recorder log:

```text
PIPE_CONNECTED transport=named_pipe pipe=\\.\pipe\mme_md
HANDSHAKE_ACCEPT account=<login> session_epoch=<epoch> account_mode=<mode> book_source_0=<n> ... book_source_4=<n>
```

Success requires the exact account, epoch, server hash, margin mode, and symbol
hash to agree. Any `HANDSHAKE_REJECT`, EA removal, missing pipe connection, or
unexpected account/server/mode is a hard failure. Do not weaken the check;
correct the operator inputs.

## Phase B — five-symbol session

Run during a documented London/New York liquid window. The EA subscribes to all
five frozen symbols from one chart.

- [ ] Confirm all symbols are selected in Market Watch and broker aliases match
      the frozen EA names. An alias mismatch is a configuration failure.
- [ ] Run for at least 30 representative minutes.
- [ ] Tail the capture log:

```powershell
Get-Content D:\mme-capture\campaign-001\logs\capture_*.log -Wait
```

- [ ] Observe `CAPTURE_STATS` at 60-second intervals.
- [ ] Confirm `sequence_gaps=0` and `dropped_events=0` throughout.
- [ ] Archive the final statistics report:

```bat
scripts\report_capture_stats.bat D:\mme-capture\campaign-001 D:\mme-capture\campaign-001\logs\capture_<timestamp>.log > D:\mme-capture\campaign-001\phase_b_stats.json
```

Success requires every symbol to average at least 10 events/second over the
selected window. The handshake must log all five `BookSource` values and the
actual account mode. Ingress p50/p95/p99 are emitted in nanoseconds; p99 must
remain below 200 microseconds. Any missing symbol, lower rate, nonzero gap/drop,
recorder halt, or unclassified handshake field fails Phase B.

## Phase C — continuous 24-hour run

Use a fresh directory and stable session epoch. Avoid terminal updates,
sleep/hibernate, network changes, and manual EA removal during the continuous
measurement.

1. Start the recorder and EA using Phase A.
2. Monitor the process and disk externally; do not auto-restart it during the
   primary continuous run.
3. After at least 24 continuous hours, stop the EA and recorder in a controlled
   manner.
4. Run:

```bat
scripts\check_wal_health.bat D:\mme-capture\phase-c-24h > D:\mme-capture\phase-c-24h\wal_health.jsonl
scripts\check_sequence_gaps.bat D:\mme-capture\phase-c-24h > D:\mme-capture\phase-c-24h\sequence_gaps.jsonl
scripts\report_capture_stats.bat D:\mme-capture\phase-c-24h D:\mme-capture\phase-c-24h\logs\capture_<timestamp>.log > D:\mme-capture\phase-c-24h\capture_stats.json
```

Require exit code zero from all three scripts, zero `INGRESS_HALT` and
`RECORDER_HALT` records, `CAPTURE_STOP ... recorder_ok=1 compression_ok=1`, no
orphan `.zst.tmp`, and no process restart within the interval.

### Forced-interruption recovery drill

Perform this in a separate validation directory so it does not invalidate the
continuous 24-hour evidence:

1. Capture until at least 10,000 events are present.
2. Record the recorder PID and forcibly terminate only that recorder process:
   `taskkill /PID <pid> /F`.
3. Detach the feed EA after the pipe failure.
4. Restart the recorder against the same WAL directory and then reattach the EA
   using a new monotonic session epoch.
5. Require `RECOVERY_OK`, with `valid_boundary <= committed_boundary`.
6. Require `RECOVERY_COMPRESSION_OK` unless a verified `.zst` already exists.
7. Run WAL health twice. Both runs must pass and report identical record and
   committed-byte totals, demonstrating recovery idempotence.
8. Run the sequence checker. A logged session transition is allowed; a gap
   within either epoch is not.

Failure to restart cleanly, recovery failure, CRC failure after recovery,
forward resynchronization, missing source WAL, or different second-pass health
results fails the drill.

## Phase D — fourteen consecutive valid days

Begin only after Phases A–C pass. Use a dedicated campaign directory and retain
the uncompressed WAL as the primary recovery format.

For each UTC day:

- [ ] Recorder operated continuously for the planned session.
- [ ] WAL health exits zero.
- [ ] Sequence-gap check exits zero.
- [ ] No recorder crash, ring saturation, dropped event, or compression error.
- [ ] No `.zst.tmp` remains.
- [ ] Raw WAL and verified zstd outputs remain present.
- [ ] Capture log records account mode and all five book-source classifications.
- [ ] Disk remains above 20 GB free.
- [ ] Store SHA-256 hashes for the daily evidence files.

A failed day resets the consecutive-day count to zero. Maintenance gaps,
terminal restarts, unexplained missing periods, CRC failures, gaps, drops, and
recorder crashes are failed days. Do not relabel partial days as valid.

After day 14, produce a manifest containing the 14 UTC dates, run/session
epochs, WAL and log hashes, daily health summaries, sequence summaries, event
counts, failure count, MT5 build, broker server, account mode, and BookSource
classifications. Human review of that manifest is required before marking
`phase2-feed-recorder` complete.

## Script behavior

- `run_feed_capture.bat` launches the native Windows recorder.
- `run_feed_capture.sh` launches that Windows executable under an already
  installed Wine environment and converts the host WAL directory with
  `winepath`; it does not install Wine.
- `check_wal_health` validates header CRC, committed-boundary metadata, bounded
  framing, frame CRCs, and incomplete compression markers. It is read-only.
- `check_sequence_gaps` checks global and per-source sequences, resetting only
  at a recorded session-epoch transition.
- `report_capture_stats` reports duration, counts, rates, handshake evidence,
  latest ingress latency, and recorder halt count.
- Windows health wrappers use the approved Ubuntu WSL Python 3 environment.

## Troubleshooting

| Symptom | Required response |
|---|---|
| `HANDSHAKE_REJECT` | Compare exact account, server text/hash, margin mode, epoch, and symbol hash. Do not bypass validation. |
| EA cannot open pipe | Start recorder first; confirm identical pipe spelling, same Windows/Wine environment, and permissions. |
| EA removes itself | Inspect MT5 Experts/Journal and recorder log; fix the write/read or handshake failure before retrying. |
| One symbol has zero/low events | Confirm exact broker symbol name and Market Watch selection. Stop; do not substitute an unapproved symbol silently. |
| `book_source_n=0` | Expected when DOM is unavailable. Record `L1_ONLY`; do not fabricate depth. |
| Sequence gap | Stop the campaign, preserve evidence, classify the interval unusable, and restart with a new epoch after investigation. |
| Ring saturation or recorder halt | Stop capture; preserve WAL/logs. Do not discard or overwrite records. Investigate storage latency/capacity. |
| WAL CRC/length failure | Preserve the original files, perform the separate recovery drill, and do not count the day as valid. |
| `.zst.tmp` remains | Compression did not complete. Preserve raw WAL, investigate, and fail the day. |
| Disk below 20 GB | Stop safely and provision storage. Do not continue unlogged. |
| Application Control blocks recorder | Add an approved organizational allow rule or run on the designated validation host; do not disable security controls ad hoc. |
| MT5 terminal update/restart | Fail the continuous interval unless explicitly part of the separate recovery drill. |

## Final operator sign-off

- [ ] Phase A compile and handshake evidence attached.
- [ ] Phase B five-symbol rates and latency pass.
- [ ] Phase C continuous 24-hour run passes.
- [ ] Forced-interruption recovery and idempotence pass.
- [ ] Fourteen consecutive valid UTC days pass.
- [ ] No live order was sent and no order EA was attached.
- [ ] `phase2-feed-recorder` completion is human-approved.
- [ ] Phase 3 remains unstarted until that approval.
