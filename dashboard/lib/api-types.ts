export type DataSource = "live" | "demo";
export type ApiErrorCode = "not_found" | "internal_error" | "bad_request";
export interface ErrorPayload { error: { code: ApiErrorCode; message: string } }
export interface Provenance { source: DataSource; sourceReason: string | null }
export interface OverviewPayload extends Provenance {
  dbPath: string | null; symbols: string[]; nTicks: number; nSignals: number;
  nQuality: number; stale: number; stalePct: number;
  momentum: { Bullish: number; Bearish: number; Neutral: number };
  meanConfidence: number; firstTs: number | null; lastTs: number | null;
}
export interface PricePoint { ts: number; price: number }
export type PricesPayload = Record<string, PricePoint[]>;
export interface SignalsPayload {
  perSymbol: Record<string, { bull: number; bear: number; neutral: number; n: number }>;
  grades: { A: number; B: number; C: number; D: number };
  regimes: { LOW: number; MEDIUM: number; HIGH: number };
  sessions: Record<"Asia" | "London" | "LondonNY" | "NewYork" | "Closed", number>;
  confidenceHistogram: { label: string; count: number }[];
  calibration: { band: string; n: number; accuracy: number | null }[];
}
export interface EquityPayload {
  points: { ts: number; gross: number; net: number }[];
  stats: { n: number; hitRate: number; cumGrossPct: number; cumNetPct: number };
}
export interface PlannedRiskPayload extends Provenance {
  hypothetical: { maxDrawdownPct: number; stddevPct: number; hitRate: number; grossPct: number; netPct: number; costDragPct: number; n: number };
  realizedVolatility: Record<string, { stddev: number; regime: "LOW" | "MEDIUM" | "HIGH" }>;
  positions: null;
}
export interface ModelPayload {
  source: "synthetic" | "live" | "db"; trainable?: boolean; reason?: string;
  real_db_reason?: string | null; model?: string; n_samples?: number; n_oos?: number; n_features?: number;
  metrics?: { auc: number; auc_folds: number[]; precision: number; recall: number; f1: number; brier: number; baseline_auc: number; permutation_auc_mean: number; permutation_auc_std: number; p_value: number | null };
  calibration?: { mean_pred: number; frac_pos: number }[];
  feature_importance?: { feature: string; coef: number; abs: number }[];
  net_of_cost?: { threshold_curve: { margin: number; n_trades: number; gross_pct: number; cost_pct: number; net_pct: number; hit_rate: number }[] };
  verdict: string;
}
export interface ReportsPayload { dates: string[] }
export interface ReportPayload { date: string; markdown: string }
export interface HealthPayload extends Provenance {
  nTicks: number;
  perSymbol: { symbol: string; ticks: number; lastPrice: number | null; lastTs: number | null; ageS: number | null; frozen: boolean }[];
  staleFlagsPresent: boolean;
}
