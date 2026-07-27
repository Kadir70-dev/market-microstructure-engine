# CURRENT_STATUS.md

> ## ✅ READY FOR RELEASE — 2026-07-28
>
> Foundation (F1–F6) and Layout (L1–L4) are complete and audited. Release
> decision: **PASS WITH MINOR FIXES** (fixes applied, see §8). Backend contract
> remains frozen at `API_CONTRACT.md` v1.0; no C++, SQLite, schema, engine, or
> API-route change was made during any UI phase.
>
> Release verification: `npm run build` green · `ctest` 2/2 green · live SQLite
> read confirmed · zero horizontal overflow at 375 / 768 / 1440 px · no unnamed
> interactive controls · keyboard navigation working · polling 6 req/min/endpoint
> with hidden-tab pause.

Verified state of the system as of **2026-07-27**, re-verified for release on
**2026-07-28**. Every claim below was checked by running the thing, not by
reading the code. Where something is unproven it says so.

Environment used for verification: Windows 11 host (Node 24.18.0, npm 11.16.0)
for the dashboard; `ubuntu:24.04` container for the C++ engine (the host lacks
`make`/`spdlog`/`libsqlite3-dev`; WSL lacks passwordless sudo).

---

## 1. Component status

| Component | State | Evidence |
|---|---|---|
| C++ `engine` | **Working** | Built Release; bounded runs wrote 27 ticks / 27 signals / 27 quality rows to `data/engine.db` with mixed momentum (Bullish 6 / Bearish 15 / Neutral 6), clean SIGTERM shutdown |
| C++ `engine_backtest` | **Working** | Read-only run reported `Signals: 12, Usable: 3, Excluded: 0 stale, 0 no baseline, 9 no future` + gross/net tables |
| `unit_tests` (CTest) | **Passing** | `1/2 unit_tests … Passed` |
| Hermes tests (CTest + host) | **Passing** | `2/2 hermes_tests … Passed`; 8/8 via `python -m unittest` on Windows |
| Dashboard build | **Passing** | `next build` — compiled, type-checked, 6 pages + 8 API routes |
| Dashboard live DB read | **Working (newly verified)** | `/api/overview` → `"source":"live"`, `nTicks:27` matching the DB exactly |
| `better-sqlite3` native module | **Working** | `require("better-sqlite3")` + in-memory query OK on Node 24 (npm's `allow-scripts` warning was benign — the prebuilt binary is present) |
| MT5 → Wine → MQL5 EA feed | **Unverified** | Linux-only; not installed here. Engine verified against a CSV in the EA's exact schema (`symbol,epoch_seconds,bid,ask,mid`) |
| `ops/*.sh`, systemd units | **Unverified on this host** | bash/systemd only |
| `ops/health_check.py` | **Working** | Ran in container; correctly reported FAIL for missing DB before collection |

---

## 2. Data inventory — implemented / derivable / missing

### 2.1 Currently implemented (real data exists today)

| Data | Table / file | Endpoint |
|---|---|---|
| Mid prices per symbol, 30s cadence | `ticks(ts, symbol, price)` | `/api/prices` |
| Momentum, volatility score + regime | `signals` | `/api/signals` |
| Staleness, session, confidence, trade quality, grade | `quality_scores` | `/api/signals`, `/api/overview` |
| Collection window, counts, stale % | derived | `/api/overview` |
| Feed health, frozen detection, tick age | derived from `ticks` | `/api/health` |
| Hypothetical fwd-60s equity, gross vs net of cost | derived | `/api/equity` |
| Confidence calibration by band | derived | `/api/signals` |
| ML baseline metrics, calibration, feature importance, net-of-cost curve | `agent/research/results/model_results.json` | `/api/model` |
| Hermes daily reports | `agent/hermes/reports/*.md` | `/api/reports`, `/api/reports/{date}` |

### 2.2 Derivable from existing tables (no engine change, endpoint work only)

- Multi-horizon (300s / 900s) directional accuracy and net return — the C++
  backtest already does this; `lib/analytics.ts` is hard-coded to 60s.
- Hypothetical max drawdown, return dispersion, cost-drag ratio, rolling hit
  rate.
- Realized volatility per symbol / session / regime.
- **Collection cadence**: tick-interval distribution, missed cycles, engine
  downtime gaps, coverage timeline.
- Grade/regime transition counts, per-session signal density.

### 2.3 Missing — not implemented, must not be faked

| Data | Why it's missing | Unblocked by |
|---|---|---|
| Spread, effective spread, microprice | `ticks` stores **mid only**; `bid`/`ask` are parsed from the CSV then discarded | Schema migration + `sqlite_logger.cpp` change |
| Order-book depth, imbalance, queue position, OFI, trade prints | The MQL5 EA exports top-of-book snapshots only | EA change + new table + tick-level feed |
| True latency (fetch / compute / persist ms, cycle wall-time) | No timing instrumentation exists | New `cycle_timings` table + C++ change |
| Broker / order-ack latency | Nothing connects to a broker | Not applicable before Phase 4 |
| **Real PnL, positions, exposure, fills, VaR, margin** | **No execution path exists anywhere — by design** (`CLAUDE.md` §12) | Explicit human Phase 4 decision only |

---

## 3. Changes made in this session

Scope was deliberately limited to foundational blockers. **No C++, quant logic,
tests, schema, or UI design was touched.**

| File | Change |
|---|---|
| `dashboard/lib/db.ts` | Replaced the process-lifetime dataset cache with a 5s TTL **plus** `engine.db` mtime invalidation (`DASHBOARD_CACHE_TTL_MS` override); demo fallbacks now carry a machine-readable `sourceReason` |
| `dashboard/lib/types.ts` | `Dataset.sourceReason?: string \| null` (additive, optional) |
| `dashboard/lib/analytics.ts` | `overview()` and `health()` pass `sourceReason` through (additive fields only) |
| `dashboard/lib/api.ts` | **New.** `jsonOk` / `jsonError` / `handle` — one error shape, `Cache-Control: no-store` on every response |
| `dashboard/app/api/*/route.ts` (8 files) | Wrapped in `handle()`; `/api/reports/{date}` 404 now uses the standard error body. **Success payload shapes unchanged** |
| `API_CONTRACT.md`, `UI_EXECUTION_PLAN.md`, `CURRENT_STATUS.md` | New planning documents |

Not changed and still exactly as committed: `src/`, `include/`, `tests/`,
`CMakeLists.txt`, `agent/`, `ops/`, `docs/`, all dashboard pages and components.

### Verified after the change

- `nTicks` went 21 → 27 across a real engine write **without a server restart**
  (previously impossible — the cache never expired).
- `GET /api/overview` with a valid DB → `"source":"live"`, counts matching
  `sqlite3` exactly.
- `GET /api/overview` with a bad path → `"source":"demo"` +
  `"sourceReason":"DASHBOARD_DB_PATH=../data/does-not-exist.db does not exist or is unreadable"`.
- `GET /api/reports/1999-01-01` → 404 `{"error":{"code":"not_found",…}}`.
- All responses carry `cache-control: no-store`.

---

## 4. Known defects (not fixed — deliberately out of scope)

1. **Markdown tables don't render** on `/reports` — `react-markdown` is used
   without `remark-gfm`, so Hermes' pipe tables print as raw text. CSS for
   `.prose-report table` already exists. → `UI_EXECUTION_PLAN.md` task F1.
2. **No favicon** → `404 /favicon.ico` on every page load. → task F2.
3. **No loading / error / empty states** in any page. → task F4.
4. **6 of 8 API routes have no UI consumer** — `/` and `/prediction` are server
   components calling `lib/` directly. → task F3.
5. **Whole-dataset in-memory load** — fine now, becomes the scaling wall around
   ~10⁶ ticks.
6. **No auth, no TLS.** Bind to localhost only.

---

## 5. Runtime facts

- Dashboard: `npm start` in `dashboard/`, port **3000**. Live data requires
  `DASHBOARD_DB_PATH=../data/engine.db`; otherwise a synthetic demo session
  (5,040 rows) renders with a visible badge.
- Engine: run from `build/`, writes `../data/engine.db`; feed path via
  `MME_QUOTES_CSV`, DB override via `MME_DB_PATH`.
- `data/engine.db` currently on disk holds **27 real engine-produced cycles**
  from bounded verification runs against a synthetic CSV feed — useful for UI
  development, **not** market-meaningful data. It is gitignored.

---

## 6. Roadmap position

Phase 0 (foundation) ✅ · Phase 1 (read-only MT5, code) ✅ · Phase 2
(observability dashboard) ✅ — now with a live-capable data layer · Phase 3
(edge research) ⏳ blocked on weeks of real collection · Phase 4/5 (execution)
not started and gated on an explicit human decision.

---

## 7. Decision log

### 2026-07-27 — Backend frozen, Foundation phase handed to Codex

1. **No engine-side changes in this phase.** No bid/ask persistence, no
   order-book capture, no latency persistence, no execution persistence, no
   SQLite schema change, no C++ modification. Standing decision — see
   `UI_EXECUTION_PLAN.md` §4.
2. **`API_CONTRACT.md` frozen at v1.0.** Response shapes, error envelope,
   provenance fields and status codes are stable. Only critical bug fixes are
   permitted while frozen — a fix restores documented behaviour, never changes
   it. Unfreezing requires an explicit decision and a version bump.
3. **Planning docs stay at the repository root** (`UI_EXECUTION_PLAN.md`,
   `API_CONTRACT.md`, `CURRENT_STATUS.md`).
4. **Codex implements the entire Foundation phase F1–F6**, not a subset.
   Handoff brief and file allow/deny list: `UI_EXECUTION_PLAN.md` §3.0.
5. **Hard stop before Layout (L1).** After F1–F6 the work is reviewed and
   builds/tests re-verified; L1 does not begin without a further decision.

Consequence of (1) for the UI: everything in §2.3 stays unbuildable this phase —
order flow, true latency, and real PnL/risk panels are not merely unstyled, they
have no data source and must not appear.

---

## 8. Release audit — 2026-07-28

**Decision: PASS WITH MINOR FIXES.** All blockers found were fixed and
re-verified before the release commit.

### Delivered in F1–F6 + L1–L4

Typed API client (`lib/client.ts`, `lib/api-types.ts`) with a typed `ApiError`;
shared polling hook (`lib/useLiveData.ts`, 10s, hidden-tab pause, request-race
guard); one shared overview/health subscription for the whole app
(`components/live-context.tsx`); per-panel loading / error / empty states with
retry (`components/states.tsx`); route-level `error.tsx` / `loading.tsx`;
provenance banner with `sourceReason` on every route; GFM markdown rendering for
Hermes reports; app icon; terminal shell with collapsible rail, UTC clock,
status strip, `Alt+1/2/3` navigation and a mobile bottom nav
(`components/shell.tsx`); layout primitives (`components/layout.tsx`); portfolio
hero, capability grid, architecture strip and maturity matrix
(`components/portfolio.tsx`); terminal design tokens in `tailwind.config.ts`.

### Backend audit result

Zero unauthorized change. The only tracked backend edits in this release are the
frozen-contract work from 2026-07-27 (`handle()` wrapper on the 8 API routes,
TTL + mtime dataset cache in `lib/db.ts`, `sourceReason` in `lib/types.ts` /
`lib/analytics.ts`). `src/`, `include/`, `tests/`, `CMakeLists.txt`, `agent/`,
`ops/`, `docs/`, and the SQLite schema are untouched — `git diff` shows no entry
for any of them.

### Fixes applied during the release audit

| Fix | Reason |
|---|---|
| `min-w-0` on the report-document card (`app/reports/page.tsx`) | The grid item was sized by the Hermes table's min-content width, scrolling the whole page 126px sideways at 390px. Isolated by hiding candidates and re-measuring `document.scrollWidth`. |
| `.prose-report table` → `display:block; overflow-x:auto` (`app/globals.css`) | 8-column Hermes tables now scroll inside the panel instead of overflowing it on narrow screens. |
| `.prose-report code` → `overflow-wrap:anywhere` (`app/globals.css`) | Hermes embeds absolute DB paths; the unbreakable token could not wrap. |
| Removed unused `UnavailablePanel` (`components/states.tsx`) | Dead export, zero references. |
| `dashboard/.gitignore` → ignore `*.png` | 19 local UI verification captures sat untracked in `dashboard/`, one `git add .` away from being committed. |

### Non-blocking, deferred

- `/reports` renders two `<h1>` (page title + the markdown report's own H1).
- Hero "Build status: Verified / Passing" and the maturity matrix's audit date
  are hardcoded strings, not derived from CI.
- `next@14.2.35` carries 2 high-severity advisories (DoS / cache-poisoning
  classes). Pre-existing, unrelated to this work, and not reachable in a
  localhost read-only deployment. A Next major upgrade is its own reviewed
  change.
- `/reports` first-load JS is 135 kB (react-markdown + remark-gfm); the two
  other routes are ~200 kB shared-chunk dominated.
- New UI files use a denser one-statement-per-line style than the rest of the
  repository.
