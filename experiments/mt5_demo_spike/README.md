# MT5 Demo Spike — ARCHIVED REFERENCE IMPLEMENTATION

**Status:** ARCHIVED · 2026-07-30 · objective achieved, no further development

> ## ⚠️ NOT PART OF THE INSTITUTIONAL ENGINE
>
> **No production engine code may depend on, include, link against, or be
> copied from this spike implementation.**
>
> This is MQL5 throwaway code written to prove one thing quickly. Its nearest
> architectural relative is `mme_exec.mq5` (Architecture Part 18 Phase 10),
> which does not exist yet and will be written against Parts 5, 8 and 10 — not
> adapted from here.

---

## Objective — achieved

Prove an end-to-end protected order path on an MT5 demo account. It did.

| Objective | Result |
|---|---|
| **End-to-end broker execution** | 12 completed round trips · 25 `DONE` retcodes · 0 duplicate positions across 306 order results |
| **Broker-side SL/TP** | 12 of 12 fills carried both, verified by reading them back off the live position. Confirmed under adversity: when the host suspended for 2 h 08 m, the broker's stop closed the position unaided |
| **Safety guards** | 100% of identity mismatches produced `INIT_FAILED` with a named reason — wrong account, wrong server, non-demo, disallowed symbol, lot ≠ broker minimum |
| **Rejection backoff** | State machine validated offline, 26/26 assertions, including a 6,000-tick flood bounded to 5 attempts. *Runtime wiring was never exercised by a live rejection* |
| **Observability** | Structured single-line logging proved sufficient to diagnose both production incidents entirely from disk, without a debugger or a running terminal |

**Trading result: net −1.43 USD over 12 trades.** Not a defect. This is a
fixed-threshold momentum scalper with symmetric stops and no edge, by design —
see the header of `mme_spike_scalper.mq5`.

---

## Why this is preserved

Archived rather than deleted, because:

1. **Historical reference** — the evidence trail for four frozen architectural
   decisions. The claims are only checkable against the code that produced them.
2. **Regression testing** — three test cases for Phases 6 and 10 were derived
   from its failures. The failing code is the specification for those tests.
3. **Incident reproduction** — two real incidents (a 275-order rejection flood;
   a silent counter desync after a stop-out executed during disconnect) are
   reproducible only against this source.
4. **Engineering documentation** — a bounded, honest artifact that states its
   own limitations in its header and reports a net loss without dressing it up.

---

## What migrates — and what does not

**Only validated findings and test cases migrate. The implementation does not.**

| Migrates | Does not migrate |
|---|---|
| Empirical confirmation of Invariant #1 (broker-side stop survives engine death) | Every line of code in this directory |
| Evidence that unbounded retry is a real failure mode | The backoff implementation — it classifies `NO_MONEY` as retryable, while Part 5.8 classifies it **Fatal, no retry** |
| Evidence that identity gating fails closed | The four guards themselves — Part 5.5 requires nine |
| Three derived test cases | The position-adoption behaviour, which silently auto-corrects where Part 8.6 requires a halt |

The spike contributes **zero lines of code** to the engine.

It is also structurally incompatible: it uses `double` prices where Part 5.9
mandates `int64` ticks, `PrintFormat` strings where Part 3 forbids strings on
the hot path, in-memory-only state where Invariant 6 requires durable kill
state, and a single hardcoded symbol where Part 3 specifies a multi-symbol
portfolio.

---

## Contents

| File | Role |
|---|---|
| `mme_spike_scalper.mq5` | The EA — signal, order path, lifecycle |
| `mme_spike_guards.mqh` | Identity, symbol, lot, filling, stops, autotrading gates |
| `mme_spike_log.mqh` | Single-line tagged logging + retcode names |
| `mme_spike_backoff.mqh` | Rejection backoff state machine |
| `mme_spike_backoff_test.mq5` | 26 deterministic offline assertions |

---

## See also

- **`docs/SPIKE_TO_ENGINE_MIGRATION.md`** — full migration analysis: validated
  findings, reusable concepts, non-transferable code, roadmap, acceptance
  criteria, risks
- **`docs/ARCHITECTURE_V1.md`** — the frozen target architecture this spike is
  subordinate to
- **`docs/ARCHITECTURE_V1_ERRATA.md`** — approved errata to the above
