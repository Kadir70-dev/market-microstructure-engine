# MARKET MICROSTRUCTURE ENGINE
# TARGET ARCHITECTURE — **VERSION 1.0**

**Status:** MERGED · CERTIFIED · FROZEN
**Supersedes:** v0.9 draft + Architecture Certification Review
**Live trading:** DISABLED at every phase in this document. Phase 11 is a specification for a future human decision, not an authorization.

> **Relationship to `CLAUDE.md`:** this document specifies the *target* architecture. `CLAUDE.md` describes the *current* system and its Safety Rule #0 (no real-money execution). Nothing here authorizes an execution path; Parts 5, 10 and 18 tighten that prohibition rather than relax it.

---

# PART 0 — THE SEVEN DETERMINATIONS

### 0.1 Maximum realistic quote-event rate

MT5 `OnTick` delivers a *coalesced sample*, not every venue tick. You receive what the broker pushes and what the terminal's message loop can process.

| Regime | Per symbol | 5 symbols aggregate |
|---|---|---|
| Quiet (Asia, EURUSD) | 0.5–3 /s | 3–15 /s |
| Active (London/NY overlap) | 3–15 /s | 20–70 /s |
| XAUUSD active | 5–25 /s | — |
| News burst (NFP, CPI, FOMC) | 50–200 /s | **300–1,000 /s** |

**Design target: 10,000 events/s per symbol without loss; 100,000+/s in replay benchmark.** These figures are *design assumptions*, replaced in Phase 2 by measured Exness distributions.

### 0.2 Maximum safe order-request rate

Binding constraint is the MT5 terminal, not the network. With **`OrderSendAsync`** (mandated — see Part 5) the terminal no longer blocks per request, but broker-side pacing still governs.

| Tier | Rate |
|---|---|
| Safe sustained (mandate) | **1 req/s** |
| Safe burst | 3 req/s ≤ 5 s, with backoff |
| Hard cap in risk engine | 2 sustained / 5 burst |

**Reframing insight:** 2,000 round trips/day over 8 hours is **0.07 trades/s**. Order rate was never the constraint. Economics and broker abuse policy are.

### 0.3 Maximum realistic fill rate

| Path | Sustained | Peak |
|---|---|---|
| MT5/Exness live | ≤ 1 fill/s | 2–3 briefly |
| MT5/Exness > 10 fills/s | **Impossible** | — |
| Paper simulator | 50,000+/s | — |
| Crypto WS | 5–20 orders/s | venue-published |

Fill *confirmation* arrives asynchronously via `OnTradeTransaction` and may lag execution by tens to hundreds of ms. The OMS is **order-state-driven, never timestamp-driven**: a fill is never rejected for arriving "late" relative to already-processed market data.

### 0.4 Exness / MT5 restrictions — assumed model

| Class | Assume | Response |
|---|---|---|
| Order-frequency throttling | Present, undocumented | Adaptive governor (Part 13) |
| Minimum holding time | Possible; common on bonus accounts | Configurable floor; never use a bonus account |
| Scalping prohibition | Generally tolerated — **verify in writing** | Phase 0 deliverable, `docs/BROKER_POLICY.md` |
| Latency/quote arbitrage | Prohibited essentially everywhere | **Architectural ban.** No strategy may key off feed-vs-reference lag |
| "Abusive trading" clause | Present and broad | Highest-probability failure at thousands/day |
| Freeze / stop level | Present, varies by symbol and session | Read at runtime; revalidate before every send **and every modify** |
| Execution mode | Market execution — slippage, not requotes | Never assume price certainty |
| **Account margin mode** | **Hedging on most Exness retail accounts** | **Must be read and honoured — Part 8** |

### 0.5 MT5 Depth of Market — per instrument

| Symbol | Type | Verdict |
|---|---|---|
| EURUSD / GBPUSD / USDJPY | FX CFD | **Unavailable or synthetic** — no CLOB exists in spot FX |
| XAUUSD | Metal CFD | **Unavailable or aggregated LP quotes** |
| XTIUSD / USOIL | Oil CFD | **Unavailable or synthetic** |

Runtime classification is mandatory:

```
BookSource ∈ { L1_ONLY, DOM_AGGREGATED, DOM_SYNTHETIC, L2_EXCHANGE, L3_MBO }
```

Every strategy declares a minimum `BookSource`. **The risk engine refuses to start any strategy whose requirement exceeds the live classification.** This is the control that prevents running a queue-dependent market maker against fabricated depth.

### 0.6 Paper / replay only

Market making · true queue position · > 10 fills/s · maker rebates · L2/L3 reconstruction · real OFI and VPIN · sub-ms decision-to-fill · thousands of *fills*/day at micro size with positive expectancy · accelerated multi-year evaluation.

### 0.7 Requires crypto WS/FIX or institutional access

Genuine L2 with sequence + checksum · L3/MBO and true queue · maker rebates · 5–20 orders/s under contractual limits · real trade prints · colocated sub-ms tick-to-trade. **Nanosecond HFT is barred by physics and exchange membership, not software.**

---

# PART 1 — FEASIBILITY VERDICT

| Question | Verdict |
|---|---|
| C++ engine > 10 events/s? | **Yes, by four orders of magnitude** |
| Paper/replay > 10 fills/s? | **Yes**, 50,000+/s |
| MT5 > 10 confirmed live trades/s? | **No.** ~1–3/s peak. Not a code-quality problem |
| Thousands of live trades/day possible? | **Yes mechanically** (0.07 trades/s). Rate was never the wall |
| Economically viable? | **Not as specified** — see below |
| Evidence before live? | All three gates, Part 22 |

### The economics

| Symbol | Size | Spread cost | Commission | **Round-trip floor** |
|---|---|---|---|---|
| EURUSD (raw) | 0.01 lot | ~$0.02 | ~$0.07 | **~$0.09** |
| EURUSD (standard) | 0.01 lot | ~$0.10 | $0 | **~$0.10** |
| XAUUSD | 0.01 lot | ~$0.15–0.30 | ~$0.07 | **~$0.22–0.37** |
| EURUSD | 0.10 lot | ~$0.20 | ~$0.70 | **~$0.90** |

**"A few cents gross per winner" is below the cost floor on every instrument at every size.** 2,000 trades/day burns ~$180/day, ~$45k/year on the smallest possible size.

Resolution requires one of: (a) gross target rises to $0.50–$2.00/winner — fewer, longer trades; (b) trade count falls to hundreds/day with a real per-trade edge — **the realistic MT5 path**; (c) venue changes to crypto where maker rebates make cost/trade zero or negative — **the realistic thousands/day path**.

**Strategic split, binding on this design:** MT5/Exness is the **L1 scalper** venue at hundreds of trades/day. Crypto WS is where the market maker, genuine L2, true queue, and thousands of fills/day are physically and economically possible. The `IFeedAdapter`/`IBroker` boundary makes this one codebase.

---

# PART 2 — CURRENT-STATE GAP ANALYSIS

| Component | Current | Action |
|---|---|---|
| Feed | `OnTimer` 5 s → CSV → `stat()` poll | **Replace** |
| Loop | `kSleepSeconds = 30` batch scan | **Replace** |
| Data type | `struct Quote { double price; }` | **Replace** |
| Schema | `ticks(ts, symbol, price)`, 1 s resolution, mid only | **Replace** |
| Book / Features / Strategy / Risk / OMS / Execution / Simulator / Portfolio / Telemetry / Replay | **none** | **Create** |
| Persistence | synchronous SQLite on the decision path | **Replace** (SQLite retained for aggregates) |
| `validation.cpp` | sound logic, wrong trigger model | **Keep**, make event-driven |
| `evaluation/metrics.cpp` | expectancy math | **Keep**, extend |
| ops / dashboard / CI | good | **Keep**, extend |
| Data asset | **27 ticks** | **Recording is Phase 2, ahead of everything** |
| ML evidence | `"source": "synthetic"`, AUC 0.545 | **Discard as evidence**, keep pipeline |
| Repo hygiene | `build/`, `.netlify/`, `*.db` tracked | **Retire from VCS** |

---

# PART 3 — TARGET COMPONENT DIAGRAM

```
╔═══════════════════════ SOURCES ═══════════════════════════════╗
║  MT5 TERMINAL                                                 ║
║   ┌──────────────────┐        ┌───────────────────┐           ║
║   │ mme_feed.mq5     │        │ mme_exec.mq5      │           ║
║   │ chart A          │        │ chart B           │           ║
║   │ OnTick/OnBook    │        │ OrderSendAsync    │           ║
║   │ OnTimer(1s) HB   │        │ OnTradeTransaction│           ║
║   │ NO ORDER CODE    │        │ dead-man switch   │           ║
║   │ COMPILED IN      │        │ 9 live guards     │           ║
║   └────────┬─────────┘        └────────┬──────────┘           ║
║   Crypto WS/FIX (future)   ·   WAL file (replay)              ║
╚════════════╪═══════════════════════════╪══════════════════════╝
      named pipe (default)         named pipe (default)
      shm (optional, gated)        session_epoch handshake
             │                            │
┌────────────▼────────────────────────────▼─────────────────────┐
│ FEED ADAPTERS → normalize → MarketEvent                       │
│ seq-gap · heartbeat · staleness · BookSource · clock offset   │
└────────────────────────────┬──────────────────────────────────┘
                             │ SPSC ring — BACKPRESSURE POLICY:
                             │ MD ring never drops. 75% → halt new
                             │ entries. 100% → engine halt.
╔════════════════════════════▼══════════════════════════════════╗
║        H O T   P A T H   (single thread, pinned core)         ║
║  no heap alloc · no syscalls · no locks · no strings          ║
║  total order: (ts_local_ns, source_priority, seq_source)      ║
║                                                               ║
║  BOOK ENGINE → FEATURE ENGINE → STRATEGY ENGINE               ║
║  L1/L2/L3       warm-mask         IStrategy                   ║
║  rebase policy  Welford accum     lifecycle FSM               ║
║  crossed-tol    NaN guard         Intent                      ║
║                                        │                      ║
║                                  ┌─────▼──────┐               ║
║                                  │ RISK GATE  │ ◀── atomic    ║
║                                  │ VETO ONLY  │     kill flag ║
║                                  │ signed cfg │     (set by   ║
║                                  └─────┬──────┘   supervisor) ║
║                                        │ Order                ║
║                                  ┌─────▼──────┐               ║
║                                  │ OMS + PMS  │               ║
║                                  │ 2 state m/c│               ║
║                                  └─────┬──────┘               ║
╚════════════════════════════════════════╪══════════════════════╝
              ┌─────────────────────────┴─────────────┐
        ┌─────▼──────┐                    ┌────────────▼────────┐
        │ PaperBroker│  DEFAULT           │ MT5Broker   GATED   │
        │ Philox RNG │                    │ 9 independent guards│
        │ regime-cond│                    │ OFF BY DEFAULT      │
        │ margin/SO  │                    └────────────┬────────┘
        └─────┬──────┘                                 │
              └──────────────┬──────────────────────────┘
                     ┌───────▼───────┐
                     │   PORTFOLIO   │ netting | hedging aware
                     │  multi-ccy    │
                     └───────┬───────┘
                             │ async, off hot path
   ┌─────────────────────────┼──────────────────────────┐
┌──▼──────────────┐ ┌────────▼─────────┐ ┌──────────────▼──────┐
│ WAL (len-pfx,   │ │ TELEMETRY        │ │ SQLite aggregates   │
│ CRC, rotated)   │ │ HDR histograms   │ │ → Next.js dashboard │
│ JOURNAL (framed)│ │ → Prometheus     │ │ corr_id trace view  │
│ → Parquet/DuckDB│ └──────────────────┘ └─────────────────────┘
└──┬──────────────┘
   │        ┌──────────────────┐   ┌───────────────────────────┐
┌──▼────────▼──────┐           │   │ SUPERVISOR THREAD (10 Hz) │
│ PYTHON RESEARCH  │           │   │ kill file · intents ·     │
│ → params.json    │           │   │ instance lock · disk check│
│ (SHA-256 signed) │           │   │ → atomic flags to hot path│
└──────────────────┘           │   └───────────────────────────┘
                    ┌──────────▼──────────────┐
                    │ CONTROL SERVICE (Python)│
                    │ TradingView HMAC webhook│
                    │ advisory intents only   │
                    └─────────────────────────┘
```

