# UI_EXECUTION_PLAN.md

Execution plan for the institutional trading UI. Sequenced so that **no task
depends on data that does not exist**. Read `API_CONTRACT.md` first — it is the
authority on what is real. This file is the authority on *order of work*.

Status of this document: planning only. No UI has been redesigned yet.

---

## 0. Non-negotiable constraints

1. **No fabricated data.** No order book, no fills, no positions, no account
   PnL, no wire latency. If a panel has no data source in `API_CONTRACT.md`, it
   ships as a documented empty state or it does not ship.
2. **Read-only.** The dashboard never writes to `engine.db`, never issues an
   order, never gains a control surface. No POST/PUT/DELETE routes.
3. **Hypothetical stays labeled.** Equity/return panels carry the word
   *hypothetical* in the visible label, not only in a tooltip.
4. **Do not touch** `src/`, `include/`, `tests/`, `agent/`, `ops/`,
   `CMakeLists.txt`, or the SQLite schema in a frontend task. UI work is
   confined to `dashboard/`.
5. **`npm run build` must pass** (it type-checks) at the end of every task.
6. Provenance (`source` / `sourceReason`) is visible on every screen showing
   dataset-derived numbers.

---

## 1. Where the frontend stands today

Working: 3 routes (`/`, `/prediction`, `/reports`), 8 API routes, 9 Recharts
components, 3 UI primitives (`Card`, `Stat`, `DemoBanner`), a coherent dark
quant palette in `tailwind.config.ts`, production build green.

Structural gaps blocking a premium UI:
- The two server pages call `lib/` directly and bypass their own API routes, so
  6 of 8 endpoints have no consumer and no client-side data layer exists.
- No loading, error, or empty states anywhere; no auto-refresh.
- Layout primitives (KPI row, data table, status pill, panel header, section
  grid) are inlined in pages rather than extracted.
- Markdown tables do not render (`react-markdown` without `remark-gfm`).
- No app icon → `404 /favicon.ico` on every page load.

---

## 2. Phasing

| Phase | Goal | Depends on |
|---|---|---|
| **F — Foundation** | Design system + data layer + states. No new panels. | Nothing (ready now) |
| **L — Layout** | Institutional shell: dense grid, workspace nav, symbol context. | F |
| **A — Analytics depth** | Multi-horizon returns, drawdown, cadence, session/regime views — all from existing tables. | F, L |
| **E — Engine-blocked** | Order flow, true latency, real PnL/risk panels. | **C++ engine work first** (§4) |

Only **F** and **L** are ready to execute. **A** needs one new endpoint each.
**E** must not be started.

---

## 3. Frontend task list (Codex-executable)

Each task is independently executable, touches a bounded file set, and states
its own acceptance check. Tasks are ordered; do not reorder F1–F6.

### 3.0 Codex handoff brief — Foundation phase (F1–F6)

**Assignment:** implement **all of F1 through F6**, in order, in one pass.
**Do not start L1 or any Layout task.** Stop after F6 and hand back for review.

**Working directory:** `dashboard/` only.

**Files you may create or modify:**
`dashboard/package.json`, `dashboard/package-lock.json`,
`dashboard/app/reports/page.tsx`, `dashboard/app/icon.svg`,
`dashboard/app/error.tsx`, `dashboard/app/loading.tsx`,
`dashboard/components/states.tsx`, `dashboard/components/ui.tsx`,
`dashboard/lib/api-types.ts`, `dashboard/lib/client.ts`,
`dashboard/lib/useLiveData.ts`, the header render in
`dashboard/app/layout.tsx` (F6 indicator only), and — **only** to wire in
states, provenance, and polling (F4/F5/F6) — `dashboard/app/page.tsx` and
`dashboard/app/prediction/page.tsx`. Those two may change how data arrives
(server render → client hook) but **not** what is rendered: same panels, same
order, same copy, same charts.

**Files you must NOT touch:** `src/`, `include/`, `tests/`, `CMakeLists.txt`,
`agent/`, `ops/`, `docs/`, `data/`, `dashboard/components/charts.tsx`,
`dashboard/lib/db.ts`,
`dashboard/lib/analytics.ts`, `dashboard/lib/quant.ts`,
`dashboard/lib/api.ts`, `dashboard/lib/types.ts`, and every file under
`dashboard/app/api/`. **The backend is frozen** (`API_CONTRACT.md`, v1.0). If a
task appears to require a backend change, stop and report instead of changing
it — the only exception is a genuine bug where a route violates the contract.

**Do not redesign the UI.** F1–F6 add a data layer, states, provenance, and
refresh. Visual layout, spacing, and panel composition stay as-is; that is the
Layout phase.

**Ground rules that override any local convenience:** the six constraints in §0
of this document, and rule #1 of `API_CONTRACT.md` — no fabricated data. If an
endpoint has no data, render an empty state that says so.

**Verification required before handing back** (run from `dashboard/`, paste real
output):

```bash
npm install
npm run build                       # must pass — this is the type-check gate
npm start                           # demo mode: no DASHBOARD_DB_PATH
DASHBOARD_DB_PATH=../data/engine.db npm start   # live mode against the real DB
```

Then confirm, with actual observations: `/`, `/prediction`, `/reports` all
return 200 in both modes; the demo badge and its `sourceReason` appear when
`DASHBOARD_DB_PATH` is unset; the Hermes tables render as real tables; no
`404 /favicon.ico`; a forced API failure shows `ErrorPanel` rather than a blank
page or a Next error overlay.

**Report back:** files changed, the build output, the per-task acceptance
results, and anything you could not do without touching a frozen file.

### Phase F — Foundation

