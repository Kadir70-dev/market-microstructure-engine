// Pure analytics over an in-memory Dataset. No DB access here — db.ts loads,
// this computes. The forward-return logic mirrors engine_backtest's gates
// (baseline at/before, future at/after, exclude stale/neutral/missing) so the
// dashboard's "hypothetical equity" is consistent with the C++ harness.

import { roundTripCost } from "./quant";
import type { Dataset, Quality, Signal, Tick } from "./types";

const HORIZON_S = 60;
const BASELINE_TOL_S = 120;
const FUTURE_TOL_S = 120;

function bySymbol<T extends { symbol: string }>(rows: T[]): Map<string, T[]> {
  const m = new Map<string, T[]>();
  for (const r of rows) {
    const arr = m.get(r.symbol);
    if (arr) arr.push(r);
    else m.set(r.symbol, [r]);
  }
  return m;
}

// Sorted-ascending tick array assumed. Rightmost tick at/before target, then
// tolerance-check: returns its price only if within `tol` seconds, else null.
function toleranceBefore(ticks: Tick[], target: number, tol: number): number | null {
  let lo = 0;
  let hi = ticks.length - 1;
  let idx = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (ticks[mid].ts <= target) {
      idx = mid;
      lo = mid + 1;
    } else hi = mid - 1;
  }
  if (idx < 0) return null;
  return target - ticks[idx].ts <= tol ? ticks[idx].price : null;
}

function priceAtOrAfter(ticks: Tick[], target: number, tol: number): number | null {
  let lo = 0;
  let hi = ticks.length - 1;
  let idx = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (ticks[mid].ts >= target) {
      idx = mid;
      hi = mid - 1;
    } else lo = mid + 1;
  }
  if (idx < 0) return null;
  return ticks[idx].ts - target <= tol ? ticks[idx].price : null;
}

// ---- Overview KPIs -------------------------------------------------------
export function overview(ds: Dataset) {
  const symbols = Array.from(new Set(ds.ticks.map((t) => t.symbol))).sort();
  const qBySym = ds.quality;
  const stale = qBySym.filter((q) => q.stale).length;
  const mom = { Bullish: 0, Bearish: 0, Neutral: 0 } as Record<string, number>;
  for (const s of ds.signals) mom[s.momentum] = (mom[s.momentum] || 0) + 1;
  const confs = qBySym.map((q) => q.confidence);
  const meanConf = confs.length
    ? confs.reduce((a, b) => a + b, 0) / confs.length
    : 0;
  const tsAll = ds.ticks.map((t) => t.ts);
  return {
    source: ds.source,
    dbPath: ds.dbPath,
    symbols,
    nTicks: ds.ticks.length,
    nSignals: ds.signals.length,
    nQuality: qBySym.length,
    stale,
    stalePct: qBySym.length ? (100 * stale) / qBySym.length : 0,
    momentum: mom,
    meanConfidence: meanConf,
    firstTs: tsAll.length ? Math.min(...tsAll) : null,
    lastTs: tsAll.length ? Math.max(...tsAll) : null,
  };
}

// ---- Price series (downsampled for charting) -----------------------------
export function priceSeries(ds: Dataset, maxPoints = 300) {
  const map = bySymbol(ds.ticks);
  const out: Record<string, { ts: number; price: number }[]> = {};
  for (const [sym, ticks] of map) {
    const step = Math.max(1, Math.floor(ticks.length / maxPoints));
    const series: { ts: number; price: number }[] = [];
    for (let i = 0; i < ticks.length; i += step) {
      series.push({ ts: ticks[i].ts, price: ticks[i].price });
    }
    out[sym] = series;
  }
  return out;
}

// ---- Signal analytics ----------------------------------------------------
export function signalAnalytics(ds: Dataset) {
  const perSymbol: Record<
    string,
    { bull: number; bear: number; neutral: number; n: number }
  > = {};
  for (const s of ds.signals) {
    const e = (perSymbol[s.symbol] ??= { bull: 0, bear: 0, neutral: 0, n: 0 });
    e.n++;
    if (s.momentum === "Bullish") e.bull++;
    else if (s.momentum === "Bearish") e.bear++;
    else e.neutral++;
  }

  const grades = { A: 0, B: 0, C: 0, D: 0 } as Record<string, number>;
  for (const q of ds.quality) grades[q.grade] = (grades[q.grade] || 0) + 1;

  const regimes = { LOW: 0, MEDIUM: 0, HIGH: 0 } as Record<string, number>;
  for (const s of ds.signals) regimes[s.vol_regime] = (regimes[s.vol_regime] || 0) + 1;

  const sessions: Record<string, number> = {};
  for (const q of ds.quality) sessions[q.session] = (sessions[q.session] || 0) + 1;

  // Confidence histogram (10-wide bins 0..100).
  const bins = Array.from({ length: 10 }, (_, i) => ({
    label: `${i * 10}-${i * 10 + 9}`,
    count: 0,
  }));
  for (const q of ds.quality) {
    const idx = Math.min(9, Math.floor(q.confidence / 10));
    bins[idx].count++;
  }

  return { perSymbol, grades, regimes, sessions, confidenceHistogram: bins };
}