**Central invariant:** identical strategy, feature, risk, OMS, portfolio and simulator code across replay, paper and live. Only `IFeedAdapter`, `IBroker` and `IClock` differ.

---

# PART 4 — C++ / PYTHON BOUNDARY

**C++ owns every per-event operation. Python owns everything a human iterates on. They communicate through versioned, SHA-256-signed files — never FFI, never an embedded interpreter, never a socket on the hot path.**

| C++ (hot path, deterministic) | Python (offline) |
|---|---|
| Feed adapters, normalization, seq/gap/heartbeat, clock offset | Dataset generation from WAL/Parquet |
| Order book, rebase, checksum, reconciliation | Feature validation and discovery |
| Feature engine, warm-mask, Welford accumulators | Leak-free label generation |
| Strategy execution + lifecycle | Walk-forward, purged/embargoed CV |
| **Risk engine — final authority, always** | Transaction-cost modelling |
| OMS + Position management, idempotency, recovery | Bayesian / grid optimization |
| Paper simulator (queue, latency, fees, slippage, margin) | Regime, Monte Carlo, bootstrap, permutation |
| Portfolio accounting, multi-currency | Drift, calibration, importance, comparison |
| WAL/journal writers, telemetry | Paper-vs-replay reconciliation, promotion gates |

**Handoff contract:** Python emits `config/params/<strategy>_<version>.json` containing thresholds, weights, the training-dataset SHA-256 and its own content hash. C++ validates schema + both hashes at load and **refuses to start on mismatch**.

**Config reload is double-buffered:** the supervisor thread parses and validates a new config into an inactive buffer, then publishes it by atomic pointer swap, applied by the hot path only at an event boundary. `SIGHUP` never touches live state.

**ML may output:** probabilities, regime labels, feature weights, parameter sets, confidence scores.
**ML may never output:** an order, a size, or a risk-limit change.

---

# PART 5 — MT5 / MQL5 BRIDGE PROTOCOL

## 5.1 Topology — two EAs, one terminal

```
                    ┌──── md pipe ────▶ C++ engine
mme_feed.mq5 ───────┤
(chart A)           └──── hb pipe ────▶
  OnTick, OnBookEvent, OnTimer(1s)
  NO ORDER CODE COMPILED IN

C++ engine ──── cmd pipe ────▶ mme_exec.mq5  ──OrderSendAsync──▶ Exness
           ◀─── evt pipe ────  (chart B)     ◀──OnTradeTransaction
                                 9 live guards + dead-man switch
```

**Rationale:** all MQL5 handlers on one chart share one thread. A blocking `OrderSend` would stall `OnTick` for the full broker round trip — going blind precisely while risk is in flight, and firing false staleness halts on your own order activity. Two charts plus `OrderSendAsync` removes both. It also means the feed EA has **no order-sending code compiled into it at all**.

## 5.2 Transport — named pipes by default

MQL5 has **no native shared memory**. A shm ring requires `kernel32` imports (`CreateFileMapping`/`MapViewOfFile`/`RtlMoveMemory`), terminal-wide "Allow DLL imports", **no atomics or memory fences anywhere in MQL5**, and under Wine a file-backed mapping shared with a native Linux process.

Named pipes are MQL5-native via `FileOpen` on `\\.\pipe\`, cost ~0.5–2 ms, and require no DLL imports. Against a 30–300 ms broker round trip that is **≤ 0.7% of end-to-end latency**.

| Transport | Status |
|---|---|
| **Named pipe** | **DEFAULT.** Mandated unless the Phase-2 spike proves otherwise |
| Shared memory | Optional optimization. Gated behind the Phase-2 spike proving a fence-free publish protocol is safe under x86-TSO, with measured benefit. DLL imports remain disabled unless the spike succeeds |
| ZeroMQ | Permitted only if the engine and terminal are on different hosts |
| CSV / mtime polling | **Prohibited** |

## 5.3 Session handshake and restart safety

Both sides persist a monotonic `session_epoch`. On attach:

1. C++ writes `HelloRecord{proto_version, session_epoch, account_expected, symbol_set_hash}`.
2. EA replies `HelloAckRecord{proto_version, account_actual, server_hash, account_margin_mode, book_source[], symbol_meta_hash}`.
3. **Any mismatch in `proto_version`, account, server or margin mode → both sides refuse to operate and alert.**
4. Reader **resynchronises to the writer's current head**, never to a persisted consumed-index.
5. **Any record carrying a stale `session_epoch` is discarded, counted as `EXPIRED_ON_RESTART`, and alerted. It is never executed.** A non-zero count is a startup halt condition pending human review.

Struct packing is explicit: fully padded PODs, little-endian, `static_assert`ed field offsets and total sizes on the C++ side, hand-verified against the MQL5 layout in `mme_protocol.mqh`.

## 5.4 Records

```
Hello / HelloAck : proto_version | session_epoch | account | server_hash |
                   margin_mode | book_source[] | symbol_meta_hash
MdRecord   : magic|ver|epoch|seq|type|symbol_id|ts_broker_ms|ts_terminal_ms|
             bid|ask|last|bid_vol|ask_vol|tick_volume|flags
BookRecord : magic|ver|epoch|seq|symbol_id|ts|n_bid|n_ask|levels[N]|checksum
CmdRecord  : magic|ver|epoch|seq|corr_id|run_id|logical_order_id|cmd|symbol_id|
             side|volume|price|sl|tp|deviation|filling_mode|type_time|
             expiration|position_ticket|arm_token|hmac
AckRecord  : magic|ver|epoch|seq|corr_id|logical_order_id|retcode|deal|order|
             position|price|volume|ts_sent_ms|ts_reply_ms
TxnRecord  : magic|ver|epoch|seq|trans_type|order|deal|position|symbol_id|
             volume|price|profit|commission|swap|ts
HbRecord   : magic|ver|epoch|seq|ts_terminal_ms|account|server_hash|trade_mode|
             margin_mode|connected|trade_allowed|equity|balance|margin|
             free_margin|account_currency|engine_hb_age_ms
```

Every record carries broker timestamp (where available), terminal timestamp, local monotonic receive timestamp, symbol ID, sequence ID, source and correlation ID.

## 5.5 Live guards — nine independent conditions, all must pass

1. **Compile-time** `#ifdef MME_ALLOW_LIVE` — absent by default; the default `mme_exec.mq5` build **cannot call `OrderSendAsync`**
2. **Account mode** — `ACCOUNT_TRADE_MODE == DEMO` unless the live flag is compiled in
3. **Account allowlist** — compiled-in login numbers
4. **Server allowlist** — `AccountInfoString(ACCOUNT_SERVER)` match
5. **Margin-mode agreement** — broker-reported mode must equal configured mode
6. **Arm file + token** — `arm_token` matches a manually created arm file, mtime < 8 h
7. **Runtime flag** — engine started with `--live`
8. **Instance lock held** — engine proved single-instance
9. **Symbol allowlist at EA level** — EA independently rejects symbols outside its compiled list

**Any disagreement → reject, log loudly, halt the bridge.**

## 5.6 Dead-man switch

`mme_exec.mq5` monitors `engine_hb_age_ms` independently. On loss exceeding `InpDeadManSeconds` (default 30 s) it **closes all positions carrying our magic number and disables itself**, without any instruction from the engine. The EA must be able to protect the book when the engine is gone.

## 5.7 Order validation before send

Symbol tradable · volume within `VOLUME_MIN/MAX` snapped to `VOLUME_STEP` · price snapped to `TICK_SIZE` · SL/TP beyond `STOPS_LEVEL` and outside `FREEZE_LEVEL` · filling mode in `FILLING_MODE` · spread ≤ max · market open · `TERMINAL_TRADE_ALLOWED` · `MQL_TRADE_ALLOWED` · **broker-side SL present** (Part 10 invariant).

**Freeze/stop levels are revalidated before every modify, not only at open.**

## 5.8 Retcode handling

| Class | Action |
|---|---|
| Success (`DONE`, `DONE_PARTIAL`, `PLACED`) | Apply to OMS |
| Retryable (`REQUOTE`, `PRICE_CHANGED`, `PRICE_OFF`, `TIMEOUT`) | Backoff, revalidate, retry **only under the same `logical_order_id`** |
| Rate (`TOO_MANY_REQUESTS`, `LIMIT_ORDERS`, `LIMIT_VOLUME`) | Immediate governor backoff |
| Fatal (`INVALID_*`, `NO_MONEY`, `TRADE_DISABLED`, `MARKET_CLOSED`, `FROZEN`) | Terminal reject, no retry |
| **Uncertain** (timeout, no ack) | **Never blind-resend.** → `UNKNOWN`, reconcile by `broker_order_ref`, resolve, then act |

## 5.9 Symbol metadata and account state

Captured at handshake and on change, keyed by internal `symbol_id`, with a broker-alias table (`XTIUSD | USOIL | WTI | CRUDE → OIL_WTI`):

`tick_size · tick_value · contract_size · volume_min/step/max · stops_level · freeze_level · spread_current · spread_float · commission · swap_long/short · swap_mode · trade_sessions · quote_sessions · execution_mode · filling_modes · digits · point · margin_rate · trade_mode · profit_currency · margin_currency`