**F1 — Render Hermes markdown correctly**
Add `remark-gfm` to `dashboard/package.json` and pass it to `ReactMarkdown` in
`app/reports/page.tsx`. `app/globals.css` already styles `.prose-report table`.
*Accept:* the Symbol Performance / Confidence Analysis tables on `/reports`
render as HTML `<table>`, not pipe text. `npm run build` passes.

**F2 — Add an app icon**
Add `app/icon.svg` (or `icon.png`) using the `◧` mark and `#38bdf8` accent.
*Accept:* no `404 /favicon.ico` in the browser network log on any route.

**F3 — Typed API client + shared response types**
Create `dashboard/lib/api-types.ts` (exported interfaces for every payload in
`API_CONTRACT.md` §1–§10) and `dashboard/lib/client.ts` with a single
`fetchJson<T>(path)` that throws a typed `ApiError` on the
`{ error: { code, message } }` body. No component may call `fetch` directly
after this task.
*Accept:* `lib/client.ts` exists, is typed, handles 4xx/5xx; build passes.

**F4 — State primitives**
Add `components/states.tsx` exporting `LoadingPanel` (skeleton matching `Card`
geometry), `ErrorPanel` (message + retry callback), `EmptyPanel` (reason text,
e.g. "engine has not collected data yet"). Add `app/error.tsx` and
`app/loading.tsx` route-level boundaries.
*Accept:* every panel can render all three states; forcing an API 500 shows
`ErrorPanel`, not a blank page or a Next error overlay.

**F5 — Provenance banner everywhere**
Extend `DemoBanner` to consume `source` + `sourceReason` (both now returned by
`/api/overview` and `/api/health`) and render the reason text. Add the same
badge to `/prediction` and `/reports`.
*Accept:* with `DASHBOARD_DB_PATH` unset, every route shows the demo badge and
the reason "DASHBOARD_DB_PATH is not set".

**F6 — Live refresh**
Add `lib/useLiveData.ts` (client hook: poll `fetchJson` every N ms, default
10000, pause when `document.hidden`, expose `{data, error, loading, lastUpdated,
refresh}`). Add a header "updated Xs ago" indicator.
*Accept:* with the engine running, KPI values change without a manual reload;
polling stops when the tab is hidden.

### Phase L — Layout

**L1 — Design tokens & density scale**
Extend `tailwind.config.ts` with spacing/typography scale for a dense terminal
layout (compact row height, tabular-nums for all numeric cells). Do not change
existing color token names — components depend on them.

**L2 — Extract layout primitives**
`components/layout.tsx`: `PanelHeader`, `KpiRow`, `DataTable` (generic, sortable
columns, `tabular-nums`, sticky header, `overflow-x-auto`), `StatusPill`,
`SectionGrid`. Refactor `app/page.tsx` to use them — **no visual regression, no
logic change**.

**L3 — Workspace shell**
Sticky header with symbol selector (from `/api/overview.symbols`), UTC clock,
connection/provenance status, and the existing LIVE TRADING DISABLED badge.
Introduce a left rail for workspace navigation. Routes stay the same.

**L4 — Symbol context**
Global symbol filter (client state) applied to price, signal, and calibration
panels. Client-side filtering only until `/api/prices?symbol=` exists.

### Phase A — Analytics depth (each needs its endpoint first)

**A1** `/api/returns` — multi-horizon (60/300/900s) directional accuracy + net
return by symbol / regime / confidence band. *Derivable now* — generalize
`forwardObservations()` to take a horizon. Mirrors `engine_backtest` exactly.
**A2** `/api/risk` — hypothetical max drawdown, dispersion, cost drag, realized
volatility per symbol. Shape in `API_CONTRACT.md` §6. `positions: null`.
**A3** `/api/cadence` — tick-interval distribution, missed cycles, downtime
gaps. Label **cadence**, never latency.
**A4** Uptime/coverage timeline panel built on A3 (visualizes collection gaps —
the thing that invalidates naive backtests).

### Phase E — Blocked, do not start

**E1** Order-flow panel — blocked on bid/ask persistence (§4).
**E2** True-latency panel — blocked on C++ cycle instrumentation.
**E3** PnL / positions / risk-limit panels — blocked on Phase 4 execution, which
requires an explicit human decision per `CLAUDE.md` §12.

---

## 4. Engine-side unblockers — **DEFERRED, NOT AUTHORIZED (decision 2026-07-27)**

> **Explicitly ruled out for this phase:** no bid/ask persistence, no order-book
> capture, no latency persistence, no execution persistence, and **no SQLite
> schema change or C++ modification of any kind**. This is a standing decision,
> not a pending question — do not re-propose these as part of a UI task, and do
> not "temporarily" add a column to make a panel work.

Listed only so the UI plan stays honest about what is missing and why.

1. **Persist bid/ask.** `ticks(ts, symbol, price)` → add nullable `bid`, `ask`.
   Touches `storage/sqlite_logger.cpp`, `market_data/file_provider.cpp` (already
   parses both), and the schema. Unblocks all spread-based order-flow metrics.
   Backward compatible: old rows keep NULL, readers must tolerate it.
2. **Cycle timing instrumentation.** New `cycle_timings(ts, symbol, fetch_ms,
   compute_ms, persist_ms)`. Unblocks the true-latency panel.
3. **Tick-level feed.** 30s polling caps every microstructure metric. Requires
   the MT5 tick path, not the CSV snapshot.

Each is a separate reviewed change with its own unit tests, and each must keep
`engine_backtest`'s dependency footprint unchanged (sqlite3 only).

---

## 5. Definition of done for the UI phase

- Every panel maps to a documented endpoint in `API_CONTRACT.md`.
- Every panel handles loading / error / empty explicitly.
- Provenance visible on every screen; `source !== "live"` is unmissable.
- Nothing hypothetical is labeled as realized.
- `npm run build` green; C++ `ctest` untouched and still green.
