// Read-only loader for the predictive-baseline results produced by
// agent/research/train.py. Falls back to a bundled snapshot so the Prediction
// page renders even when deployed without the research output present.

import fs from "fs";
import path from "path";

const MODEL_PATH =
  process.env.DASHBOARD_MODEL_PATH ||
  path.resolve(process.cwd(), "..", "agent", "research", "results", "model_results.json");

export interface ModelResults {
  source: string;
  real_db_reason?: string | null;
  trainable?: boolean;
  reason?: string;
  n_samples?: number;
  n_oos?: number;
  n_features?: number;
  model?: string;
  metrics?: {
    auc: number;
    auc_folds: number[];
    precision: number;
    recall: number;
    f1: number;
    brier: number;
    baseline_auc: number;
    permutation_auc_mean: number;
    permutation_auc_std: number;
    p_value: number | null;
  };
  calibration?: { mean_pred: number; frac_pos: number }[];
  feature_importance?: { feature: string; coef: number; abs: number }[];
  net_of_cost?: {
    threshold_curve: {
      margin: number;
      n_trades: number;
      gross_pct: number;
      cost_pct: number;
      net_pct: number;
      hit_rate: number;
    }[];
  };
  verdict: string;
}

export function loadModelResults(): ModelResults {
  try {
    if (fs.existsSync(MODEL_PATH)) {
      return JSON.parse(fs.readFileSync(MODEL_PATH, "utf-8"));
    }
  } catch {
    /* fall through to bundled */
  }
  return FALLBACK;
}

// Snapshot of a representative run (synthetic source) so the page never blanks.
const FALLBACK: ModelResults = {
  source: "synthetic",
  n_samples: 4570,
  n_oos: 3932,
  n_features: 16,
  model: "LogisticRegression (StandardScaler) + walk-forward",
  metrics: {
    auc: 0.545,
    auc_folds: [0.54, 0.55, 0.53, 0.56, 0.54],
    precision: 0.53,
    recall: 0.58,
    f1: 0.55,
    brier: 0.249,
    baseline_auc: 0.5,
    permutation_auc_mean: 0.498,
    permutation_auc_std: 0.009,
    p_value: 0.032,
  },
  calibration: [
    { mean_pred: 0.46, frac_pos: 0.47 },
    { mean_pred: 0.49, frac_pos: 0.49 },
    { mean_pred: 0.51, frac_pos: 0.52 },
    { mean_pred: 0.54, frac_pos: 0.55 },
  ],
  feature_importance: [
    { feature: "mom_sign", coef: 0.149, abs: 0.149 },
    { feature: "sess_Asia", coef: -0.107, abs: 0.107 },
    { feature: "grade_ord", coef: -0.093, abs: 0.093 },
    { feature: "trade_quality", coef: 0.08, abs: 0.08 },
    { feature: "cost", coef: -0.075, abs: 0.075 },
    { feature: "roll_mean_5", coef: -0.07, abs: 0.07 },
  ],
  net_of_cost: {
    threshold_curve: [
      { margin: 0.0, n_trades: 3932, gross_pct: 8.46, cost_pct: 65.5, net_pct: -57.05, hit_rate: 52.9 },
      { margin: 0.02, n_trades: 3132, gross_pct: 6.32, cost_pct: 52.4, net_pct: -46.11, hit_rate: 53.1 },
      { margin: 0.05, n_trades: 1966, gross_pct: 4.56, cost_pct: 33.7, net_pct: -29.18, hit_rate: 53.0 },
      { margin: 0.1, n_trades: 697, gross_pct: 3.44, cost_pct: 12.9, net_pct: -9.44, hit_rate: 53.8 },
    ],
  },
  verdict:
    "AUC 0.545 (above chance, perm p=0.032). Gross edge +8.5% but net-of-cost -57% over 3932 trades — does NOT survive costs. No deployable edge.",
};
