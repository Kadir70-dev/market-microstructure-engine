# Throughput stress test — paper execution pipeline

**Status:** complete for the measurement; two architectural limits found, both fixed.
**Mode:** `LIVE_TRADING=false`, PaperBroker only, no MT5, no bridge, no order egress.
**Harness:** `src/apps/throughput_bench_main.cpp`, target `throughput_bench`.

---

## 1. What was asked and what was measured

The goal was to find the software ceiling of the paper-trading pipeline: force
every eligible opportunity, evaluate every tick, permit immediate re-entry, and
measure the sustainable rate. The target was "hundreds to thousands of trades
per minute".

The measured plateau is **~6,770,000 trades per minute** (~112,800/second),
single-threaded, through the complete pipeline — signal engine → strategy FSM →
RiskEngine → PaperBroker fill mechanics → position accounting → journal.

That is roughly 2,250x the top of the requested range. **The limit is not the
architecture. It is the risk limits**, which is the correct place for it to be:
`max_trades_per_day = 100,000` is reached in 0.89 seconds of wall time.

---

## 2. Method

The recorded EURUSD tape (17,670 quotes) is loaded once and looped, so data
supply is not the limiter. Market time advances a fixed 1 ms per quote, so
cooldowns, time stops and the RiskEngine's rate windows see a plausible market
clock; throughput is measured against the wall clock. Both numbers are reported
because they answer different questions:

- **wall-clock rate** — what the machine can do.
- **market-time rate** — what the risk limits would permit a real session to do.

Per-second samples are emitted so the trading plateau can be separated from the
post-cap spin; a single run-average would blend the two and understate both.

### Relaxed (trading conservatism, not safety)

| Parameter | Production | Stress |
|---|---|---|
| `cooldown_ns` | 30 s | 0 |
| `max_hold_ns` | 300 s | 1 ns |
| `take_profit_ticks` / `stop_loss_ticks` | 60 / 40 | 1 / 1 |
| `entry_momentum` | 2.0e-6 | 1.0e-12 |
| spread / volatility / quote-rate filters | active | open |
| `max_consecutive_losses` | 5 | lifted |

### Preserved (every safety control stays armed)

Kill switch, stale-data guard, daily-loss halt, drawdown halt, sequence-gap,
backpressure, latency, disconnection and reconciliation halts, the hard 8-hour
session cap, and **every** RiskEngine limit including the per-second,
per-minute and per-day rate caps. Two of them fired during the runs — see §4.

The one deliberate rescaling: `max_daily_loss` is set to 1% of the synthetic
account balance rather than its retail-sized default of 1,000,000 minor units.
The halt stays armed and still trips; without the rescaling it ended the run at
t=3.6 s, before any throughput plateau existed. This is sizing a live control to
the account, not disabling it.

---

## 3. Architectural limits found

### 3.1 `PositionBook::open` never reclaimed closed slots

`include/exec/account.hpp`

In hedging mode every entry opens a fresh ticket. `open()` appended to a
64-element array and only ever grew `count_`; closed positions kept their slot
forever. So `max_positions = 64` was silently a cap on **the number of round
trips a run may ever perform**, not on concurrent exposure. At trade 65 the book
was full, `open()` returned `nullptr`, and the broker fail-closed with
`corrupted_state`.

The fail-close was correct behaviour on a corrupt precondition. The precondition
was the bug.

Fixed by reclaiming closed slots. A closed ticket is history and history lives
in the journal — its `position_update` is emitted before it can be closed.
Tickets remain monotonic (`ticket_seq_` never rewinds), so a reused slot can
never be confused with the position that vacated it.

**Effect: 64 → 45,831 trades in a 15 s run.**

### 3.2 `has_seen` linear-scanned the journal on every submit

`include/exec/paper_broker.hpp`

Order-identity dedupe (Part 8.2, Invariant 4) walked every journal record on
every `submit()`. At a full 16,384-record ring that is O(16k) per order, and it
showed up directly in the measurement:

| Stage | p50 before | p50 after |
|---|---|---|
| quote apply | 64 ns | 64 ns |
| strategy decision | 128 ns | 128 ns |
| **submit** | **16,384 ns** | **512 ns** |
| entry-to-close round trip | 32,768 ns | 4,096 ns |

Two orders of magnitude above everything else, and the single dominant cost in
the pipeline.

