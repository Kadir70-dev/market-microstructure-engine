# API_CONTRACT.md

> ## 🔒 FROZEN — v1.0 · 2026-07-27
>
> **All backend contracts in this document are frozen for the UI Foundation
> phase (F1–F6).** The response shapes in §1–§10, the error envelope in §0, and
> the provenance fields (`source`, `sourceReason`) are stable. Frontend work may
> rely on them without defensive re-checking.
>
> **Permitted backend changes while frozen:** critical bug fixes only — a route
> returning wrong data, throwing, or violating this document. A bug fix restores
> the documented behaviour; it never changes it.
>
> **Not permitted while frozen:** new endpoints, new or renamed response fields,
> changed types or units, changed status codes, and **any C++ / SQLite schema
> change** (see the explicit deferral in `UI_EXECUTION_PLAN.md` §4).
>
> Unfreezing requires an explicit decision and a version bump here (v1.1+),
> recorded in `CURRENT_STATUS.md` §7.

Data contracts between the C++ engine's SQLite ground truth and the dashboard
frontend. **This is the authoritative interface spec for UI work.** A frontend
task may only consume endpoints marked `IMPLEMENTED` or `PLANNED-DERIVABLE`;
anything marked `NOT IMPLEMENTED` must render an explicit empty state, never a
placeholder number.

> **Rule #1 — no fabricated data.** There is no order book, no execution, no
> fills, no positions, and no wire-latency measurement anywhere in this system.
> A UI that displays them would be lying. See `CLAUDE.md` §12.

---

## 0. Conventions

**Transport.** All routes are `GET`, Next.js App Router route handlers under
`dashboard/app/api/`, `runtime = "nodejs"`, `dynamic = "force-dynamic"`,
`Cache-Control: no-store`. Read-only: no POST/PUT/DELETE exists or may be added.

**Success.** HTTP 200, payload returned unwrapped (no envelope) exactly as
documented below.

**Failure.** Any thrown error, missing resource, or invalid parameter:

```jsonc
// HTTP 4xx / 5xx
{ "error": { "code": "not_found" | "internal_error" | "bad_request", "message": "human readable" } }
```

Implemented centrally in `dashboard/lib/api.ts` (`handle`, `jsonOk`,
`jsonError`). Route handlers must not construct raw `NextResponse.json` errors.

**Empty vs error.** "No data yet" is **not** an error. Empty datasets return 200
with empty collections and zeroed scalars (`points: []`, `n: 0`,
`accuracy: null`). `null` means *not computable*; `0` means *computed and zero*.
The frontend must distinguish these.

**Time.** All timestamps are **unix seconds, UTC** (`ts`). No local time
anywhere. Sessions are UTC-based with no DST (`lib/quant.ts::detectSession`).

**Data source.** Every dataset-derived payload carries provenance:

```ts
source: "live" | "demo"          // live = read from engine.db; demo = synthetic
sourceReason: string | null      // why it fell back to demo; null when live
```

The UI **must** display a persistent badge whenever `source !== "live"`, and
should surface `sourceReason` (e.g. "DASHBOARD_DB_PATH is not set").

**Freshness.** `lib/db.ts` caches the dataset for `DASHBOARD_CACHE_TTL_MS`
(default 5000ms) **and** invalidates on `engine.db` mtime change. Engine cycle
is 30s, so a 5s TTL is always fresher than the data itself. Clients may poll at
5–15s; there is no push/stream transport (see §11).

---

## 1. Overview — `GET /api/overview` — **IMPLEMENTED**

Source: `ticks`, `signals`, `quality_scores`.

```ts
{
  source: "live" | "demo";
  sourceReason: string | null;
  dbPath: string | null;          // absolute path when live, else null
  symbols: string[];              // sorted, engine-internal names: "EUR/USD" | "XAU/USD" | "USO"
  nTicks: number;
  nSignals: number;
  nQuality: number;
  stale: number;                  // count of quality rows with stale=1
  stalePct: number;               // 0..100, 0 when nQuality === 0
  momentum: { Bullish: number; Bearish: number; Neutral: number };
  meanConfidence: number;         // 0..100, 0 when no rows
  firstTs: number | null;         // null when no ticks
  lastTs: number | null;
}
```

---

## 2. Live prices — `GET /api/prices` — **IMPLEMENTED (polling, mid-only)**

Source: `ticks`. Downsampled to ≤300 points per symbol by uniform stride.

```ts
Record<string /* symbol */, { ts: number; price: number }[]>   // ascending ts
```

