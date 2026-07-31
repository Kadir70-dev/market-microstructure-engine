# Spike → Engine Migration

**Status:** ARCHIVED SPIKE · MIGRATION PLANNED
**Date:** 2026-07-30
**Subject:** `experiments/mt5_demo_spike/`
**Authority:** subordinate to `docs/ARCHITECTURE_V1.md` (frozen v1.0) and
`docs/ARCHITECTURE_V1_ERRATA.md`

> **This document proposes zero architecture changes.** `docs/RISK_INVARIANTS.md`
> states that amendments to Architecture Version 1.0 require the same written
> approval as a live-trading decision. None is requested here. Every finding
> below is already covered by the frozen document; the spike *validated* the
> architecture, it did not outgrow it.

---

## 1. Archival declaration

`experiments/mt5_demo_spike/` is **archived and reference-only as of
2026-07-30**. It is complete against its objective and receives no further
development.

### 1.1 What it was

A deliberately throwaway MQL5 expert advisor, built to answer one question:
*can a correctly protected order be placed on an MT5 demo account, end to end?*
It answered yes. Its own file header states it plainly — "THROWAWAY DEMO SPIKE.
Not part of the institutional engine… It is not an edge."

Files, all frozen at their 2026-07-30 state:

| File | Role |
|---|---|
| `mme_spike_scalper.mq5` | The EA — signal, order path, lifecycle |
| `mme_spike_guards.mqh` | Identity, symbol, lot, filling, stops, autotrading gates |
| `mme_spike_log.mqh` | Single-line tagged logging + retcode names |
| `mme_spike_backoff.mqh` | Rejection backoff state machine |
| `mme_spike_backoff_test.mq5` | 26 deterministic offline assertions |

### 1.2 Why it is preserved rather than deleted

1. **It is the evidence trail for four frozen architectural decisions.** The
   claims in §2 are only checkable against the code that produced them.
2. **Its two failures became test cases.** §5 turns them into concrete Phase 6
   and Phase 10 requirements. The failing code is the specification for those
   tests.
3. **It is a bounded, honest artifact.** It states its own limitations in its
   header, refuses non-demo accounts structurally, and reports a net loss
   without dressing it up. That is worth keeping as a reference for tone.
4. **Deleting it would destroy reproducibility** of the 2026-07-30 session
   without saving anything meaningful — five files, ~40 KB.

### 1.3 Binding constraint on dependency

> **No production engine code may depend on, include, link against, or be
> ported from `experiments/mt5_demo_spike/`.**

The spike is MQL5. Its nearest architectural relative is `mme_exec.mq5`, which
belongs to **Architecture Part 18 Phase 10** and is specified as *built and
disabled*. The engine's execution path does not exist yet and, when it does,
will be written against Parts 5, 8 and 10 — not adapted from this code.

**The spike transfers zero lines of code.** It transfers validated evidence
(§2), partial concepts requiring redesign (§3), and test cases (§5).

---

## 2. Validated findings

Empirical results from the 2026-07-30 demo session. 12 completed round trips,
306 order results, account `474128546` on `Exness-MT5Trial15`, 0.01 lot
throughout.

### 2.1 Broker-side protection survives engine death — Invariant #1 confirmed

**The strongest result of the exercise.** Part 10.1 Invariant 1 states: *"No
position may exist without a broker-side protective stop… Engine-managed stops
are prohibited as the sole protection."*

At 20:17:46 the EA opened a SELL at 1.15115 with a server-side stop at 1.15215.
The host machine then suspended for **2 hours 8 minutes**. During that window
price moved through the stop, and the broker closed the position unaided. On
resync at 22:27:01 the terminal reported `0 positions, 0 orders`.

The engine was dead. The protection was not. This is Invariant 1 working
exactly as designed, observed rather than argued.

### 2.2 Identity gating fails closed

All six init guards rejected every mismatch tested — wrong account, wrong
server, non-demo mode, disallowed symbol, lot ≠ broker minimum, SL/TP inside
`stops_level`. 100% of mismatches produced `INIT_FAILED` with a named reason.
No input, flag, or recompile path bypassed the demo check.

Maps to Part 5.5 guards 2, 3, 4 and 9 (see §3.5 for the five that are missing).

### 2.3 Unbounded retry is a real failure mode, not a theoretical one

A persistent broker rejection produced **275 rejected orders**, at one point six
in six seconds. The per-minute rate limiter provided no protection because it
only advanced on *accepted* orders.