Account-level: `account_currency · margin_mode · leverage`.

**Prices are `int64` ticks everywhere on the hot path.** Doubles appear only at the display boundary.

**Mid-session metadata change:** a change to `stops_level`, `freeze_level` or `filling_mode` raises a `SymbolMetaChangeEvent`; in-flight orders for that symbol are revalidated, and any that no longer satisfy constraints are cancelled rather than amended silently.

---

# PART 6 — TRADINGVIEW INTEGRATION

**Outside the latency path.** Permitted: charting, strategy-state and fill/PnL display, slow-timeframe regime confirmation, research comparison, supervisory alerts, human kill/pause.

### Why TradingView cannot support 10+ trades/second

Webhooks are public-internet HTTPS POSTs (100 ms – seconds, unbounded tail, no ordering guarantee) · alerts fire on bar close or TradingView's own evaluation schedule, not per tick · alert counts are plan-limited to tens/hundreds · no delivery guarantee and no ack channel · **TradingView's feed is not your broker's feed** — acting on it against an Exness account trades a different instrument's prices · no order state, so it cannot participate in reconciliation. This places it three to five orders of magnitude from the hot path.

### Webhook security contract

HMAC-SHA256 over the raw body, constant-time compare · mandatory `timestamp` + `nonce`, **TTL ≤ 30 s**, replay cache · strict schema, unknown fields rejected · IP allowlist + mTLS where available · ≤ 10 req/min · full request logging with verdict.

| Intent | Effect |
|---|---|
| `REGIME_HINT` / `BIAS_HINT` | Strategy input only. **Cannot open a position** |
| `PAUSE` | Stop new entries; manage existing |
| `FLATTEN` | Close all, then disable |
| `KILL` | Immediate halt, persistent state, manual re-arm |

Safety-direction intents (`PAUSE`/`FLATTEN`/`KILL`) are honoured unconditionally. Hints pass through risk like any other input. **No webhook path places a trade.** Intents reach the hot path via the supervisor thread's atomic flags and an SPSC command queue — never a syscall on the hot path.

---

# PART 7 — MARKET-EVENT SCHEMAS

## 7.1 Event header — 56 B, on every event

```cpp
struct EventHeader {              // static_assert(sizeof == 56)
  uint64_t seq_global;            // engine-assigned, monotonic
  uint64_t ts_broker_ns;          // venue/broker, UTC ns, 0 if unavailable
  uint64_t ts_terminal_ns;        // MT5 terminal, UTC ns (offset-corrected)
  uint64_t ts_local_ns;           // steady_clock at ingress — latency origin
  uint64_t corr_id;               // order→ack→fill→position→journal linkage
  uint32_t seq_source;            // per-source, gap detection
  uint32_t symbol_id;             // interned; no strings on the hot path
  uint16_t source_id;             // MT5_FEED|MT5_EXEC|CRYPTO_WS|REPLAY|SIM|INTERNAL
  uint16_t type;                  // EventType
  uint8_t  source_priority;       // total-order tie-break
  uint8_t  flags;
  uint16_t _pad;
};
```

## 7.2 Total event order

The engine imposes a **total order** on every event:

```
(ts_local_ns, source_priority, seq_source)
```

`source_priority` is a fixed table — `MT5_EXEC(0) < MT5_FEED(1) < CRYPTO_WS(2) < INTERNAL_TIMER(3) < CONTROL(4)`. Execution events sort ahead of market data at equal timestamps so the OMS always sees state changes before decisions made on them.

Replay `TimerEvent`s are generated deterministically from the virtual clock and interleaved by the same rule. **Live and replay orderings across sources may differ; only replay is reproducible.** This is stated, accepted, and does not affect determinism *within* a replay.

## 7.3 Event sizing — two classes

**Fixed events — 128 B total (56 B header + ≤ 72 B payload), `static_assert`ed:**

| Event | Payload |
|---|---|
| `QuoteEvent` | `bid, ask, bid_size, ask_size, flags` — 36 B |
| `TradeEvent` | `price, size, aggressor_side, trade_id` — 28 B |
| `BookDelta` | `side, price, new_size, action{ADD,MOD,DEL}` — 26 B |
| `TimerEvent` | `timer_id, scheduled_ns` — 12 B |
| `OrderAck` | `logical_order_id, broker_order_id, price, volume` — 32 B |
| `OrderReject` | `logical_order_id, retcode, reason_class` — 16 B |
| `Fill` / `PartialFill` | `logical_order_id, deal_id, position_ticket, price, volume, commission, swap, remaining, is_final` — 68 B |
| `CancelAck` | `logical_order_id, cancelled_volume` — 16 B |
| `PositionUpdate` | `position_ticket, symbol_id, volume, avg_price, unrealized, margin` — 44 B |
| `AccountUpdate` | `balance, equity, margin, free_margin, margin_level, currency_id` — 44 B |
| `HeartbeatEvent` | `source_id, ts, connected, trade_allowed, book_source` — 20 B |
| `GapEvent` | `source_id, expected_seq, received_seq, lost_count` — 16 B |
| `BackpressureEvent` | `ring_id, occupancy_pct, dropped_count` — 12 B |
| `SymbolMetaChangeEvent` | `symbol_id, changed_mask` — 8 B |
| `BookRebaseEvent` | `symbol_id, old_ref, new_ref` — 20 B |
| `ClockStepEvent` | `wall_delta_ns, detected_at` — 16 B |

**Slab-backed events — `BookSnapshot` only:**

A 32-level two-sided book is ~1 KB and **cannot fit a 128 B union**. `BookSnapshot` therefore carries a handle into a pre-allocated slab pool:

```cpp
struct BookSnapshotRef {   // 21 B payload
  uint32_t slab_index; uint32_t generation;
  uint16_t n_bid; uint16_t n_ask;
  uint64_t checksum; uint8_t book_source;
};
```

Slab pool: `MAX_BOOK_DEPTH = 32`, 4,096 slabs × 1,040 B ≈ **4.3 MB**, allocated once at startup. `generation` detects a reused slab (stale handle → assertion in debug, event drop + counter in release). **Zero allocation after startup is preserved.**

---

# PART 8 — ORDER AND POSITION STATE MACHINES

## 8.1 Account margin mode is first-class

`ACCOUNT_MARGIN_MODE` is read at handshake and must equal the configured mode. **The engine refuses to start on disagreement.** Position semantics differ fundamentally:

| | **Netting** | **Hedging** *(Exness retail default)* |
|---|---|---|
| Positions per symbol | exactly one, signed | many, each a distinct `position_ticket` |
| "Close position" | reduce toward zero | close **a specific ticket** |
| Average entry | recomputed on each fill | per ticket |
| SL/TP | one set per symbol | one set per ticket |
| Swap | one accrual | per ticket |
| `reduce_only` | reduces \|net volume\| | closes/reduces a **named ticket** |
| Reconciliation key | `symbol_id` | `position_ticket` |

The portfolio maintains **positions keyed by `position_ticket`** with a derived per-symbol aggregate view used for risk limits. In netting mode there is exactly one ticket per symbol; the same code path serves both. This removes the permanent false-mismatch failure that a net-only model produces on hedging accounts.

## 8.2 Order identity

Two identifiers, separating determinism from uniqueness:

- **`logical_order_id`** = `hash(strategy_id, symbol_id, seq_global)` — deterministic, reproducible in replay, used for journal linkage and replay comparison.
- **`run_id`** — constant within a run, recorded in the journal and WAL headers, varying across restarts.
- **`broker_order_ref` = `(run_id, logical_order_id)`** — globally unique.

**MT5 encoding:** magic number = `run_id` (low 32 bits) ‖ strategy tag; order comment = base36(`logical_order_id`), ≤ 31 chars. A collision check against the in-flight set runs before every send; a collision is a halt condition, not a retry.

## 8.3 Order state machine

```
                    ┌──────────────┐
                    │     NEW      │
                    └───┬──────┬───┘
              risk veto │      │ approve / resize
                  ┌─────▼──┐   ▼
                  │RISK_REJ│ ┌────────────┐
                  │ ECTED  │ │PENDING_SEND│
                  └────────┘ └─────┬──────┘
                                   ▼
                              ┌────────┐
                              │  SENT  │  (OrderSendAsync returns here)
                              └┬──┬──┬─┘
                  ack ┌────────┘  │  └────────┐ timeout
                      ▼           ▼           ▼
              ┌──────────────┐ ┌────────┐ ┌────────┐
              │ ACKNOWLEDGED │ │REJECTED│ │UNKNOWN │
              └──┬────────┬──┘ └────────┘ └───┬────┘
       partial   │        │ full             │ reconcile →
                 ▼        ▼                   │ any terminal
        ┌────────────────┐ ┌────────┐         │ state
        │PARTIALLY_FILLED├─▶ FILLED │◀────────┘
        └───┬────────────┘ └────────┘
            │ cancel
            ▼
     ┌──────────────┐   ┌───────────┐   ┌─────────┐
     │CANCEL_PENDING├──▶│ CANCELLED │   │ EXPIRED │
     └──────────────┘   └───────────┘   └─────────┘
```

Terminal: `RISK_REJECTED, FILLED, CANCELLED, REJECTED, EXPIRED`.
**`UNKNOWN` is never terminal** — it holds full reserved exposure until reconciliation resolves it.

## 8.4 Position state machine

Break-even and trailing stops require modifying a **live position**, which on MT5 is `PositionModify` — a distinct operation on a distinct object.

```
        ┌──────────┐
        │ OPENING  │  (order accepted, awaiting fill)
        └────┬─────┘
             ▼
        ┌──────────┐  modify req   ┌────────────────┐
        │   OPEN   ├──────────────▶│ MODIFY_PENDING │
        │          │◀──────────────┤                │
        └──┬───┬───┘  ack / reject └────────────────┘
   close   │   │ broker SL/TP hit
           ▼   ▼
    ┌──────────┐        ┌──────────┐
    │ CLOSING  ├───────▶│  CLOSED  │
    └──────────┘        └──────────┘
             │
             ▼ divergence detected
    ┌───────────────────┐
    │ RECONCILE_UNKNOWN │ → halt, human resolution
    └───────────────────┘
```

Modify requests are idempotent on `(position_ticket, modify_seq)`. Freeze and stop levels are revalidated immediately before every modify. A rejected modify never silently retries — it returns to `OPEN` with the old stop intact and increments a counter.

## 8.5 Guarantees

- **Idempotency:** a `broker_order_ref` may be sent more than once but only ever transitions one order object. Exposure is reserved at `PENDING_SEND` and released only on a terminal state.
- **Duplicate prevention:** in-flight ref set checked before every send.
- **Retries:** same ref, capped attempts, exponential backoff, **never from `UNKNOWN`**.
- **Brackets:** SL/TP attached at send. **A symbol whose stop/freeze levels prevent a broker-side SL is not tradable** (Part 10).
- **Reduce-only:** carries the target `position_ticket`; risk rejects any reduce-only order that would increase absolute exposure.
- **Fill ordering:** the OMS is order-state-driven. A fill timestamped before already-processed market data is applied normally and never dropped.

