// Deterministic DEMO dataset.
//
// Your live engine.db is tiny (it has only run briefly), so charting it would
// look empty. This generator fabricates ONE realistic trading session and
// derives signals/quality with the SAME formulas as the C++ engine (see
// quant.ts), so the dashboard looks alive and is screenshot-ready out of the
// box. It is clearly watermarked "DEMO DATA" in the UI. Point DASHBOARD_DB_PATH
// at a real engine.db to replace it with live data.
//
// Deterministic (seeded PRNG) so every build/screenshot is identical.

import {
  classifyVolatility,
  computeConfidence,
  detectSession,
  gradeFor,
  sessionMultiplier,
} from "./quant";
import type { Dataset, Quality, Signal, Tick } from "./types";

const WINDOW = 10;
const STEP_S = 30;

// 2026-05-27 is a Wednesday. Cover Asia tail (07-08) → London (08-13) →
// London+NY overlap (13-17) → NewYork (17-21) for a full-session spread.
const DAY_START_UTC = Date.UTC(2026, 4, 27, 7, 0, 0) / 1000; // seconds
const N_CYCLES = 14 * 60 * 2; // 14h × 120 cycles/h = 1680

interface SymCfg {
  symbol: string;
  start: number;
  sigma: number; // per-step relative vol (std of log return)
  freeze?: [number, number]; // optional [startCycle, endCycle) frozen feed
}

const SYMBOLS: SymCfg[] = [
  { symbol: "EUR/USD", start: 1.085, sigma: 0.00009 },
  { symbol: "XAU/USD", start: 2350.0, sigma: 0.00022 },
  { symbol: "USO", start: 78.5, sigma: 0.00035, freeze: [880, 892] }, // demo a stale feed
];

function mulberry32(seed: number) {
  let a = seed | 0;
  return function () {
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function gauss(rng: () => number): number {
  // Box-Muller.
  let u = 0;
  let v = 0;
  while (u === 0) u = rng();
  while (v === 0) v = rng();
  return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
}

function stddevLogReturns(prices: number[]): number {
  if (prices.length < 2) return 0;
  const rets: number[] = [];
  for (let i = 1; i < prices.length; i++) {
    rets.push(Math.log(prices[i] / prices[i - 1]));
  }
  const mean = rets.reduce((s, r) => s + r, 0) / rets.length;
  const variance =
    rets.reduce((s, r) => s + (r - mean) * (r - mean), 0) / rets.length;
  return Math.sqrt(variance);
}

function buildSymbol(cfg: SymCfg): {
  ticks: Tick[];
  signals: Signal[];
  quality: Quality[];
} {
  // Per-symbol seed so each instrument is independent but reproducible.
  const seed =
    cfg.symbol.split("").reduce((s, c) => s + c.charCodeAt(0), 0) * 7919;
  const rng = mulberry32(seed);

  const ticks: Tick[] = [];
  const signals: Signal[] = [];
  const quality: Quality[] = [];

  const window: number[] = [];
  let price = cfg.start;
  let prevRet = 0;
  let volMult = 1;

  for (let i = 0; i < N_CYCLES; i++) {
    const ts = DAY_START_UTC + i * STEP_S;
    const frozen = cfg.freeze && i >= cfg.freeze[0] && i < cfg.freeze[1];

    if (!frozen) {
      // Volatility clustering: volMult random-walks around 1, occasional bursts.
      volMult = Math.max(0.4, volMult * 0.97 + 0.03 + (rng() - 0.5) * 0.4);
      // Mild momentum persistence (phi) so momentum is weakly predictive —
      // gives the hypothetical equity curve realistic structure. DEMO only.
      const ret = 0.15 * prevRet + cfg.sigma * volMult * gauss(rng);
      prevRet = ret;
      price = price * Math.exp(ret);
    }
    // When frozen, price is left unchanged → triggers staleness detection.

    const rounded = Number(price.toFixed(cfg.symbol === "EUR/USD" ? 5 : 3));
    ticks.push({ ts, symbol: cfg.symbol, price: rounded });

    window.push(rounded);
    if (window.length > WINDOW) window.shift();

    // Momentum: last vs previous, ±1e-7 dead-zone (momentum.cpp).
    let momentum: Signal["momentum"] = "Neutral";
    if (window.length >= 2) {
      const diff = window[window.length - 1] - window[window.length - 2];
      if (diff > 1e-7) momentum = "Bullish";
      else if (diff < -1e-7) momentum = "Bearish";
    }

    const vol = stddevLogReturns(window);
    const regime = classifyVolatility(vol);

    // Staleness: last 3 prices equal within 1e-9 (validation.cpp::isStale).
    let stale = false;
    if (window.length >= 3) {
      const a = window[window.length - 1];
      const b = window[window.length - 2];
      const c = window[window.length - 3];
      stale = Math.abs(a - b) <= 1e-9 && Math.abs(a - c) <= 1e-9;
    }

    const confidence = computeConfidence({
      stale,
      sampleCount: window.length,
      windowTarget: WINDOW,
      momentum,
      volRegime: regime,
    });
    const session = detectSession(ts);
    const tq = Math.max(
      0,
      Math.min(100, Math.round(confidence * sessionMultiplier(session))),
    );

    signals.push({ ts, symbol: cfg.symbol, momentum, vol_score: vol, vol_regime: regime });
    quality.push({
      ts,
      symbol: cfg.symbol,
      stale: stale ? 1 : 0,
      session,
      confidence,
      trade_quality: tq,
      grade: gradeFor(tq),
    });
  }

  return { ticks, signals, quality };
}

let cached: Dataset | null = null;

export function demoDataset(): Dataset {
  if (cached) return cached;
  const ticks: Tick[] = [];
  const signals: Signal[] = [];
  const quality: Quality[] = [];
  for (const cfg of SYMBOLS) {
    const r = buildSymbol(cfg);
    ticks.push(...r.ticks);
    signals.push(...r.signals);
    quality.push(...r.quality);
  }
  ticks.sort((a, b) => a.ts - b.ts);
  signals.sort((a, b) => a.ts - b.ts);
  quality.sort((a, b) => a.ts - b.ts);
  cached = { ticks, signals, quality, source: "demo", dbPath: null };
  return cached;
}