// ---- Forward-return engine (shared by calibration + equity) --------------
interface FwdObs {
  ts: number;
  symbol: string;
  confidence: number;
  raw: number; // (future-baseline)/baseline
  dirNet: number; // sign-adjusted return minus round-trip cost
  dirGross: number; // sign-adjusted return
  correct: boolean;
}

function forwardObservations(ds: Dataset): FwdObs[] {
  const ticksBySym = bySymbol(ds.ticks);
  const qByKey = new Map<string, Quality>();
  for (const q of ds.quality) qByKey.set(`${q.ts}|${q.symbol}`, q);

  const obs: FwdObs[] = [];
  for (const s of ds.signals) {
    if (s.momentum === "Neutral") continue;
    const q = qByKey.get(`${s.ts}|${s.symbol}`);
    if (!q || q.stale) continue;
    const ticks = ticksBySym.get(s.symbol);
    if (!ticks) continue;

    const base = toleranceBefore(ticks, s.ts, BASELINE_TOL_S);
    const fut = priceAtOrAfter(ticks, s.ts + HORIZON_S, FUTURE_TOL_S);
    if (base === null || fut === null || base === 0) continue;

    const raw = (fut - base) / base;
    const sign = s.momentum === "Bullish" ? 1 : -1;
    const dirGross = sign * raw;
    const cost = roundTripCost(s.symbol);
    obs.push({
      ts: s.ts,
      symbol: s.symbol,
      confidence: q.confidence,
      raw,
      dirGross,
      dirNet: dirGross - cost,
      correct: dirGross > 0,
    });
  }
  obs.sort((a, b) => a.ts - b.ts);
  return obs;
}

// ---- Confidence calibration (the quant/ML chart) -------------------------
export function confidenceCalibration(ds: Dataset) {
  const obs = forwardObservations(ds);
  const bands = [
    { band: "Low (0-29)", lo: 0, hi: 29 },
    { band: "Medium (30-59)", lo: 30, hi: 59 },
    { band: "High (60-100)", lo: 60, hi: 100 },
  ];
  return bands.map((b) => {
    const inBand = obs.filter((o) => o.confidence >= b.lo && o.confidence <= b.hi);
    const n = inBand.length;
    const correct = inBand.filter((o) => o.correct).length;
    return {
      band: b.band,
      n,
      accuracy: n ? (100 * correct) / n : null,
    };
  });
}

// ---- Hypothetical equity curve (signals NOT traded — labeled in UI) ------
export function equityCurve(ds: Dataset) {
  const obs = forwardObservations(ds);
  let cumGross = 0;
  let cumNet = 0;
  const points = obs.map((o) => {
    cumGross += o.dirGross;
    cumNet += o.dirNet;
    return {
      ts: o.ts,
      gross: cumGross * 100, // express cumulative return in %
      net: cumNet * 100,
    };
  });
  const n = obs.length;
  const wins = obs.filter((o) => o.correct).length;
  return {
    points,
    stats: {
      n,
      hitRate: n ? (100 * wins) / n : 0,
      cumGrossPct: cumGross * 100,
      cumNetPct: cumNet * 100,
    },
  };
}

// ---- DB-derived health (mirrors ops/health_check.py spirit) --------------
export function health(ds: Dataset) {
  const nowS = Math.floor(Date.now() / 1000);
  const map = bySymbol(ds.ticks);
  const perSymbol = Array.from(map.entries()).map(([sym, ticks]) => {
    const last = ticks[ticks.length - 1];
    const lastN = ticks.slice(-5).map((t) => t.price);
    const frozen =
      lastN.length >= 5 && new Set(lastN).size === 1;
    return {
      symbol: sym,
      ticks: ticks.length,
      lastPrice: last?.price ?? null,
      lastTs: last?.ts ?? null,
      ageS: last ? nowS - last.ts : null,
      frozen,
    };
  });
  const anyStale = ds.quality.some((q) => q.stale);
  return {
    source: ds.source,
    nTicks: ds.ticks.length,
    perSymbol,
    staleFlagsPresent: anyStale,
  };
}