## 8.6 Reconciliation

Every 5 s and on every reconnect/restart: compare engine positions and orders against `PositionsTotal`/`OrdersTotal`, keyed by `position_ticket` (hedging) or `symbol_id` (netting), matched via magic number and comment. **Equity and balance are reconciled alongside positions** — a PnL divergence beyond tolerance is itself a break.

On divergence: **halt, alert, no automatic correction, human resolution required.** Silent auto-correction of a position mismatch is prohibited.

Orphan detection: any broker-side order or position carrying our magic number but unknown to the OMS → immediate halt + alert.

Restart recovery: journal replay → OMS/PMS rebuild → broker reconcile → resolve every `UNKNOWN` → only then permit new orders.

---

# PART 9 — PAPER EXECUTION SIMULATOR

**Default execution mode in every build.**

## 9.1 Determinism

The simulator's probabilistic models use a **counter-based PRNG (Philox-4×32-10)** keyed on `(run_seed, corr_id, stage_id)`. Every draw is a pure function of the event stream — no sequential state, order-independent, trivially reproducible. `run_seed` is recorded in the journal header.

**The Phase 4 determinism test is re-run as a Phase 5 exit criterion with the simulator in the loop.**

## 9.2 Fill mechanics

| Component | Specification |
|---|---|
| **Never mid** | Mid-price fills are prohibited by construction; no code path can produce one |
| Market orders | Cross the spread; walk L2 levels where available; calibrated slippage on L1 |
| Limit orders | Fill only on genuine market activity through the level — never on touch |
| **Queue position** | `queue_ahead = size_at_level` at placement; decremented by trades at that price and an estimated share of depth reductions; fill only at `queue_ahead ≤ 0`. On `L1_ONLY`/`DOM_*` sources, queue-dependent results are tagged **NOT VALIDATED** and are inadmissible for promotion |
| Partial fills | `fill = min(remaining, aggressor_size − queue_ahead)`; multiple per order; exact remaining accounting |
| Latency | Distributions, sampled at `t_send + λ`, never `t_decision` |
| Rejections / requotes | Probabilistic, conditional (below) |
| Stale quotes | Orders against a quote older than N ms are rejected or filled at the newer, worse price |
| Costs | Commission, spread, swap (incl. triple-Wednesday), tick and lot rounding, exact `SymbolMeta` |
| **Margin & stop-out** | Margin, margin call and broker stop-out are modelled; a simulated stop-out is a first-class outcome |
| Adverse selection | Mid recorded at fill and +1 s / +5 s / +30 s, reported in currency |
| Own market impact | **Explicitly ignored**, valid at ≤ 0.10 lots on these instruments; stated assumption, revisited above that size |

## 9.3 Regime-conditional models

Latency, slippage and rejection are **conditional distributions**, bucketed by observed regime (spread percentile × event rate × realised volatility), calibrated per bucket from Phase 9 demo observation. Independent sampling systematically flatters the backtest on exactly the trades that matter most, because broker latency, slippage and rejects all spike together during bursts.

**Pessimistic mode uses the adverse-regime bucket unconditionally.**

## 9.4 Three modes — only one is decision-grade

| Mode | Queue | Latency | Slippage | Rejects | Regime bucket | Use |
|---|---|---|---|---|---|---|
| Optimistic | front | p10 | none | 0% | benign | upper bound only |
| Base | mid | p50 | expected | observed | matched | development |
| **Pessimistic** | **back** | **p99** | **2×** | **2×** | **adverse** | **the only promotion-grade mode** |

Invariant, tested: `PnL(optimistic) ≥ PnL(base) ≥ PnL(pessimistic)` for any run.

---

# PART 10 — RISK INVARIANTS

Risk sits between strategy and execution. **A strategy submits an `Intent`; risk returns `Approve | Resize | Reject`.** Only the risk engine can construct an `Order`; `IBroker::send` accepts nothing else. Enforced by type, not convention.

## 10.1 Hard invariants

1. **No position may exist without a broker-side protective stop.** Any symbol whose stop/freeze levels prevent attaching a server-side SL is removed from the allowlist at startup. Engine-managed stops are prohibited as the sole protection.
2. `Σ fills == Σ position volume` per ticket and per symbol, always.
3. No order exists without a preceding risk approval.
4. No two orders share a `broker_order_ref`.
5. Reserved exposure ≥ actual exposure, always. `UNKNOWN` orders count fully.
6. Kill state is durable across process death.
7. Live mode requires nine independent conditions (Part 5.5).
8. **Exactly one engine instance may hold the trading lock** — `flock` on a pidfile plus a named mutex, acquired before any adapter attaches. The EA additionally rejects commands from any `session_epoch` other than the one it handshook with.
9. **Effective risk limits are hash-verified at startup**: `config/limits.json` carries a SHA-256 recorded in `RISK_INVARIANTS.md`; a mismatch halts startup unless an `approved_by` field is present. Effective limits and their hash are logged and written to the journal header.
10. A startup **risk self-test** asserts limits are non-zero, internally ordered and mutually consistent (`gross ≥ net`, `burst ≥ sustained`); failure halts startup.

## 10.2 Pre-trade checks (O(1), < 1 µs)

`max_risk_per_trade` · `max_position_per_symbol` · `max_gross_exposure` · `max_net_exposure` · `max_leverage` · **`free_margin` projected under a configured adverse shock (2σ)** · `max_open_orders` · `max_in_flight_per_symbol` · `max_order_requests_per_second` · `max_orders_per_minute` · `max_fills_per_minute` · `max_trades_per_day` · `min_time_between_orders` · `max_spread` · `max_slippage_estimate` · fat-finger bounds · symbol allowlist · session gate · `reduce_only` consistency · **feature warm-mask satisfied** · **self-trade prevention**.

**Self-trade prevention:** on hedging accounts a strategy may not open a position opposing an existing position on the same symbol held by a different strategy, unless `allow_cross_strategy_opposition` is explicitly set. This prevents wash-trade patterns that also attract broker surveillance.

## 10.3 Halt conditions — trip → flatten → disable → **persist** → manual re-arm

| Halt | Trigger |
|---|---|
| Daily loss | realized+unrealized ≤ −`max_daily_loss` |
| Intraday drawdown | equity ≤ peak × (1 − `max_dd`) |
| Consecutive losses | ≥ `max_consecutive_losses` |
| Feed staleness | no event for `max_staleness_ms` |
| Sequence gap | any `GapEvent` |
| **Backpressure** | MD ring ≥ 75% → no new entries; 100% → engine halt |
| Latency breach | sustained p99 stage breach |
| Excessive rejects | reject rate > `max_reject_rate` over a window |
| Disconnection | heartbeat lost or `connected == false` |
| Reconciliation break | position, order **or equity** mismatch |
| Instance-lock loss | lock no longer held |
| Stale-epoch commands | `EXPIRED_ON_RESTART > 0` |
| Disk headroom | free space < `min_free_gb` |
| Manual kill | `ops/KILL` present |
| Control-service kill | authenticated `KILL` intent |

## 10.4 Kill path

The kill file, control intents, instance lock and disk headroom are polled by a **supervisor thread at 10 Hz**, which sets `std::atomic<bool>` flags. The hot path performs a relaxed atomic load per event. **No syscall occurs on the hot path.** Worst-case kill latency of 100 ms is immaterial against a 30–300 ms broker round trip.

Kill state is written to `state/kill.json` with reason, timestamp and tripping metric **before** flattening begins. On startup, if the file exists, the engine starts halted regardless of configuration. Clearing it is a manual, logged human action.

---

# PART 11 — PERSISTENCE AND REPLAY

## 11.1 Storage tiers

| Tier | Store | Content |
|---|---|---|
| Hot | in-memory ring | last N events, dashboard |
| **WAL** | append-only binary, **length-prefixed + CRC32**, pre-faulted mmap segments | every ingress event, verbatim, before processing |
| **Journal** | append-only, **same framing and CRC** | every order/position transition, fill, risk decision, keyed by `corr_id` |
| Snapshot | periodic binary | portfolio + OMS + PMS + book state |
| Cold | Parquet, partitioned `symbol/date` | research |
| Analytics | DuckDB over Parquet | Python |
| Aggregates | SQLite (existing) | per-minute bars, PnL, config — dashboard only |

**Record framing:** `length(4) | type(2) | flags(2) | payload(N) | crc32(4)`. Sequential append cost is unchanged from fixed-stride; variable-length `BookSnapshot` records are now representable. Torn-write recovery truncates at the last valid CRC, for **both** WAL and journal.

**Pre-faulting:** WAL segments are pre-allocated and mapped with `MAP_POPULATE`; without this, first-touch page faults breach the 5 µs p99 write target.

**Header:** every WAL and journal file begins with `run_id`, `run_seed`, `session_epoch`, effective-limits hash, params hash, build hash, toolchain version and clock-calibration record.

## 11.2 Rotation, retention, capacity

Segments rotate at 256 MB or hourly, whichever first; closed segments are zstd-compressed. Retention: 90 days raw, indefinite for Parquet. Free-space precondition is checked at startup and hourly by the supervisor; below `min_free_gb` the engine halts rather than trading unlogged. See Part 16.

## 11.3 Determinism

**Scope, stated explicitly:** determinism is guaranteed for **the same binary on the same architecture**. It is enforced by:

- `-ffp-contract=off`, no `-ffast-math`, no `-march=native` in release builds
- pinned toolchain version, recorded in every WAL/journal header
- deterministic Philox RNG with recorded seed
- total event ordering by `(ts_local_ns, source_priority, seq_source)`
- no wall-clock reads, no unordered-container iteration, no uninitialized reads on any decision path

Cross-machine reproduction runs in CI as a **warning-level** check; divergence is investigated but does not block. Full cross-machine bit-determinism is out of scope for v1.0 (see Appendix C).

**Gate:** same WAL replayed twice → byte-identical journal, 100 consecutive runs. Blocking at Phase 4 and re-verified at Phase 5.

**Live-vs-replay reconciliation:** replay a recorded live session, diff simulated against actual paper fills. The divergence is the simulator's error bar and is reported with every backtest.

---

# PART 12 — LATENCY BUDGET

Twelve instrumentation points, recorded into lock-free HDR histograms, reported as count / min / p50 / p95 / p99 / p99.9 / max.