This validates three separate frozen provisions: Part 5.8's retcode classes,
Part 13's rate governor, and Part 10.3's "excessive rejects" halt condition.

### 2.4 Order-path mechanics

| Property | Result |
|---|---|
| Retcode distribution | 275 `NO_MONEY` · 25 `DONE` · 6 `CONNECTION` |
| Fills carrying broker SL **and** TP | 12 of 12 |
| Duplicate positions opened | 0 across 306 order results |
| Lot-cap violations | 0 |
| Exits by time stop | 11 of 12 |
| Exits by broker stop-loss | 1 of 12 |
| Net result | **−1.43 USD** over 12 trades |

The net loss is not a defect. The strategy is a fixed-threshold momentum
scalper with symmetric 100/100-point stops, declared in its own header as not
an edge. Average −0.12/trade against ~0.08 round-trip spread cost is exactly
what a zero-edge strategy paying the spread produces.

---

## 3. Reusable concepts

Nothing in this section is reusable *as code*. Each item is a concept whose
architectural home already exists and is specified more strictly than the spike
implements it.

### 3.1 Broker-side protection → Part 10.1, Part 5.7

**Status: validated, already frozen.** No design work needed. Phase 6 should
additionally implement the startup allowlist filter — Part 10.1 requires that
*"any symbol whose stop/freeze levels prevent attaching a server-side SL is
removed from the allowlist at startup."* The spike checks this per-attach; the
engine must check it per-symbol at boot.

### 3.2 Rejection backoff → Part 5.8, Part 13, Part 10.3

**Status: concept only. The spike's classification is wrong.**

The spike applies one linear backoff to every failure. Part 5.8 specifies five
classes with different actions:

| Class | Part 5.8 action | Spike behaviour |
|---|---|---|
| Success | Apply to OMS | ✅ matches |
| Retryable (`REQUOTE`, `PRICE_CHANGED`, `PRICE_OFF`, `TIMEOUT`) | Backoff, revalidate, retry **under the same `logical_order_id`** | ⚠️ backs off, but has no order identity |
| Rate (`TOO_MANY_REQUESTS`, `LIMIT_ORDERS`, `LIMIT_VOLUME`) | Immediate governor backoff | ❌ not distinguished |
| Fatal (`INVALID_*`, **`NO_MONEY`**, `TRADE_DISABLED`, `MARKET_CLOSED`, `FROZEN`) | **Terminal reject, no retry** | ❌ **retries with backoff — architecturally wrong** |
| Uncertain (timeout, no ack) | **Never blind-resend** → `UNKNOWN`, reconcile by `broker_order_ref` | ❌ blind-resends |

The spike's own incident is the clearest illustration: it treated `NO_MONEY` as
retryable and issued 275 attempts. Under Part 5.8 that is a Fatal class and
should have produced **exactly one**.

What transfers: the *evidence* that this matters, and the shape of the state
machine (linear backoff, consecutive-failure ceiling, success resets). Not the
classification, not the code.

### 3.3 Structured logging → Part 11.1 journal

**Status: discipline only.**

`SpikeLog` calls `PrintFormat`. Part 3 forbids strings on the hot path
outright — *"no heap alloc · no syscalls · no locks · no strings."* Part 11.1
mandates a binary framed journal (`length(4) | type(2) | flags(2) | payload(N)
| crc32(4)`) keyed by `corr_id`, with the frame layout fixed by
`ARCHITECTURE_V1_ERRATA.md`.

What transfers is the *habit*: one greppable line per state transition, one tag
per event class, human-readable retcode names rather than bare integers. That
habit belongs in the **journal decoder CLI**, not the engine.

### 3.4 Position adoption → Part 8.6 reconciliation

**Status: cautionary. Do not reproduce.**

The spike demonstrated two behaviours that look similar and are not:

**Correct.** A restarted instance found an inherited position by magic number
and managed it to a clean time-stop exit (ticket `2076978671`, opened by the
23:07:21 instance, closed by the 23:08:18 instance).

**Incorrect, and the more important finding.** When the broker's stop-loss
executed while the terminal was offline, `OnTradeTransaction` never fired. The
EA's counters — `g_last_close_time`, `g_losses_today`, `g_gross_today`,
`g_consec_losses` — never registered the loss. Cooldown was skipped and daily
P&L understated. **The EA silently continued trading on a false model of its
own state.**

Part 8.6 and Part 10.3 require a reconciliation break — position, order **or
equity** mismatch — to **halt without auto-correct**. The spike silently
auto-corrected. This is precisely the failure the architecture forbids, and it
is now a Phase 6 test case (§5.3).

