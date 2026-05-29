# Dashboard — read-only observability

A Next.js (App Router, TypeScript, Tailwind, Recharts) dashboard over the
engine's SQLite ground truth. It **never writes** to the database and **never
places trades** — it opens `engine.db` read-only, mirroring the engine's
cold-path boundary.

## What it shows

- **KPIs** — symbols, ticks, signals, directional vs neutral, mean confidence, stale %.
- **Price chart** — multi-symbol, normalized to 100 at session start (scale-invariant).
- **Hypothetical equity curve** — cumulative forward-60s directional return,
  gross vs **net of the per-symbol round-trip cost model**, computed with the
  *same look-ahead-safe gates* as `engine_backtest`. Explicitly labeled
  hypothetical: **signals are not traded.**
- **Signal analytics** — momentum mix by symbol, trade-quality grade donut, confidence histogram.
- **Confidence calibration** — does higher confidence → higher realized accuracy?
  (stale/neutral excluded, reference line at the 50% coin-flip).
- **Feed health** — per-symbol last tick, age, and frozen-feed detection (mirrors `validation/isStale`).
- **Hermes reports** — renders the daily markdown reports.

## Run

```bash
cd dashboard
npm install
npm run dev          # http://localhost:3000
```

### Data source

By default the dashboard shows a **deterministic DEMO session** (synthetic data
derived with the engine's exact formulas) so it renders richly out of the box
and deploys anywhere. To show your real data:

```bash
DASHBOARD_DB_PATH=../data/engine.db npm run dev
```

A banner always indicates whether you're viewing LIVE or DEMO data. Optional:
`DASHBOARD_REPORTS_DIR` overrides the Hermes reports location
(default `../agent/hermes/reports`).

## Architecture notes

- **Server Components** call the analytics layer directly (no client fetch
  waterfall); chart components are the only `"use client"` boundary.
- **API routes** (`/api/overview`, `/api/prices`, `/api/signals`, `/api/equity`,
  `/api/health`, `/api/reports`) expose the same JSON for external use/polling.
- `better-sqlite3` is an **optional**, lazily-required dependency — DEMO mode
  works even if the native module isn't built.
- Pure analytics (`lib/analytics.ts`) and shared quant formulas (`lib/quant.ts`)
  are framework-free and unit-testable.

## Deploy (optional)

Builds as a standard Next.js app. For a live demo link, commit a small sample
`engine.db` and set `DASHBOARD_DB_PATH`, or just deploy as-is (DEMO mode).