| # | Stage | Target p99 | Notes |
|---|---|---|---|
| 1 | `md_received` | — | origin (`ts_local_ns`) |
| 2 | `book_updated` | 2 µs | |
| 3 | `features_calculated` | 5 µs | ~20 features |
| 4 | `strategy_completed` | 5 µs | |
| 5 | `risk_completed` | 1 µs | |
| 6 | `order_queued` | 1 µs | |
| **—** | **internal 1→6** | **≤ 15 µs** | **the only part under your control** |
| 7 | `ea_received` | **2 ms** (named pipe) | shm option: 100 µs |
| 8 | `mt5_order_sent` | 1–5 ms | terminal internals |
| 9 | `broker_acknowledged` | **30–300 ms** | **dominates everything** |
| 10 | `fill_received` | +0–200 ms | async `OnTradeTransaction` |
| 11 | `portfolio_updated` | 2 µs | |
| 12 | `persistence_completed` | off hot path | async |

**End-to-end tick→fill: 35–500 ms, of which ≥ 99.9% is stages 8–10.** The named-pipe transport adds ≤ 2 ms — **≤ 0.7% of end-to-end** — which is why it is the default over a fragile shm path.

**This settles the HFT question.** A 15 µs internal path is 0.04% of a 35 ms round trip. **Optimizing below ~50 µs yields no live edge on MT5.** The hot path stays fast so replay runs at 100× and so the crypto adapter has somewhere to be fast — not because it buys MT5 latency.

**Deployment reality:** the p99/p99.9 internal targets require a dedicated core (`isolcpus` + IRQ affinity + pinned thread). **On a shared VPS, scheduler preemption alone exceeds 15 µs**, and p99.9 targets are formally waived there — recorded as an environment attribute alongside every benchmark run, not quietly missed.

Also reported: dropped events, sequence gaps, **ring occupancy p50/p99/max**, order rejects by class, fill ratio, cancellation ratio, `EXPIRED_ON_RESTART` count.

---

# PART 13 — BROKER-RATE DISCOVERY AND THROTTLING

**Discovery (demo only, Phase 10):** supervised probe, never automatic, stepping 0.5 → 1 → 2 → 3 → 5 req/s in 10-minute stages, recording ack latency, reject rate and retcodes, slippage, `TOO_MANY_REQUESTS` incidence, disconnects. **Stop at first degradation.** Output: `config/broker_limits.json`, human-reviewed and committed.

**Adaptive governor (always on):**

```
start:                    1.0 req/s, burst 3
reject (rate class):      rate ×= 0.5, cooldown 60 s
ack p99 > 2× baseline:    rate ×= 0.7
slippage p95 > 2× base:   rate ×= 0.7
disconnect:               rate = 0, halt
15 min clean:             rate = min(rate × 1.2, cap)
hard ceiling:             never exceeds broker_limits.json
```

Live cap ≤ **25% of discovered safe rate**. The governor may only lower autonomously; raising the ceiling requires config change plus human approval.

**Separate budgets — never conflated:**

| Quantity | Internal | Paper/replay | MT5 live |
|---|---|---|---|
| Market events/s | 100,000+ | 100,000+ | 20–1,000 |
| Strategy decisions/s | 100,000+ | 100,000+ | 20–1,000 |
| **Order requests/s** | n/a | unbounded | **1 sustained / 3 burst** |
| Acknowledgements/s | n/a | unbounded | ~1 |
| **Fills/s** | n/a | 50,000+ | **≤ 1** |
| **Round-trip trades/s** | n/a | 25,000+ | **≤ 0.5** |
| Round-trip trades/day | n/a | 100,000+ | **hundreds; thousands unproven** |

---

# PART 14 — CLOCK AND TIME DISCIPLINE

Three clocks exist and must be reconciled explicitly; without this, Part 12's entire budget is unverifiable.

| Clock | Source | Role |
|---|---|---|
| **Local monotonic** | `CLOCK_MONOTONIC` / TSC | **Authoritative for all latency and ordering** |
| Terminal | MT5 server time | broker-timezone, DST-shifting; offset-corrected to UTC ns before use |
| Broker/venue | tick timestamps | best-effort, may be absent or coarse |

**TSC discipline:** at startup the engine verifies `constant_tsc` and `nonstop_tsc`; **if either is absent it falls back to `clock_gettime(CLOCK_MONOTONIC)`** rather than producing wrong measurements. The hot thread is pinned to one core (TSC is not synchronised across all cores on all hardware). TSC→ns calibration runs once at startup and the calibration record is written to every WAL/journal header.

**Offset estimation:** terminal-to-local offset is estimated continuously from heartbeat round-trips (min-filtered over a rolling window) and recorded in the WAL. All timestamps are normalised to **UTC nanoseconds** before entering an `EventHeader`.

**Host requirement:** NTP configured to **slew, never step** (`chrony makestep 0`). A detected wall-clock step raises a `ClockStepEvent`; monotonic time remains authoritative and trading is unaffected, but the interval is flagged for research exclusion.

---

# PART 15 — BACKPRESSURE AND FLOW CONTROL

Every ring is fixed-capacity, and every ring has a declared policy. An undefined policy means the behaviour is discovered in production.

| Ring | Capacity | Policy on full |
|---|---|---|
| **Market data (feed → hot path)** | 65,536 events | **NEVER DROP.** 75% → `BackpressureEvent`, halt new entries. 100% → **engine halt**, interval marked unusable for research |
| **Command (engine → EA)** | 1,024 | **NEVER DROP.** Full → reject the intent at risk with `RESOURCE_EXHAUSTED` |
| **Event (EA → engine)** | 8,192 | **NEVER DROP.** Full → halt |
| WAL / journal writer | 65,536 | Never drop while trading; a full ring halts trading before it drops a record |
| Telemetry | 16,384 | **May drop** with a monotonic `dropped_count`, surfaced and alerted |

**Rationale:** dropping market data silently invalidates the order book and every downstream feature. Converting that into a loud, safe, observable halt is consistent with the governing principle that every ambiguous state resolves toward *not trading*.

**Fan-out topology:** the feed adapter is the single producer into one MD ring consumed by the hot path. The hot path is in turn the single producer into three independent SPSC rings (WAL, journal, telemetry) — no SPMC, no shared consumers, no locks. Head and tail indices are cache-line padded on every ring.

**Sizing basis:** 1,000 events/s burst × 5 s consumer stall tolerance = 5,000 events; rounded to 65,536 (power of two) × 136 B ≈ **8.9 MB**.

---

# PART 16 — CAPACITY AND SIZING

| Quantity | Value |
|---|---|
| Framed event record | 136 B fixed / ~1,060 B book snapshot |
| Events/day, 5 symbols, normal | ~1.0 M |
| Events/day, busy | ~2.5 M |
| **WAL raw/day** | **~350 MB** (peak ~600 MB with snapshots) |
| WAL compressed (zstd-3) | ~70–120 MB/day |
| 14-day recording campaign | ~5 GB raw / ~1.5 GB compressed |
| 90-day retention | ~32 GB raw / ~9 GB compressed |
| Journal/day | < 5 MB |
| Parquet (research copy) | ~40% of compressed WAL |
| **Minimum provisioned disk** | **200 GB** |
| `min_free_gb` halt threshold | 20 GB |
| Fixed RSS budget | ≤ 512 MB (rings 8.9 MB + slab pool 4.3 MB + book/feature state + histograms) |

Rotation at 256 MB or hourly; compression on close; retention enforced by `ops/mme_cleanup.sh`; free space checked at startup and hourly.

---

# PART 17 — FILE MANIFEST

### CREATE

```
include/core/       types fixed_point event event_header ring_buffer spsc_queue
                    clock tsc_calibration symbol_table symbol_meta account_meta
                    config result arena slab_pool backpressure
include/feed/       feed_adapter mt5_pipe_adapter mt5_shm_adapter(optional)
                    crypto_ws_adapter replay_adapter recorder sequence_tracker
                    heartbeat_monitor book_source clock_offset
include/book/       order_book book_l1 book_l2 book_snapshot book_checksum
                    book_reconciler book_rebase
include/features/   feature_engine feature_vector warm_mask rolling welford
                    spread microprice imbalance ofi trade_flow volatility
                    momentum liquidity queue_decay adverse_selection toxicity regime
include/strategy/   strategy intent strategy_context strategy_lifecycle params
                    scalper market_maker
include/risk/       risk_engine limits limits_verifier kill_switch rate_governor
                    exposure margin_projection self_trade_prevention halt_state
include/oms/        order order_state position position_state oms pms
                    order_identity reconciler recovery
include/exec/       broker paper_broker mt5_broker queue_model latency_model
                    fee_model slippage_model margin_model fill_engine
                    sim_mode philox_rng regime_bucket
include/portfolio/  portfolio position_book pnl currency_converter attribution account
include/persist/    wal_writer wal_reader framing journal snapshot
                    parquet_export sqlite_aggregates retention
include/telemetry/  histogram latency_probe counters metrics_exporter stage_ids
include/control/    supervisor control_intent intent_queue instance_lock
src/**              (mirrors every header)
src/apps/           record_main replay_main paper_main live_main bench_main
                    probe_main spike_transport_main
agent/mt5_bridge/   mme_feed.mq5  mme_exec.mq5  mme_protocol.mqh
                    mme_pipe.mqh  mme_guards.mqh  mme_deadman.mqh  mme_symbols.mqh
agent/research/     data/{wal_reader,parquet_build,duckdb_schema}
                    features/validate  labels/triple_barrier
                    validation/{walk_forward,purged_cv,monte_carlo,bootstrap,permutation}
                    costs/cost_model  opt/{bayesian,grid}
                    analysis/{regime,drift,calibration,importance,compare,reconcile}
                    tracking/experiment  export/params  gates/promotion
control_service/    app auth schemas intents rate_limit
config/             symbols aliases limits(+hash) broker_limits latency_profiles
                    fees sim_modes params/<strategy>_<version>.json
tests/cpp/          fixed_point ring_buffer backpressure slab_pool book_l1 book_l2
                    rebase checksum sequence tie_break features warm_mask welford
                    strategy lifecycle risk limits_verifier kill_switch governor
                    margin_projection self_trade oms_states position_states
                    idempotency recovery queue_model latency_model regime_bucket
                    fee_model currency paper_broker partial_fills no_mid_fill
                    mode_ordering portfolio wal framing journal determinism
                    determinism_with_sim no_alloc instance_lock deadman
docs/               ARCHITECTURE_V1.md BUILD_EXECUTION_PLAN.md RISK_INVARIANTS.md
                    CURRENT_CHECKPOINT.md BROKER_POLICY.md LATENCY_BUDGET.md
                    PROMOTION_GATES.md TEST_MATRIX.md CAPACITY_PLAN.md
                    CLOCK_DISCIPLINE.md TRANSPORT_DECISION.md
ops/                KILL arm_live.sh latency_report.sh reconcile_check.sh
                    disk_guard.sh
state/              kill.json halt_reason.json session_epoch run_id  (gitignored)
```