### 3.5 Safety guards → Part 5.5 nine live guards

**Status: 4 of 9 present.**

| # | Part 5.5 guard | Spike |
|---:|---|---|
| 1 | Compile-time `#ifdef MME_ALLOW_LIVE` | ❌ |
| 2 | `ACCOUNT_TRADE_MODE == DEMO` | ✅ re-checked every tick |
| 3 | Account allowlist | ✅ |
| 4 | Server allowlist | ✅ |
| 5 | Margin-mode agreement | ❌ |
| 6 | Arm file + token, mtime < 8 h | ❌ |
| 7 | Runtime `--live` flag | ❌ |
| 8 | Instance lock held | ❌ |
| 9 | Symbol allowlist at EA level | ✅ |

The four present ones fail closed correctly. The five absent ones are exactly
the ones that gate *live* trading — consistent with a demo-only spike, and the
reason it can never be promoted.

### 3.6 Kill switch → Part 10.4

**Status: weaker than specification.**

The spike polls a kill file on every tick and on a 1 s timer. Part 10.4
requires a dedicated **supervisor thread at 10 Hz** setting `std::atomic<bool>`
flags, with the hot path performing a relaxed atomic load and **no syscall on
the hot path**. The spike performs a filesystem existence check per tick — a
syscall in the hot path.

More seriously, spike kill state is in-memory only. Part 10.1 Invariant 6
requires kill state durable across process death, written to `state/kill.json`
**before** flattening begins, with the engine starting halted if the file
exists. The spike would fail Invariant 6.

### 3.7 Observability → Part 12

**Status: not transferable.** Part 12 requires twelve instrumented stages in
lock-free HDR histograms reporting count/min/p50/p95/p99/p99.9/max. The spike
has no timing instrumentation of any kind.

---

## 4. Non-transferable spike code

Each was a deliberate simplification. Each is explicitly contradicted by the
frozen architecture. None may appear in engine code.

| Spike simplification | Contradicts |
|---|---|
| `double` prices throughout | Part 5.9 — *"Prices are `int64` ticks everywhere on the hot path. Doubles appear only at the display boundary."* |
| Single hardcoded symbol; at most one position ever | Part 3 — multi-symbol portfolio, netting/hedging aware; Part 8.1 margin mode first-class |
| Fixed lot, never sized | Part 10 — *"A strategy submits an `Intent`; risk returns `Approve \| Resize \| Reject`. Only the risk engine can construct an `Order`."* |
| All state in memory, lost on restart | Part 11.1 — WAL + journal + snapshot tiers; Invariant 6 durable kill |
| `PrintFormat` string logging | Part 3 — no strings on the hot path |
| Account/server/symbol as EA inputs | Part 10.1 §9 — SHA-256 hash-verified `config/limits.json`; mismatch halts startup |
| Blind close retry | Part 5.8 — Uncertain → `UNKNOWN`, **never blind-resend**; reconcile by `broker_order_ref` |
| No `corr_id`, no `logical_order_id` | Part 5.8 retry identity; Part 11.1 journal keying |
| Counters never reconciled against broker truth | Part 8.6 reconciliation; Invariant 2 (`Σ fills == Σ position volume`) |
| Rate counters advance only on success | Part 10.2 — `max_order_requests_per_second`, `max_orders_per_minute` are pre-trade checks |
| Time stop as de-facto exit (11 of 12) | Phase 7 strategy concern; no edge |
| Fixed-threshold momentum signal | Self-declared "not an edge"; net −1.43 over 12 trades |
| `OnTick`-driven filesystem kill check | Part 10.4 — supervisor thread, no syscall on hot path |

---

## 5. Test cases derived from spike failures

The spike's value is largely here. Three concrete cases, sourced from observed
behaviour, slotting into existing Part 18 test families.

### 5.1 Fatal retcode produces exactly one attempt — Phase 10

Given a broker returning a Fatal-class retcode (`NO_MONEY`, `INVALID_*`,
`TRADE_DISABLED`, `MARKET_CLOSED`, `FROZEN`), the engine issues **exactly one**
order request and no retry. *Observed failure: 275 requests against
`NO_MONEY`.* Joins Part 18 Phase 10's "every retcode class" requirement.

### 5.2 Rate limiting counts attempts, not successes — Phase 6

