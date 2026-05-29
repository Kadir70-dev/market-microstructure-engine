// Row shapes mirror the engine's SQLite schema (storage/sqlite_logger.cpp).
export interface Tick {
  ts: number; // unix seconds
  symbol: string;
  price: number;
}

export interface Signal {
  ts: number;
  symbol: string;
  momentum: "Bullish" | "Bearish" | "Neutral";
  vol_score: number;
  vol_regime: "LOW" | "MEDIUM" | "HIGH";
}

export interface Quality {
  ts: number;
  symbol: string;
  stale: number; // 0/1
  session: string;
  confidence: number; // 0-100
  trade_quality: number; // 0-100
  grade: "A" | "B" | "C" | "D";
}

export interface Dataset {
  ticks: Tick[];
  signals: Signal[];
  quality: Quality[];
  source: "live" | "demo";
  dbPath: string | null;
}