### REPLACE

| Existing | Replaced by | Reason |
|---|---|---|
| `src/main.cpp` | `src/apps/paper_main.cpp` | 30 s poll → event loop |
| `include/market_data/provider.hpp` | `include/feed/feed_adapter.hpp` | `Quote{price}` cannot express a book |
| `src/market_data/file_provider.cpp` | `mt5_pipe_adapter.cpp`, `replay_adapter.cpp` | CSV/mtime polling |
| `src/storage/sqlite_logger.cpp` | `persist/wal_writer.cpp` + `sqlite_aggregates.cpp` | fsync on hot path |
| `src/indicators/*` | `features/*` | recompute → incremental |
| `agent/mt5_bridge/mt5_file_export.mq5` | `mme_feed.mq5` + `mme_exec.mq5` | 5 s timer → OnTick; feed/exec split |
| `src/evaluation/main.cpp` | `src/apps/replay_main.cpp` + Python | ad-hoc SQL → real replay |
| `CMakeLists.txt` | multi-target, sanitizer/bench configs, pinned FP flags | new topology |

### MODIFY

`src/validation/validation.cpp` → `strategy/filters/`, event-driven, no strings · `src/evaluation/metrics.cpp` → `analysis/metrics.cpp` + profit factor, Sortino, capacity · `tests/unit_tests.cpp` → split into `tests/cpp/`, same zero-dependency pattern · `dashboard/*` → new aggregates, Prometheus, **`corr_id` trace view** · `ops/systemd/*` → recorder, engine, control service, exporter · `.github/workflows/ci.yml` → sanitizers, determinism, benchmark regression gate.

### RETIRE

`build/`, `dashboard/.netlify/`, `data/engine.db*` from VCS · `include/market_data/`, `src/market_data/` after Phase 2 · `src/indicators/` after Phase 3 · `model_results.json` **as evidence** · superseded roadmap claims in `README.md`/`CURRENT_STATUS.md`.

---

# PART 18 — IMPLEMENTATION PHASES AND ACCEPTANCE CRITERIA

> Live trading is disabled in every phase. Phase 11 is a specification, not an authorization.

### PHASE 0 — Foundations & Governance

**Objective:** Repo hygiene, build topology, governance docs, broker policy in writing.
**Files:** `CMakeLists.txt`, `.gitignore`, `docs/{ARCHITECTURE_V1,BUILD_EXECUTION_PLAN,RISK_INVARIANTS,CURRENT_CHECKPOINT,BROKER_POLICY,CAPACITY_PLAN,CLOCK_DISCIPLINE}.md`, `config/{symbols,aliases}.json`
**Dependencies:** none · **Tests:** build green Linux+Windows; docs lint · **Benchmark:** clean build < 60 s
**Risks:** Exness declines written policy confirmation → record as a red flag
**Completion:** `build/`/`.netlify/` untracked; sanitizer + bench build types exist; **pinned toolchain and FP flags set**; **written broker policy answer logged**; all seven docs exist
**Checkpoint:** `phase0-foundations`

### PHASE 1 — Core Types, Clock & Event Infrastructure

**Objective:** Zero-allocation primitives with correct time.
**Files:** `include/core/*`, `telemetry/histogram`, `core/tsc_calibration`, `core/slab_pool`, `core/backpressure`
**Tests:** `fixed_point` (no precision loss across all tick sizes), `ring_buffer` under TSan, `backpressure` (policy per ring), `slab_pool` (generation detects stale handle), `no_alloc` (allocator hook), TSC fallback path
**Benchmark:** ring ≥ 10 M msg/s; `EventHeader` == 56 B; fixed event == 128 B (`static_assert`)
**Completion:** benchmarks met; ASan/UBSan/TSan clean; **zero heap allocation after startup, proven by test**; **TSC discipline verified including the fallback**
**Checkpoint:** `phase1-core-types`

### PHASE 2 — Transport Spike, MT5 Feed Bridge & Recorder ⭐ *critical path*

**Objective:** Real tick data flowing and recorded. **Start the data clock.**
**2a — Transport spike (blocking sub-phase):** benchmark named pipe vs shm under Wine, including fence-free publish safety. Named pipe is the default and is retained unless shm demonstrates a material, safe benefit. Output: `docs/TRANSPORT_DECISION.md`.
**Files:** `mme_feed.mq5`, `mme_protocol.mqh`, `mme_pipe.mqh`, `feed/{mt5_pipe_adapter,sequence_tracker,heartbeat_monitor,recorder,book_source,clock_offset}`, `persist/{wal_writer,framing}`, `src/apps/record_main.cpp`
**Tests:** `sequence` (gap detection), `framing`+`wal` (CRC, torn-write truncation), `tie_break` (total order), heartbeat staleness, **session-epoch handshake rejects mismatched account/server/margin-mode**
**Benchmark:** ingress→normalized p99 < 200 µs; WAL write p99 < 5 µs (pre-faulted); 100 k events/s synthetic
**Risks:** DOM absent → *expected*, classify and continue on L1
**Completion:** ≥ 10 events/s/symbol during London/NY on all 5 symbols; **zero sequence gaps over 24 h**; `BookSource` and `AccountMode` classified and logged; transport decision documented; **≥ 14 consecutive days of WAL recorded — the gate for everything downstream**
**Checkpoint:** `phase2-feed-recorder`

### PHASE 3 — Order Book & Feature Engine

**Files:** `include/book/*`, `include/features/*`
**Tests:** `book_l1`/`book_l2`, `rebase` (bounded cost, emits event), `crossed` (tolerance window, not instant halt), `checksum`, `features` vs hand-computed fixtures, `welford` (stability over 10⁸ updates), `warm_mask`, NaN/Inf guard
**Benchmark:** book update p99 < 2 µs; feature vector p99 < 5 µs; ≥ 500 k updates/s
**Completion (conditional on `BookSource`):**
 · `L2_EXCHANGE`/`L3_MBO` → checksum matches source 100% over a session
 · `L1_ONLY`/`DOM_*` → monotonic sequence, non-crossed within tolerance, bid/ask sanity vs `SymbolMeta`, **plus full checksum validation demonstrated against a crypto L2 capture** so the code path is genuinely exercised
 · every feature unit-tested; feature × `BookSource` validity matrix published; **no `std::map`, no allocation, no strings on the hot path — enforced by test**
**Checkpoint:** `phase3-book-features`

### PHASE 4 — Deterministic Replay 🚧 *hard gate*

**Files:** `feed/replay_adapter`, `core/clock` (virtual), `src/apps/replay_main.cpp`
**Tests:** `determinism` — same WAL ×2 → identical journal hash; virtual-clock ordering; speed-independence (1× vs 1000× identical); tie-break under simultaneous timestamps
**Benchmark:** ≥ 100× realtime; ≥ 100 k events/s
**Completion:** **determinism passes 100 consecutive runs. Nothing proceeds until it does.**
**Checkpoint:** `phase4-deterministic-replay`

### PHASE 5 — Paper Execution Simulator

**Files:** `include/exec/*` including `philox_rng`, `regime_bucket`, `margin_model`
**Tests:** `queue_model`, `partial_fills` (property: Σ partials == total, never over-fill, 10⁶ cases), `latency_model` (state sampled at `t_send+λ`), `regime_bucket` (conditional draws), `fee_model` vs `SymbolMeta`, `currency`, **`no_mid_fill`**, `mode_ordering`, margin/stop-out
**Benchmark:** ≥ 50 k simulated fills/s
**Completion:** three modes operational; **no code path can produce a mid-price fill**; property tests pass; **`determinism_with_sim` passes 100 runs — the Phase 4 gate re-verified with the simulator in the loop**; queue-dependent results auto-tagged NOT VALIDATED on L1 sources
**Checkpoint:** `phase5-paper-simulator`

### PHASE 6 — Risk Engine, OMS/PMS & Portfolio

**Files:** `include/risk/*`, `include/oms/*`, `include/portfolio/*`, `control/instance_lock`
**Tests:** one trip-test per §10.3 halt; `limits_verifier` (hash mismatch halts startup; self-test rejects inconsistent limits); `oms_states` and `position_states` (every legal transition; every illegal one rejected); `idempotency`; `recovery` (kill −9 at 20 injection points); `UNKNOWN` resolution; reconciliation break halts without auto-correct; kill-state durability; `instance_lock`; `margin_projection`; `self_trade`; **both margin modes exercised**
**Benchmark:** risk check p99 < 1 µs; OMS/PMS transition < 500 ns
**Completion:** every Part 10 invariant has a passing test; **kill state survives `kill -9`**; **no type-legal path from strategy to broker bypasses risk**; **100% branch coverage on `risk/` and `oms/`**
**Checkpoint:** `phase6-risk-oms`

### PHASE 7 — Strategies (paper only)

**Files:** `include/strategy/*`, `config/params/*`
**Tests:** deterministic in replay; every filter unit-tested; time-stop and hard-stop always honoured; **no-averaging-down**; cooldown; lifecycle FSM; **market maker refuses to start unless `BookSource ≥ L2_EXCHANGE`**; **warm-mask blocks trading on cold features**
**Benchmark:** decision p99 < 5 µs
**Completion:** scalper runs over ≥ 14 days of real recorded data with full pessimistic-mode reports; market maker hard-blocked on MT5
**Checkpoint:** `phase7-strategies`

### PHASE 8 — Python Research Layer & Promotion Gates

**Files:** all `agent/research/**`
**Tests:** leakage (shuffled labels → AUC ≈ 0.5); purge/embargo boundaries; **cost model agrees with the C++ fee model to the cent**; reconciliation harness
**Benchmark:** full walk-forward over 6 months of ticks < 30 min
**Completion:** walk-forward + purged CV + Monte Carlo + bootstrap + permutation operational; **`gates/promotion.py` emits machine-checkable PASS/FAIL** against Part 22
**Checkpoint:** `phase8-research-gates`

### PHASE 9 — MT5 Demo Paper Trading (live feed, simulated fills)

**Files:** `paper_main` against live feed, `control/supervisor`, `ops/systemd/*`, `control_service/*`, telemetry export, dashboard repoint
**Tests:** 5-session soak; disconnect/reconnect; terminal restart; engine restart; **dead-man switch drill**; kill drill; **stale-epoch command rejection after forced restart**; webhook auth (valid/expired/replayed/tampered)
**Benchmark:** internal 1→6 p99 ≤ 15 µs on dedicated cores (p99.9 waived on shared VPS, recorded); zero dropped MD events over 5 sessions
**Completion:** 5 consecutive clean sessions; zero risk breaches; **live-vs-replay fill divergence < 10%**; all 12 latency stages reporting; **regime-conditional latency/reject/slippage buckets calibrated from observed data**; dashboard live with `corr_id` trace
**Checkpoint:** `phase9-mt5-paper`

