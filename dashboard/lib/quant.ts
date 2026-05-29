// Shared quant helpers that mirror the C++ engine exactly. Kept in one place so
// the demo generator and the analytics layer agree byte-for-byte with
// validation.cpp / evaluation/main.cpp.

export type Session =
  | "Closed"
  | "Asia"
  | "London"
  | "London+NY"
  | "NewYork";

// UTC-based session detection — mirrors validation.cpp::detectSession (no DST).
export function detectSession(tsSeconds: number): Session {
  const d = new Date(tsSeconds * 1000);
  const wday = d.getUTCDay(); // 0=Sun .. 6=Sat
  const hour = d.getUTCHours();

  // Forex weekend: Fri 22:00 UTC -> Sun 22:00 UTC.
  if (wday === 6) return "Closed";
  if (wday === 5 && hour >= 22) return "Closed";
  if (wday === 0 && hour < 22) return "Closed";

  if (hour >= 22) return "Asia";
  if (hour < 8) return "Asia";
  if (hour < 13) return "London";
  if (hour < 17) return "London+NY";
  return "NewYork";
}

export function sessionMultiplier(s: Session): number {
  switch (s) {
    case "Closed":
      return 0.0;
    case "Asia":
      return 0.6;
    case "London":
      return 1.0;
    case "London+NY":
      return 1.0;
    case "NewYork":
      return 0.95;
  }
}

export function gradeFor(score: number): "A" | "B" | "C" | "D" {
  if (score >= 75) return "A";
  if (score >= 50) return "B";
  if (score >= 25) return "C";
  return "D";
}

// Per-symbol round-trip cost as a fraction of price — mirrors the cost table in
// evaluation/main.cpp.
export function roundTripCost(symbol: string): number {
  switch (symbol) {
    case "EUR/USD":
      return 0.00015;
    case "USO":
      return 0.0002;
    case "XAU/USD":
      return 0.00015;
    default:
      return 0.0003;
  }
}

// Volatility regime from stddev of log-returns — mirrors volatility.cpp.
export function classifyVolatility(stddev: number): "LOW" | "MEDIUM" | "HIGH" {
  if (stddev > 0.001) return "HIGH";
  if (stddev > 0.0003) return "MEDIUM";
  return "LOW";
}

// Confidence (0-100) — mirrors validation.cpp::computeConfidence.
export function computeConfidence(opts: {
  stale: boolean;
  sampleCount: number;
  windowTarget: number;
  momentum: string;
  volRegime: string;
}): number {
  if (opts.stale) return 0;
  let base = 100.0;
  if (opts.sampleCount < opts.windowTarget && opts.windowTarget > 0) {
    base *= opts.sampleCount / opts.windowTarget;
  }
  if (opts.volRegime === "HIGH") base *= 0.5;
  else if (opts.volRegime === "MEDIUM") base *= 0.8;
  if (opts.momentum === "Neutral") base *= 0.3;
  return Math.max(0, Math.min(100, Math.round(base)));
}