It was also weaker than it appeared. The journal is a bounded ring; once it
rolled, the duplicate guarantee quietly expired. A duplicate `broker_order_ref`
arriving after rotation would have been accepted.

Replaced with a fixed open-addressed set (1,024 slots, 8-step probe, splitmix64
hash). O(1), no allocation, and independent of the journal so it survives
rotation. It remains a bounded window — the hot path may not allocate — but the
window is now explicit, and eviction is deterministic so the Part 11.3
determinism gate is unaffected.

Only the 8-byte logical id is stored: every ref a broker mints carries its own
`config_.run_id`, so run_id is invariant within an instance and `has_seen`
guards the run explicitly instead of widening every slot. Sizing is load-bearing
— an initial 8,192 x 16 B table overflowed MSVC's 1 MiB default stack in suites
that keep two brokers live by value (`phase5_reduce_only`, `0xC00000FD`).

**Effect: submit p50 16,384 ns -> 512 ns; overall plateau 750k -> 6.77M
trades/min, a 9x improvement.**

### 3.3 Slippage cap inverted the Part 9.4 mode ordering (regression, fixed)

`include/exec/models.hpp`

An earlier fix bounded compounding slippage by capping the *product* of the mode
multiplier and the regime stress at 2.0. In the adverse bucket that collapsed
base (1.0 x 4.0) and pessimistic (2.0 x 4.0) onto the same clamped value, so
pessimistic was no longer worse than base — inverting
`PnL(opt) >= PnL(base) >= PnL(pess)`, the property that makes pessimistic the
promotion-grade mode. Caught by `slippage_pessimistic_worse_than_base`.

The bound now applies to the regime amplifier only, so the mode multiplier still
carries the ordering. Worst case is 4x spread instead of 8x.

---

## 4. Where the ceiling actually is

All three binding constraints are risk controls behaving exactly as designed:

| Control | Value | Effect |
|---|---|---|
| `max_trades_per_day` | 100,000 | hard stop at t = 0.89 s |
| `max_orders_per_minute` | 10,000 | caps market-time rate at 5,000 round trips/min |
| `max_daily_loss` | (retail default) | tripped at t = 3.6 s pre-fix |
| 8-hour session cap | 28,800 s market | ended one run at market_seconds = 28,800 exactly |

After the day cap binds, the engine keeps evaluating every tick and rejecting
every intent at ~2.4M events/s — the fail-closed path is not a slow path.

---

## 5. Resource profile

| Metric | Value |
|---|---|
| CPU | 1.15 cores (single-threaded + sampling) |
| RSS | 6.7 MiB, **zero growth** over 18.6M events |
| Idle pipeline | ~2.4M events/s |
| Trading pipeline | ~1.37M events/s |
| ns/event | 538 |

Flat memory across 18.6M events confirms the no-allocation hot-path design
holds under sustained load: every structure on the path is a fixed array.

---

## 6. Remaining bottleneck

The journal ring (16,384 records) saturates roughly every 630 trades — 158
rotations per 100,000 trades. For the paper simulator this is only a research
concern, but a live system with durable journaling would be doing an fsync at
that cadence, and that, not the strategy, would set the real order rate. This
is the next thing to look at before any live-adjacent work.

---

## 7. Test status

93 of 94 pass. The one failure, `phase7_wal_tailer`, is pre-existing and
platform-specific, not a consequence of these changes: it includes no `exec/`
header and touches none of the modified code. Its five failing assertions all
read a WAL segment while the writer still holds it, and
`src/persist/wal_writer.cpp` opens both the `.wal` and `.meta` handles with
`dwShareMode = 0`. On Windows that makes a live segment unopenable by any
reader, including one in the same process. The remaining 44 assertions in that
suite — finished segments, multi-segment, cursor restart, no-duplicate — pass.

`paper_trade.exe` could not be relinked while the live runtime was running and
holding the binary, so **the running paper trader still carries the 64-lifetime-
position defect from §3.1** and will fail closed at its 65th round trip. It
needs a restart on the rebuilt binary.

## 8. Reproduction

```
cmake --build build-phase2c-win-release --config Release --target throughput_bench
./build-phase2c-win-release/Release/throughput_bench.exe data/wal 20 0
```

Arguments: `<wal-dir> [wall_seconds] [symbol_id] [tick_step_ns]`.