### PHASE 10 — Rate Discovery + Live Order Path (built, disabled)

**Files:** `mme_exec.mq5`, `mme_guards.mqh`, `mme_deadman.mqh`, `exec/mt5_broker`, `risk/rate_governor`, `src/apps/probe_main.cpp`, `config/broker_limits.json`
**Tests:** **all nine guards individually proven to fail closed**; every retcode class; uncertain-timeout → no duplicate exposure (fault injection); governor backoff; arm-file expiry; **dead-man switch closes positions on engine loss**; `OrderSendAsync` result path via `OnTradeTransaction`
**Completion:** rate discovery complete **on demo**; `broker_limits.json` committed and human-reviewed; every guard fails closed; **live mode remains disabled — no live order has been sent**
**Checkpoint:** `phase10-live-path-disabled`

### PHASE 11 — Controlled Live

**Not authorized by this document.** Requires every Part 22 gate, written approval in `docs/LIVE_APPROVAL.md`, minimum lot, live cap ≤ 25% of discovered safe rate, daily human review, rehearsed shutdown procedure.
**Checkpoint:** `phase11-controlled-live` *(not to be created without approval)*

---

# PART 19 — TEST MATRIX

| Category | Tests | Gate |
|---|---|---|
| Unit | fixed-point, rings, slab pool, book, features, Welford, fees, currency | every phase |
| Property | partial fills, order/position FSMs, queue model, exposure accounting | 5, 6 |
| **Determinism** | same WAL → identical journal ×100, **with and without the simulator** | **4 and 5 (blocking)** |
| Sanitizers | ASan, UBSan, TSan, MSan on the hot path | every phase |
| No-allocation | allocator hook, zero post-init allocations | 1+ |
| Backpressure | each ring's policy under saturation | 1, 9 |
| Fault injection | `kill -9` ×20 points, disconnect, timeout, torn WAL/journal, stale epoch | 2, 6, 9 |
| Risk | one trip-test per limit; limits-hash; fail-closed guards; instance lock | **6, 10 (blocking)** |
| Recovery | restart with open orders, open positions, `UNKNOWN`, both margin modes | 6, 9 |
| Reconciliation | injected position and **equity** mismatch → halt, no auto-correct | 6, 9 |
| Bridge safety | nine guards, dead-man switch, stale-epoch, `OrderSendAsync` path | 10 |
| Integration | replay → paper → simulated fill, end to end | 9 |
| Leakage | shuffled labels → AUC ≈ 0.5; purge/embargo | 8 |
| Statistical | permutation, bootstrap CI, Monte Carlo, multiple-comparison correction | 8 |
| Security | webhook valid/expired/replayed/tampered/unauthorized IP | 9 |
| Soak | 5 consecutive sessions, zero incidents | 9 |
| Regression | benchmark thresholds enforced in CI | every phase |

**Coverage mandate:** `risk/` and `oms/` require 100% branch coverage. Everything else ≥ 80%.

---

# PART 20 — BENCHMARK PLAN

`src/apps/bench_main.cpp`, in CI, failing the build on > 10% regression. Every result records the environment profile (dedicated cores vs shared VPS).

| Benchmark | Threshold |
|---|---|
| Ring throughput | ≥ 10 M msg/s |
| Event normalization | ≥ 1 M events/s |
| Book update (L1 / L2×10) | ≤ 0.5 µs / ≤ 2 µs p99 |
| Book rebase | ≤ 50 µs, bounded |
| Feature vector | ≤ 5 µs p99 |
| Strategy decision | ≤ 5 µs p99 |
| Risk check | ≤ 1 µs p99 |
| OMS/PMS transition | ≤ 500 ns p99 |
| **Internal 1→6** | **≤ 15 µs p99** (dedicated cores; p99.9 waived on shared VPS) |
| WAL write (pre-faulted) | ≤ 5 µs p99 |
| Pipe transport round trip | ≤ 2 ms p99 |
| Replay speed | ≥ 100× realtime |
| Simulated fills | ≥ 50 k/s |
| Steady-state allocations | **exactly 0** |
| Peak RSS | ≤ 512 MB, bounded |

---

# PART 21 — FAILURE AND RECOVERY SCENARIOS

| Scenario | Detection | Response | Recovery |
|---|---|---|---|
| Feed stall | heartbeat timeout | halt new entries; broker-side SL protects open positions | resume on N clean heartbeats |
| Sequence gap | `GapEvent` | halt; mark interval unusable | resync from snapshot |
| **MD ring saturation** | occupancy ≥ 75% / 100% | halt entries / halt engine | drain, resume, interval flagged |
| Crossed book | bid ≥ ask beyond tolerance | reject snapshot; halt symbol | next clean snapshot |
| **Engine death** | EA `engine_hb_age_ms` | **dead-man switch closes all magic-tagged positions** | systemd restart → journal replay → reconcile |
| MT5 terminal crash | heartbeat loss | halt; **no resends** | restart → handshake → reconcile |
| Broker disconnect | `connected == false` | halt; positions remain broker-side with SL | reconcile on reconnect |
| **Stale commands after restart** | `session_epoch` mismatch | **discard, count, alert; never execute** | halt pending human review |
| Order timeout | no ack in window | → `UNKNOWN`; never blind-resend | reconcile by `broker_order_ref` |
| Partial fill then disconnect | reconciliation | halt | resolve from broker state |
| Position/equity mismatch | 5 s reconcile | **halt, alert, no auto-correct** | human resolution |
| Orphan order/position | magic-number scan | halt, alert | human resolution |
| Reject storm | reject-rate window | governor backoff; halt at threshold | manual re-arm |
| Latency degradation | p99 breach | governor backoff; halt if sustained | auto-recover after clean period |
| Daily loss / drawdown | continuous | flatten, disable, persist kill | **manual re-arm only** |
| Margin call / stop-out risk | free-margin projection | reject new; alert | — |
| Disk headroom | supervisor, hourly | halt (never trade unlogged) | `ops/mme_cleanup.sh` |
| Clock step | monotonic vs wall divergence | log, flag interval; monotonic authoritative | — |
| Two instances | instance lock | second refuses to start | — |
| Config/params/limits corruption | schema + SHA-256 | refuse to start | fix and re-approve |
| Kill file at boot | startup check | start halted | manual clear |

**Governing principle: every ambiguous state resolves toward *not trading*.**
**Highest-priority alarm: `position_open AND engine_halted`.** It pages immediately; the dead-man switch is the automated backstop, human response is the escalation.

---

# PART 22 — PROMOTION GATES

**Gate A — Research → Replay:** ≥ 3 months **real** recorded tick/L2 data (never synthetic) · ≥ 1,000 in-sample / ≥ 500 out-of-sample trades · positive net expectancy after full modelled costs · no walk-forward fold worse than −50% of mean · permutation p < 0.01 · bootstrap 95% CI on expectancy excludes zero · no leakage · configurations tested disclosed with multiple-comparison correction.

**Gate B — Replay → MT5 Demo Paper:** pessimistic mode positive · zero risk-invariant violations · **determinism passes with the simulator in the loop** · restart recovery passes at all injection points · latency within budget for the environment profile · sim-vs-replay divergence < 10% · strategy capacity estimated.

**Gate C — Demo → Controlled Live:** ≥ 2,000 demo trades over ≥ 20 sessions across ≥ 3 volatility regimes · positive net expectancy after **real observed** spread, commission and slippage · no session worse than −2× mean daily loss · reject rate < 2% · disconnects < 1/session · **zero reconciliation breaks** · **zero stale-epoch command incidents** · drawdown within approved range · adverse-selection cost quantified and accepted · latency-adjusted alpha decay measured · **written broker confirmation** the pattern is permitted · **written human approval** · minimum lot · live cap ≤ 25% of discovered safe rate · rehearsed shutdown.

**Metrics at every gate:** net expectancy/trade · profit factor · Sharpe · Sortino · max drawdown · win rate · avg win · avg loss · turnover · fill ratio · cancellation ratio · adverse-selection cost · latency-adjusted alpha decay · capacity.

**Positive gross PnL is never sufficient at any gate.**

---

# PART 23 — FINAL VERDICT

| Question | Answer |
|---|---|
| C++ engine > 10 events/s? | **Yes — four orders of magnitude above** |
| Paper/replay > 10 fills/s? | **Yes**, 50,000+/s |
| Exness/MT5 > 10 confirmed live trades/s? | **No.** ~1–3/s peak. Not a code-quality problem |
| Thousands of live trades/day technically possible? | **Yes mechanically** (0.07 trades/s) |
| Economically viable? | **Not as specified.** "A few cents" is below the $0.09–0.37 round-trip floor. Requires larger per-trade edge (fewer trades) or a crypto venue |
| Evidence before live? | All three gates in Part 22 |

**Nanosecond HFT is not achievable through MT5, Exness, TradingView, retail internet, or any VPS. That barrier is physics and exchange membership, not software.** This document targets 1–100 ms live execution and microsecond internal processing — the correct honest ceiling for this infrastructure.

---
---

# APPENDIX A — CHANGE LOG (v0.9 → v1.0)

## Critical findings merged (7 of 7)

| ID | Change | Sections rewritten |
|---|---|---|
| **C-1** | Account margin mode (netting/hedging) made first-class. Positions keyed by `position_ticket` with a derived per-symbol aggregate. Guard #5 added. Engine refuses to start on mode disagreement | 0.4, 5.3, 5.5, 5.9, **8.1**, 8.6, 10, 18-P6 |
| **C-2** | Backpressure policy declared per ring. MD ring never drops; 75% → halt entries, 100% → engine halt. `BackpressureEvent` added. Ring occupancy in telemetry | 3, 7.3, 10.3, **15 (new)**, 19, 21 |
| **C-3** | Philox counter-based PRNG keyed on `(run_seed, corr_id, stage_id)`; seed in journal header. Determinism gate re-run at Phase 5 exit | **9.1**, 11.3, 18-P5, 19, 22-B |
| **C-4** | Split into `mme_feed.mq5` (chart A, no order code) and `mme_exec.mq5` (chart B), `OrderSendAsync` mandated | **3, 5.1**, 12, 17, 18-P2/P10 |
| **C-5** | Broker-side protective stop is Invariant #1; symbols that cannot carry one are untradable. EA dead-man switch closes magic-tagged positions on engine heartbeat loss | **5.6**, 8.5, **10.1**, 18-P10, 21 |
| **C-6** | `session_epoch` handshake; reader resyncs to writer head; stale-epoch records discarded, counted, alerted, never executed; non-zero count is a startup halt | **5.3**, 5.4, 10.3, 18-P2, 21, 22-C |
| **C-7** | Two event classes: 128 B fixed (56 B header + ≤ 72 B payload, `static_assert`ed) and slab-backed `BookSnapshot` (4,096 × 1,040 B pool). WAL/journal become length-prefixed + CRC32 | **7.1–7.3**, **11.1**, 18-P1/P2 |