Rejected order requests count toward `max_order_requests_per_second` and
`max_orders_per_minute`. *Observed failure: the spike's per-minute limit of 3
permitted 275 requests because it incremented only after acceptance.* Joins the
Part 10.2 pre-trade check tests.

### 5.3 Deal executed during disconnect halts on resync — Phase 6

Inject a fill or stop-out that completes while the transport is down. On
reconnect, reconciliation must detect the position/equity mismatch and **halt
without auto-correct**. *Observed failure: the spike silently re-adopted, kept
trading, and under-reported daily P&L.* Joins the existing "`kill -9` at 20
injection points" fault-injection family and the Part 10.3 reconciliation-break
halt.

---

## 6. Migration roadmap

### 6.1 Phase numbering

Two schemes exist in this repository and they conflict. **Architecture Part 18
numbering is authoritative.**

| Source | "Phase 3" |
|---|---|
| `README.md`, `CLAUDE.md` roadmap | Edge research |
| **`docs/ARCHITECTURE_V1.md` Part 18** | **Order Book & Feature Engine** |

Edge research maps onto **Phases 3, 4 and 8** plus **Part 22 Gate A**:

| Work | Architecture home |
|---|---|
| Feature extraction | Phase 3 — Book & Feature Engine |
| Replay engine | **Phase 4 — Deterministic Replay** 🚧 hard gate |
| Predictive models, validation, walk-forward, significance | Phase 8 — Python Research Layer & Promotion Gates |
| Edge measurement | Part 22 Gate A |

### 6.2 The binding constraint

`docs/CURRENT_CHECKPOINT.md` records that `phase2-feed-recorder` is incomplete:
the 24-hour zero-gap validation and 14-day WAL recording gate **have not
started**. Verified against the filesystem on 2026-07-30:

- Zero WAL segments exist anywhere in the repository
- `data/engine.db` last written 2026-07-27 — the 27 synthetic-feed cycles
- `MQL5\Files\` contains no quote export
- No recorder process running
- `mme_feed` failed init with code 1 repeatedly (16:47 → 17:03)

**Days of real recorded data currently held: 0.**

Architecture Part 18 Phase 2 states the objective as *"Start the data clock"*
and its completion criterion as *"≥ 14 consecutive days of WAL recorded — the
gate for everything downstream."* Part 22 Gate A then requires *"≥ 3 months
real recorded tick/L2 data (never synthetic)."*

No engineering effort shortens either clock.

### 6.3 Transition actions

1. Archive the spike per §1 — **preserve the files**, mark reference-only.
2. Remove the spike EA from the EURUSDm chart. It trades continuously and its
   counters desync on any disconnect (§3.4).
3. Leave `mme_feed` as the only EA on the terminal, and fix its init failure.
4. Record §5's three test cases against Phases 6 and 10.
5. Refresh `CLAUDE.md`, which describes a three-table SQLite engine and omits
   `core/`, `feed/`, `persist/`, `telemetry/`, the WAL, the recorder and the
   MT5 pipe protocol — all of which exist.

---

## 7. Engineering milestones

Effort is engineering-days for one developer. **Calendar** is wall-clock and
dominates.

| # | Milestone | Phase | Depends on | Effort | Calendar |
|---|---|---|---|---:|---:|
| **M0** | Archive spike; fix `mme_feed` init; refresh `CLAUDE.md` | — | none | 1–2 d | 2 d |
| **M1** | **Start the data clock** — recorder supervised and alerting | 2 | M0 | 2–3 d | **≥ 14 d** |
| **M2** | Book & Feature Engine | 3 | M1 *started* | 10–15 d | ∥ M1 |
| **M3** | **Deterministic Replay** 🚧 | 4 | M2 | 5–8 d | ∥ |
| **M4** | Python research layer + promotion gates | 8 | M3 | 10–15 d | ∥ |
| **M5** | **Gate A verdict** | Pt 22 | M1 complete + M4 | 3–5 d | **≥ 3 mo from M1** |

**The critical path is M1 → M5 and it is approximately three months of calendar
time regardless of engineering velocity.** M2–M4 fit entirely inside that
window. M1 must therefore start before anything else.

**M3 is the hard gate.** An indeterministic replay makes every subsequent edge
measurement unfalsifiable, so it takes priority over model sophistication.

### 7.1 Existing assets

Fold these into M4 rather than rewriting:

- `agent/research/{train,features,synthetic,quant}.py` — walk-forward
  LogisticRegression baseline that already refuses to train on untrainable data
- `src/evaluation/metrics.cpp` and `agent/hermes/` — look-ahead and staleness
  exclusion rules Part 22 will need

---

## 8. Acceptance criteria

**These are not new.** Architecture Part 18 and Part 22 already specify them
quantitatively and machine-checkably. Reuse verbatim; do not reinvent.

| Milestone | Criteria | Source |
|---|---|---|
| **M0** | `mme_feed` runs 1 h with zero init failures; spike marked archived; no engine code references `experiments/` | this doc |
| **M1** | ≥10 events/s/symbol during London/NY on all 5 symbols · **zero sequence gaps over 24 h** · `BookSource` and `AccountMode` classified and logged · **≥14 consecutive days of WAL recorded** | Pt 18 Ph 2 |
| **M2** | book update p99 < 2 µs · feature vector p99 < 5 µs · ≥500 k updates/s · every feature unit-tested · feature × `BookSource` validity matrix published · **no `std::map`, no allocation, no strings on the hot path, enforced by test** | Pt 18 Ph 3 |
| **M3** | **Same WAL replayed twice → byte-identical journal, 100 consecutive runs. Nothing proceeds until it does.** · ≥100× realtime · ≥100 k events/s · speed-independence (1× vs 1000× identical) | Pt 18 Ph 4, Pt 11.3 |
| **M4** | Shuffled labels → AUC ≈ 0.5 · purge/embargo boundaries correct · **cost model agrees with the C++ fee model to the cent** · `gates/promotion.py` emits machine-checkable PASS/FAIL · full walk-forward over 6 months < 30 min | Pt 18 Ph 8 |
| **M5** | ≥3 months **real** data (never synthetic) · ≥1,000 in-sample / ≥500 out-of-sample trades · positive net expectancy after full modelled costs · no walk-forward fold worse than −50% of mean · **permutation p < 0.01** · bootstrap 95% CI on expectancy excludes zero · no leakage · multiple-comparison correction disclosed | Pt 22 Gate A |

> Part 22: **"Positive gross PnL is never sufficient at any gate."**

---

## 9. Risks

### R1 — The three-month clock is the entire schedule

Gate A's *"never synthetic"* wording is unambiguous. If recording starts
2026-07-31, the earliest possible edge verdict is approximately **2026-11-01**.
If it starts three weeks later, that becomes late November. Nothing else in
this plan moves that date. Every day not recording is a day added.

**Mitigation:** treat M1 as blocking all other work until the recorder is up
and verified. It is 2–3 days of engineering followed by patience.

### R2 — Host suspension silently breaks the 14-day gate

On 2026-07-30 the spike lost 2 h 08 m to a host suspension with no detection.
The Phase 2 criterion is *consecutive* days with zero sequence gaps; one
unnoticed sleep resets the count to zero.

**Mitigation:** disable sleep and hibernate on the recording host before M1
begins; make gap detection page loudly rather than log quietly. This is the
highest-value hour of work currently available.

### R3 — `mme_feed` is currently non-functional

The feed EA — the component that must run unattended for 14 days — failed init
with code 1 repeatedly on 2026-07-30 and has produced no output since. Its
reliability is unproven over any duration.

**Mitigation:** M0 must diagnose the init failure and demonstrate a clean
one-hour run before the 14-day count is considered started.

### R4 — Stale primary documentation

`CLAUDE.md` describes a system materially smaller than the one that exists. Any
future session — human or AI — reading it as the source of truth will plan
against the wrong architecture, as occurred during this session.

**Mitigation:** refresh in M0. Consider making `ARCHITECTURE_V1.md` the
explicit entry point.

### R5 — Spike promotion pressure

The spike works, places protected trades, and is visibly satisfying. That is
precisely what makes it dangerous: it has no edge (net −1.43 over 12 trades),
fails five of nine live guards, and violates Invariant 6.

**Mitigation:** §1.3's dependency constraint, and the observation that the
spike's own header already says this.

---

## 10. Summary

The spike answered its question and is closed. The order path works,
broker-side protection survives engine death, and identity gating fails closed.
Its two failures were more valuable than its successes and are now test cases.

**It contributes no code to the engine.**

The binding constraint on alpha is not engineering — it is that zero days of
real market data have been recorded, against a 14-day phase gate and a
three-month promotion gate. The next action is to start the recorder.

---

*Related: `docs/ARCHITECTURE_V1.md` · `docs/ARCHITECTURE_V1_ERRATA.md` ·
`docs/RISK_INVARIANTS.md` · `docs/CURRENT_CHECKPOINT.md` · `CURRENT_STATUS.md`*