**Constraints the UI must respect:** `price` is the **mid** only — the engine
persists no bid/ask (see §4). Cadence is 30s, not tick-level. This endpoint is
not a real-time quote feed and must not be labeled one.

*Planned parameters (not yet implemented):* `?symbol=`, `?from=`, `?to=`,
`?maxPoints=`. Until then the client receives the full window and slices
locally.

---

## 3. Signals — `GET /api/signals` — **IMPLEMENTED**

Source: `signals` + `quality_scores`.

```ts
{
  perSymbol: Record<string, { bull: number; bear: number; neutral: number; n: number }>;
  grades: { A: number; B: number; C: number; D: number };
  regimes: { LOW: number; MEDIUM: number; HIGH: number };
  sessions: Record<"Asia"|"London"|"LondonNY"|"NewYork"|"Closed", number>;
  confidenceHistogram: { label: string; count: number }[];   // 10 fixed bins, "0-9" … "90-99"
  calibration: { band: string; n: number; accuracy: number | null }[];
  // bands: "Low (0-29)" | "Medium (30-59)" | "High (60-100)"
  // accuracy is fwd-60s directional %, null when n === 0 (NOT 0)
}
```

Calibration exclusion rules (identical to `engine_backtest`): Neutral excluded,
stale excluded, baseline must be ≤120s before the signal, future tick must be
within 120s of `ts+60`. Excluded observations are **dropped, never imputed**.

---

## 4. Order-flow metrics — **NOT IMPLEMENTED — blocked on the engine**

No endpoint. Do not build a UI panel that implies one exists.

| Metric | Blocker |
|---|---|
| Spread, effective spread, microprice | `mme_quotes.csv` *has* `bid,ask` but `storage/sqlite_logger.cpp` persists **mid only** — `ticks(ts, symbol, price)`. Requires a schema migration. |
| Depth / book imbalance / queue position | The MQL5 EA exports top-of-book only. Requires an EA change (DOM export) *and* a new table. |
| Trade prints, aggressor side, OFI | Never collected. Not available from the file-export feed at all. |

**Unblocking sequence (engine work, not UI work):** add `bid`/`ask` columns to
`ticks` (nullable, backward compatible) → backfill nothing, forward-only →
expose `GET /api/orderflow` returning per-symbol spread stats. Until that lands,
the frontend renders a documented "not collected yet" panel.

---

## 5. Equity / PnL — `GET /api/equity` — **IMPLEMENTED (HYPOTHETICAL ONLY)**

```ts
{
  points: { ts: number; gross: number; net: number }[];   // cumulative %, ascending
  stats: { n: number; hitRate: number; cumGrossPct: number; cumNetPct: number };
}
```

> **Mandatory UI labeling.** These signals were **never traded**. This is a
> cumulative forward-60s directional return with a per-symbol round-trip cost
> model applied — a research diagnostic, not a track record. Any label reading
> "PnL", "returns", "performance", or "account equity" without the word
> *hypothetical* adjacent to it is a contract violation.

**Real PnL is NOT IMPLEMENTED and must not be approximated.** There is no
execution path, no fills, no positions, no account. Real PnL arrives only at
roadmap Phase 4 (demo execution) behind an explicit human decision.

---

## 6. Risk — **PARTIALLY DERIVABLE / NOT IMPLEMENTED**

No endpoint yet. Split precisely:

| Metric | Status |
|---|---|
| Max drawdown, return dispersion / stddev, cost-to-gross-edge ratio, hit-rate confidence interval | **DERIVABLE** from `forwardObservations()` in `lib/analytics.ts` — all *hypothetical*, same labeling rule as §5. |
| Realized volatility per symbol/session | **DERIVABLE** from `ticks` (log-return stddev; formula in `indicators/volatility.cpp`). |
| Position exposure, leverage, margin, VaR on a book, kill-switch state | **NOT IMPLEMENTED.** There are no positions. Do not render. |

*Planned shape when built (`GET /api/risk`):*

```ts
{
  source, sourceReason,
  hypothetical: {
    maxDrawdownPct: number; stddevPct: number; hitRate: number;
    grossPct: number; netPct: number; costDragPct: number; n: number;
  };
  realizedVolatility: Record<string, { stddev: number; regime: "LOW"|"MEDIUM"|"HIGH" }>;
  positions: null;   // permanently null until Phase 4
}
```

---

## 7. Latency — **PARTIALLY DERIVABLE / NOT IMPLEMENTED**