## High findings — disposition

| ID | Decision | Justification |
|---|---|---|
| H-1 Clock discipline | **APPROVE** | Without invariant-TSC verification and core pinning the entire Part 12 budget is unverifiable — measurement correctness is a precondition for every other latency claim |
| H-2 FP determinism | **APPROVE (scoped)** | Pinned FP flags and toolchain adopted; determinism guaranteed for same-binary/same-arch, cross-machine reduced to a CI warning — full bit-portability is disproportionate cost for a single-host system |
| H-3 Event tie-breaking | **APPROVE** | Determinism requires a documented total order; without it the Phase 4 gate passes by luck of scheduling |
| H-4 Named pipes default | **APPROVE** | Removes the MQL5 DLL-import and fence-free-publish risk for ≤ 0.7% of end-to-end latency — the highest reliability-per-effort change in the review |
| H-5 Kill check off hot path | **APPROVE** | Resolves a direct contradiction with the zero-syscall invariant and removes an unbounded-latency source at no meaningful cost to kill responsiveness |
| H-6 Order identity split | **APPROVE** | Determinism and cross-restart uniqueness were mutually exclusive as written; the split satisfies both and makes MT5 identifier encoding explicit |
| H-7 Position modify FSM | **APPROVE** | Break-even and trailing stops are explicitly required of the scalper and had no representation in the model |
| H-8 Signed risk limits | **APPROVE** | The document's strongest safety claim was asserted in prose and unenforced in design |
| H-9 Single-instance lock | **APPROVE** | Two concurrent instances silently double every limit — a catastrophic failure prevented by a few lines of design |
| H-10 Feature warm-up | **APPROVE** | Cold accumulators after a restart is exactly the post-incident moment when bad decisions are least affordable |
| H-11 Regime-conditional models | **APPROVE** | Independent sampling flatters the backtest specifically on the trades that matter; this is the largest remaining optimism bias |
| H-12 Conditional Phase-3 gate | **APPROVE** | An unmeetable gate gets waived, and a waived gate teaches the team that gates are negotiable |

**12 approved, 0 rejected.** H-2 adopted with a narrowed scope, documented in Appendix C.

## Medium findings merged (16 of 17)

M-1 currency conversion · M-2 book rebase policy + event · M-3 Welford/Kahan accumulators · M-4 time-vs-event window declaration · M-5 margin projection · M-6 simulator margin/stop-out · M-7 multi-strategy arbitration + self-trade prevention · M-8 strategy lifecycle FSM · M-9 WAL sizing/rotation/retention (Part 16) · M-10 journal torn-write framing · M-11 protocol handshake + packing contract · M-12 fill-ordering clarification · M-14 mmap pre-faulting · M-15 VPS latency realism · M-16 crossed-book tolerance · M-17 double-buffered config reload.

**M-13 (symbol sharding) NOT merged** — deferred to Appendix C as a documented scaling path; merging it now would add threading complexity with no benefit at 5 symbols.

## Low findings merged (4 of 6, all free)

L-2 NaN/Inf guard · L-3 EA-side symbol allowlist (guard #9) · L-5 stated market-impact assumption · L-6 `corr_id` dashboard trace view.

**L-1 explicitly rejected** — virtual dispatch is retained; CRTP conversion costs readability for no measurable benefit at a 5 µs budget.
**L-4 not merged** — unconditional feature computation is correct at this scale.

---

# APPENDIX B — CONSISTENCY AUDIT

| Check | Result |
|---|---|
| **No contradictions** | PASS — three v0.9 contradictions resolved: kill-file syscall vs zero-syscall invariant (H-5); probabilistic simulator vs byte-identical replay (C-3); fixed-128 B union vs `BookSnapshot` size (C-7) |
| **No duplicated requirements** | PASS — backpressure appears once in Part 15, referenced from 3/10/19/21. Clock discipline once in Part 14, referenced from 7/12. Capacity once in Part 16 |
| **No conflicting state machines** | PASS — Order FSM and Position FSM are disjoint, linked only through `Fill → position_ticket`. `UNKNOWN` (order) and `RECONCILE_UNKNOWN` (position) resolve independently. Both terminal sets are closed |
| **No conflicting message formats** | PASS — all records carry `magic\|ver\|epoch\|seq`. `CmdRecord` carries `run_id` + `logical_order_id` matching Part 8.2. `HbRecord` carries `margin_mode` matching Part 8.1 and `engine_hb_age_ms` matching Part 5.6. Handshake fields match the guard list |
| **No impossible latency targets** | PASS — stage 7 corrected 100 µs → 2 ms for the named-pipe default. Internal 1→6 ≤ 15 µs is conditioned on dedicated cores, with p99.9 formally waived on shared VPS. WAL 5 µs p99 is achievable given mandated pre-faulting |
| **No impossible memory layouts** | PASS — `EventHeader` = 56 B (`static_assert`). Fixed events ≤ 128 B, largest payload (`Fill`, 68 B) inside the 72 B budget. `BookSnapshot` moved to a slab pool. Total fixed memory: rings 8.9 MB + slabs 4.3 MB + book/feature state + histograms, inside the 512 MB RSS bound |
| **No replay inconsistencies** | PASS — total order defined (H-3). RNG deterministic and seeded (C-3). FP semantics pinned and scoped (H-2). Timer generation specified. Gate re-run at Phase 5 with the simulator in the loop |
| **No MT5 inconsistencies** | PASS — two EAs with disjoint responsibilities. `OrderSendAsync` consistent with the async `OnTradeTransaction` ack path already in the schemas. Margin mode consistent across handshake, guards, OMS and portfolio. DOM verdict (0.5) consistent with the conditional Phase 3 gate (H-12) and the market-maker admission check |
| **No OMS inconsistencies** | PASS — identity model satisfies determinism and uniqueness simultaneously. Idempotency, retry and `UNKNOWN` rules are mutually consistent. Position modify has an explicit path. Reconciliation keys match the margin mode. Fill ordering rule stated once and applied consistently |
| **No risk inconsistencies** | PASS — ten invariants are non-overlapping. Every halt condition in 10.3 has a detection source in Part 21 and a test in Part 19. Nine live guards enumerated identically in 5.5 and 10.1. Broker-side-SL invariant is consistent with 5.7 validation and the 5.6 dead-man switch |

**Unresolved Critical findings: 0.**

---

# APPENDIX C — REMAINING KNOWN LIMITATIONS

1. **No demonstrated edge.** The strategy hypothesis is unvalidated. This architecture is a platform for finding out honestly, not evidence that anything works.
2. **Economics unresolved at the stated target.** "A few cents per winner" remains below the cost floor. This is a business decision the architecture cannot solve; it only measures it accurately.
3. **No genuine depth on any MT5 target instrument.** Market making, true queue position and OFI on real depth remain paper-only until a crypto adapter exists.
4. **Cross-machine bit-determinism out of scope** (H-2 scoping). Same-binary/same-arch only; CI cross-machine check is warning-level.
5. **Single hot thread** (M-13 deferred). Head-of-line blocking across symbols is acceptable at 5 symbols; symbol-sharded threads are the documented scaling path, and cross-symbol strategies could not span shards.
6. **Shared-VPS p99.9 targets waived** (M-15). Dedicated cores are required for the full latency budget.
7. **Queue model uncalibrated on L1 sources.** Results are auto-tagged NOT VALIDATED and are inadmissible for promotion — the limitation is contained, not eliminated.
8. **Broker policy risk is unmitigated by engineering.** Thousands of short-hold trades/day matches retail surveillance profiles. Written broker confirmation is a Gate C requirement precisely because no code change addresses it.
9. **Crypto adapter is specified but unbuilt.** The venue where the thousands-per-day target is physically and economically possible is a post-v1.0 phase.
10. **`ts_broker_ns` is best-effort.** Many MT5 feeds provide only second or millisecond resolution; latency attribution upstream of the terminal is correspondingly coarse.

---

# APPENDIX D — IMPLEMENTATION PHASE LIST

| # | Phase | Checkpoint | Gate character |
|---|---|---|---|
| 0 | Foundations & Governance | `phase0-foundations` | Written broker policy |
| 1 | Core Types, Clock & Event Infrastructure | `phase1-core-types` | Zero-allocation proof |
| 2 | **Transport Spike, MT5 Feed Bridge & Recorder** | `phase2-feed-recorder` | **14 days of real WAL — critical path** |
| 3 | Order Book & Feature Engine | `phase3-book-features` | Conditional on `BookSource` |
| 4 | **Deterministic Replay** | `phase4-deterministic-replay` | **Blocking: 100 identical runs** |
| 5 | Paper Execution Simulator | `phase5-paper-simulator` | **Determinism re-verified with sim** |
| 6 | Risk Engine, OMS/PMS & Portfolio | `phase6-risk-oms` | **100% branch coverage on risk/oms** |
| 7 | Strategies (paper only) | `phase7-strategies` | MM blocked on MT5 by `BookSource` |
| 8 | Python Research & Promotion Gates | `phase8-research-gates` | Machine-checkable PASS/FAIL |
| 9 | MT5 Demo Paper Trading | `phase9-mt5-paper` | 5 clean sessions, < 10% divergence |
| 10 | Rate Discovery + Live Path (disabled) | `phase10-live-path-disabled` | **All 9 guards fail closed** |
| 11 | Controlled Live | *(withheld)* | **Requires written human approval** |

---

# APPENDIX E — FREEZE RECORD

All seven Critical findings are integrated into the body of the architecture, not appended. All twelve High findings are approved and merged, one with documented scope narrowing. Sixteen of seventeen Medium findings are merged; the exception is deferred with justification. Four free Low findings are merged; two are explicitly rejected.

The consistency audit finds no contradictions, no duplicated requirements, no conflicting state machines or message formats, no impossible latency targets or memory layouts, and no replay, MT5, OMS or risk inconsistencies. Three internal contradictions present in v0.9 are resolved.

The core architecture is unchanged: risk-before-execution enforced by type, one codebase across replay/paper/live, determinism as a blocking gate, WAL-first recording, `BookSource`-gated strategy admission, pessimistic-mode-only promotion, and the honest economic verdict. No major component was replaced.

`docs/RISK_INVARIANTS.md` freezes harder than the rest — changes to it require the same written approval as a live-trading decision.

Two items carry into implementation regardless: **Phase 2 begins with the transport spike** before the rest of Phase 2 is committed, and **the Phase 4 determinism gate re-runs as a Phase 5 exit criterion**.

## ARCHITECTURE VERSION 1.0 APPROVED FOR IMPLEMENTATION
