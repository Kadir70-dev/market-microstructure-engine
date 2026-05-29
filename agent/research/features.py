"""Feature matrix construction.

Builds a per-(ts, symbol) feature matrix with a LOOK-AHEAD-SAFE forward-60s
directional label, using the same baseline/future-tick gates as
engine_backtest. Works identically on live SQLite data and on the synthetic
session, so nothing about the modelling is special-cased to demo data.
"""

from __future__ import annotations

import sqlite3
from pathlib import Path

import numpy as np
import pandas as pd

from . import quant

HORIZON_S = 60
BASELINE_TOL_S = 120
FUTURE_TOL_S = 120

VOL_ORD = {"LOW": 0, "MEDIUM": 1, "HIGH": 2}
GRADE_ORD = {"D": 0, "C": 1, "B": 2, "A": 3}
SESSIONS = ["Asia", "London", "London+NY", "NewYork", "Closed"]

FEATURE_COLS = [
    "mom_ret", "mom_sign", "vol_score", "vol_regime_ord",
    "confidence", "trade_quality", "grade_ord",
    "roll_mean_5", "roll_std_5", "roll_ret_sum_3", "cost",
] + [f"sess_{s}" for s in SESSIONS]


def load_sqlite(db_path: str | Path) -> tuple[pd.DataFrame, pd.DataFrame, pd.DataFrame]:
    conn = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    try:
        ticks = pd.read_sql_query("SELECT ts, symbol, price FROM ticks ORDER BY ts", conn)
        signals = pd.read_sql_query(
            "SELECT ts, symbol, momentum, vol_score, vol_regime FROM signals ORDER BY ts", conn
        )
        quality = pd.read_sql_query(
            "SELECT ts, symbol, stale, session, confidence, trade_quality, grade "
            "FROM quality_scores ORDER BY ts",
            conn,
        )
    finally:
        conn.close()
    return ticks, signals, quality


def build_matrix(
    ticks: pd.DataFrame, signals: pd.DataFrame, quality: pd.DataFrame
) -> pd.DataFrame:
    rows = []
    for sym, tg in ticks.groupby("symbol"):
        tg = tg.sort_values("ts").reset_index(drop=True)
        ts_arr = tg["ts"].to_numpy()
        px = tg["price"].to_numpy(dtype=float)
        if len(px) < 4:
            continue
        # Log returns; r[0] = 0.
        r = np.zeros(len(px))
        r[1:] = np.log(px[1:] / px[:-1])

        sig = signals[signals["symbol"] == sym].set_index("ts")
        qual = quality[quality["symbol"] == sym].set_index("ts")
        cost = quant.round_trip_cost(sym)
        ts_to_idx = {int(t): i for i, t in enumerate(ts_arr)}

        for i, t in enumerate(ts_arr):
            t = int(t)
            if t not in sig.index or t not in qual.index:
                continue
            s = sig.loc[t]
            q = qual.loc[t]
            # pandas can return a frame on duplicate ts; take first row.
            if isinstance(s, pd.DataFrame):
                s = s.iloc[0]
            if isinstance(q, pd.DataFrame):
                q = q.iloc[0]

            # Forward-60s label (look-ahead-safe). Baseline = this tick.
            base = px[i]
            j = np.searchsorted(ts_arr, t + HORIZON_S, side="left")
            if j >= len(ts_arr) or ts_arr[j] - (t + HORIZON_S) > FUTURE_TOL_S:
                continue  # no future tick within tolerance -> exclude
            fut = px[j]
            fwd_ret = (fut - base) / base if base else 0.0

            # Exclusions mirror the backtest: stale + neutral are non-trades.
            if int(q["stale"]) == 1 or s["momentum"] == "Neutral":
                continue

            # Rolling stats over recent returns (cheap, causal).
            lo5 = max(0, i - 4)
            lo3 = max(0, i - 2)
            roll_mean_5 = float(r[lo5 : i + 1].mean())
            roll_std_5 = float(r[lo5 : i + 1].std())
            roll_ret_sum_3 = float(r[lo3 : i + 1].sum())

            rows.append(
                {
                    "ts": t,
                    "symbol": sym,
                    "mom_ret": float(r[i]),
                    "mom_sign": 1.0 if r[i] > 0 else (-1.0 if r[i] < 0 else 0.0),
                    "vol_score": float(s["vol_score"]),
                    "vol_regime_ord": VOL_ORD.get(s["vol_regime"], 0),
                    "confidence": float(q["confidence"]),
                    "trade_quality": float(q["trade_quality"]),
                    "grade_ord": GRADE_ORD.get(q["grade"], 0),
                    "roll_mean_5": roll_mean_5,
                    "roll_std_5": roll_std_5,
                    "roll_ret_sum_3": roll_ret_sum_3,
                    "cost": cost,
                    **{f"sess_{ss}": 1.0 if q["session"] == ss else 0.0 for ss in SESSIONS},
                    "fwd_ret": fwd_ret,
                    "y": 1 if fwd_ret > 0 else 0,
                }
            )

    df = pd.DataFrame(rows)
    if not df.empty:
        df = df.sort_values("ts").reset_index(drop=True)
    return df


def trainability(df: pd.DataFrame, min_rows: int = 200) -> tuple[bool, str]:
    """Honest gate: can this data support a model at all?"""
    if df.empty:
        return False, "no labeled rows (no non-neutral, non-stale signals with a future tick)"
    if len(df) < min_rows:
        return False, f"only {len(df)} labeled rows (need >= {min_rows})"
    classes = df["y"].nunique()
    if classes < 2:
        return False, f"label has a single class (no directional variance)"
    informative = [c for c in FEATURE_COLS if df[c].nunique() > 1]
    if len(informative) < 3:
        return False, f"only {len(informative)} features have variance"
    return True, f"{len(df)} rows, {len(informative)} informative features, both classes present"