| Metric | Status |
|---|---|
| Collection cadence: distribution of consecutive `ticks.ts` deltas per symbol, missed-cycle count, downtime gaps | **DERIVABLE** from `ticks` alone. This is *cadence*, not latency. |
| Feed staleness age: `now - lastTs`, frozen detection | **IMPLEMENTED** in `/api/health`. |
| Fetch→parse→persist duration, cycle wall-time, DB write latency | **NOT IMPLEMENTED.** Requires new C++ instrumentation + a `cycle_timings` table. |
| Broker/wire/order-ack latency | **NOT IMPLEMENTED and not applicable** — nothing connects to a broker. |

Any panel labeled "latency" that shows cadence must say **cadence**.

---

## 8. Model analytics — `GET /api/model` — **IMPLEMENTED (file-backed)**

Source: `agent/research/results/model_results.json`, written by
`agent/research/train.py`; bundled fallback snapshot in `lib/model.ts`.

```ts
{
  source: "synthetic" | "live" | "db";
  trainable?: boolean;            // false => only { trainable, reason, verdict, source } are present
  reason?: string;
  real_db_reason?: string | null;
  model?: string; n_samples?: number; n_oos?: number; n_features?: number;
  metrics?: { auc; auc_folds: number[]; precision; recall; f1; brier;
              baseline_auc; permutation_auc_mean; permutation_auc_std;
              p_value: number | null };
  calibration?: { mean_pred: number; frac_pos: number }[];
  feature_importance?: { feature: string; coef: number; abs: number }[];
  net_of_cost?: { threshold_curve: { margin; n_trades; gross_pct; cost_pct; net_pct; hit_rate }[] };
  verdict: string;                // plain-language honest conclusion — always render it
}
```

The UI must render `verdict` verbatim and badge `source !== "live"` as
SYNTHETIC. `trainable === false` is a first-class state, not an error.

---

## 9. Reports — `GET /api/reports`, `GET /api/reports/{date}` — **IMPLEMENTED**

```ts
// GET /api/reports
{ dates: string[] }                        // "YYYY-MM-DD", newest first; falls back to a demo date

// GET /api/reports/2026-05-24
{ date: string; markdown: string }         // 200
{ error: { code: "not_found", message: string } }   // 404, invalid or unknown date
```

Source: `agent/hermes/reports/*.md` (override with `DASHBOARD_REPORTS_DIR`).
Markdown is GitHub-flavored and **contains tables** — the renderer must enable
`remark-gfm` (see `UI_EXECUTION_PLAN.md`, task F1).

---

## 10. Feed health — `GET /api/health` — **IMPLEMENTED**

```ts
{
  source: "live" | "demo";
  sourceReason: string | null;
  nTicks: number;
  perSymbol: { symbol: string; ticks: number; lastPrice: number | null;
               lastTs: number | null; ageS: number | null; frozen: boolean }[];
  staleFlagsPresent: boolean;
}
```

`frozen` = last 5 ticks byte-identical, mirroring `validation.cpp::isStale`.
This is **DB-derived** health; it is *not* `ops/health_check.py` (process
liveness, PID, log freshness, CSV mtime), which has no HTTP surface.

---

## 11. Transport limits (read before designing anything "real-time")

- **Polling only.** No WebSocket, no SSE, no push. The engine writes to SQLite;
  the dashboard reads it. Recommended client poll: 10s (data changes every 30s).
- **Whole-dataset reads.** `loadDataset()` loads *all* rows into memory on cache
  miss. Fine at current volume (thousands of rows); becomes the first scaling
  wall around ~10⁶ ticks. Fix is SQL-side aggregation + windowing, not client
  pagination.
- **Single process, no auth.** Bind to localhost. There is no authn/authz layer
  and none is in scope while the surface is read-only.

---

## 12. Endpoint status summary

| Domain | Endpoint | Status |
|---|---|---|
| Overview | `/api/overview` | IMPLEMENTED |
| Live prices | `/api/prices` | IMPLEMENTED (mid-only, 30s, polling) |
| Signals | `/api/signals` | IMPLEMENTED |
| Order flow | — | **NOT IMPLEMENTED** (engine schema blocker) |
| Equity | `/api/equity` | IMPLEMENTED (hypothetical) |
| Real PnL | — | **NOT IMPLEMENTED** (no execution path, by design) |
| Risk | — | PARTIALLY DERIVABLE — `/api/risk` planned |
| Latency | — | cadence DERIVABLE; true latency **NOT IMPLEMENTED** |
| Model | `/api/model` | IMPLEMENTED (file-backed) |
| Reports | `/api/reports`, `/api/reports/{date}` | IMPLEMENTED |
| Feed health | `/api/health` | IMPLEMENTED |
