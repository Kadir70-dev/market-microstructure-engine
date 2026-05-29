"""Honest predictive baseline: next-direction probability via Logistic Regression
with walk-forward (time-ordered) validation.

Outputs results/model_results.json (consumed by the dashboard) plus calibration,
feature-importance, and net-of-cost equity plots.

Usage (from repo root, with the venv active):
    python -m agent.research.train                     # auto: live DB if trainable, else synthetic
    python -m agent.research.train --source synthetic
    python -m agent.research.train --source db --db data/engine.db

Design choices (all deliberate, see README):
  * Logistic Regression, not a GBM — interpretable, calibrated, won't overfit.
  * Walk-forward expanding window, NOT random CV — no look-ahead leakage.
  * Probability output + calibration curve, not binary predictions.
  * Net-of-cost evaluation — gross edge is meaningless until costs are paid.
  * Permutation test — proves whether AUC is above chance, or honestly isn't.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

from . import features as F
from .features import FEATURE_COLS

N_FOLDS = 5
PERMUTATIONS = 30
REPO_ROOT = Path(__file__).resolve().parents[2]


def _walk_forward(X: np.ndarray, y: np.ndarray, ts_order: np.ndarray):
    """Expanding-window walk-forward. Returns (oos_idx, oos_proba) pooled across
    folds 1..N (fold 0 is the initial train-only block)."""
    from sklearn.linear_model import LogisticRegression
    from sklearn.pipeline import make_pipeline
    from sklearn.preprocessing import StandardScaler

    order = np.argsort(ts_order, kind="stable")
    bounds = np.linspace(0, len(order), N_FOLDS + 1, dtype=int)
    oos_idx, oos_proba = [], []
    fold_aucs = []
    from sklearn.metrics import roc_auc_score

    for k in range(1, N_FOLDS + 1):
        tr = order[: bounds[k - 1]]  # all rows before this fold's test block
        te = order[bounds[k - 1] : bounds[k]]
        if len(tr) < 30 or len(te) == 0 or len(np.unique(y[tr])) < 2:
            continue
        model = make_pipeline(
            StandardScaler(), LogisticRegression(max_iter=1000, C=1.0)
        )
        model.fit(X[tr], y[tr])
        p = model.predict_proba(X[te])[:, 1]
        oos_idx.extend(te.tolist())
        oos_proba.extend(p.tolist())
        if len(np.unique(y[te])) >= 2:
            fold_aucs.append(float(roc_auc_score(y[te], p)))
    return np.array(oos_idx), np.array(oos_proba), fold_aucs


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", choices=["auto", "db", "synthetic"], default="auto")
    ap.add_argument("--db", default=str(REPO_ROOT / "data" / "engine.db"))
    ap.add_argument("--out", default=str(Path(__file__).resolve().parent / "results"))
    ap.add_argument("--min-rows", type=int, default=200)
    args = ap.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    # ---- Load + decide source honestly ----------------------------------
    real_reason = None
    source = args.source
    df = None

    def load_db():
        ticks, signals, quality = F.load_sqlite(args.db)
        return F.build_matrix(ticks, signals, quality)

    def load_syn():
        from .synthetic import load_synthetic
        ticks, signals, quality = load_synthetic()
        return F.build_matrix(ticks, signals, quality)

    if source in ("auto", "db"):
        df_db = load_db()
        ok, reason = F.trainability(df_db, args.min_rows)
        real_reason = reason
        if ok:
            df, source = df_db, "live"
        elif source == "db":
            print(f"[train] live DB NOT trainable: {reason}")
            # still emit an honest verdict file
            _emit_untrainable(out_dir, reason)
            return 0
    if df is None:
        df = load_syn()
        source = "synthetic"

    ok, reason = F.trainability(df, args.min_rows)
    if not ok:
        _emit_untrainable(out_dir, reason)
        print(f"[train] not trainable: {reason}")
        return 0

    X = df[FEATURE_COLS].to_numpy(dtype=float)
    y = df["y"].to_numpy(dtype=int)
    ts = df["ts"].to_numpy()

    from sklearn.metrics import (
        brier_score_loss,
        precision_recall_fscore_support,
        roc_auc_score,
    )

    oos_idx, oos_proba, fold_aucs = _walk_forward(X, y, ts)
    y_oos = y[oos_idx]
    fwd_oos = df["fwd_ret"].to_numpy()[oos_idx]
    cost_oos = df["cost"].to_numpy()[oos_idx]
    sym_oos = df["symbol"].to_numpy()[oos_idx]
    ts_oos = ts[oos_idx]

    auc = float(roc_auc_score(y_oos, oos_proba))
    pred = (oos_proba >= 0.5).astype(int)
    prec, rec, f1, _ = precision_recall_fscore_support(
        y_oos, pred, average="binary", zero_division=0
    )
    brier = float(brier_score_loss(y_oos, oos_proba))

    # ---- Permutation test: is AUC above chance? -------------------------
    rng = np.random.default_rng(12345)
    perm_aucs = []
    for _ in range(PERMUTATIONS):
        yp = rng.permutation(y)
        pidx, pproba, _ = _walk_forward(X, yp, ts)
        if len(np.unique(yp[pidx])) >= 2:
            perm_aucs.append(float(roc_auc_score(yp[pidx], pproba)))
    perm_mean = float(np.mean(perm_aucs)) if perm_aucs else 0.5
    perm_std = float(np.std(perm_aucs)) if perm_aucs else 0.0
    p_value = (
        (1 + sum(1 for a in perm_aucs if a >= auc)) / (1 + len(perm_aucs))
        if perm_aucs
        else None
    )

    # ---- Calibration curve ----------------------------------------------
    from sklearn.calibration import calibration_curve
    frac_pos, mean_pred = calibration_curve(y_oos, oos_proba, n_bins=10, strategy="quantile")
    calibration = [
        {"mean_pred": float(mp), "frac_pos": float(fp)}
        for mp, fp in zip(mean_pred, frac_pos)
    ]

    # ---- Feature importance (standardized LR coefficients) --------------
    from sklearn.linear_model import LogisticRegression
    from sklearn.pipeline import make_pipeline
    from sklearn.preprocessing import StandardScaler

    full = make_pipeline(StandardScaler(), LogisticRegression(max_iter=1000, C=1.0))
    full.fit(X, y)
    coefs = full.named_steps["logisticregression"].coef_[0]
    importance = sorted(
        [{"feature": f, "coef": float(c), "abs": float(abs(c))} for f, c in zip(FEATURE_COLS, coefs)],
        key=lambda d: -d["abs"],
    )

    # ---- Net-of-cost evaluation -----------------------------------------
    net = _net_of_cost(oos_proba, fwd_oos, cost_oos)

    verdict = _verdict(auc, p_value, net)

    results = {
        "source": source,
        "real_db_reason": real_reason,
        "n_samples": int(len(df)),
        "n_oos": int(len(oos_idx)),
        "n_features": len(FEATURE_COLS),
        "period": {
            "start": int(ts.min()),
            "end": int(ts.max()),
        },
        "model": "LogisticRegression (StandardScaler) + walk-forward",
        "metrics": {
            "auc": auc,
            "auc_folds": fold_aucs,
            "precision": float(prec),
            "recall": float(rec),
            "f1": float(f1),
            "brier": brier,
            "baseline_auc": 0.5,
            "permutation_auc_mean": perm_mean,
            "permutation_auc_std": perm_std,
            "p_value": p_value,
        },
        "calibration": calibration,
        "feature_importance": importance,
        "net_of_cost": net,
        "verdict": verdict,
    }

    (out_dir / "model_results.json").write_text(json.dumps(results, indent=2))
    print(json.dumps({k: results[k] for k in ["source", "n_oos", "verdict"]}, indent=2))
    print(f"AUC={auc:.4f} (perm {perm_mean:.3f}±{perm_std:.3f}, p={p_value}) "
          f"net@thr0={net['threshold_curve'][0]['net_pct']:.3f}%")

    _make_plots(out_dir, calibration, importance, oos_proba, fwd_oos, cost_oos, ts_oos)
    print(f"[train] wrote {out_dir/'model_results.json'} + plots")
    return 0


def _net_of_cost(proba, fwd_ret, cost):
    """Trade when |p-0.5| > margin; position = sign(p-0.5); pnl = pos*fwd - cost."""
    out = []
    for margin in [0.0, 0.02, 0.05, 0.1]:
        mask = np.abs(proba - 0.5) > margin
        n = int(mask.sum())
        if n == 0:
            out.append({"margin": margin, "n_trades": 0, "gross_pct": 0.0,
                        "cost_pct": 0.0, "net_pct": 0.0, "hit_rate": 0.0})
            continue
        pos = np.sign(proba[mask] - 0.5)
        gross = float(np.sum(pos * fwd_ret[mask]) * 100)
        cst = float(np.sum(cost[mask]) * 100)
        hits = float(np.mean((pos * fwd_ret[mask]) > 0) * 100)
        out.append({
            "margin": margin, "n_trades": n,
            "gross_pct": gross, "cost_pct": cst, "net_pct": gross - cst,
            "hit_rate": hits,
        })
    return {"threshold_curve": out}


def _verdict(auc, p_value, net):
    net0 = net["threshold_curve"][0]
    sig = "above chance" if (p_value is not None and p_value < 0.05 and auc > 0.5) else "indistinguishable from chance"
    surv = net0["net_pct"] > 0
    return (
        f"AUC {auc:.3f} ({sig}, perm p={p_value}). "
        + (f"Gross edge {net0['gross_pct']:.2f}% but net-of-cost "
           f"{net0['net_pct']:.2f}% over {net0['n_trades']} trades — "
           + ("SURVIVES costs." if surv else "does NOT survive costs. No deployable edge."))
    )


def _emit_untrainable(out_dir: Path, reason: str):
    (out_dir / "model_results.json").write_text(json.dumps({
        "source": "live",
        "trainable": False,
        "reason": reason,
        "verdict": f"Live data is NOT trainable: {reason}. "
                   "Collect weeks of session-aligned ticks, then re-run.",
    }, indent=2))


def _make_plots(out_dir, calibration, importance, proba, fwd, cost, ts):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    # Calibration
    fig, ax = plt.subplots(figsize=(5, 4))
    mp = [c["mean_pred"] for c in calibration]
    fp = [c["frac_pos"] for c in calibration]
    ax.plot([0, 1], [0, 1], "--", color="#64748b", label="perfect")
    ax.plot(mp, fp, "o-", color="#a78bfa", label="model")
    ax.set_xlabel("mean predicted P(up)")
    ax.set_ylabel("observed frequency")
    ax.set_title("Calibration")
    ax.legend()
    fig.tight_layout()
    fig.savefig(out_dir / "calibration.png", dpi=110)
    plt.close(fig)

    # Feature importance (top 10)
    top = importance[:10][::-1]
    fig, ax = plt.subplots(figsize=(6, 4))
    ax.barh([t["feature"] for t in top], [t["coef"] for t in top], color="#38bdf8")
    ax.set_title("Feature importance (standardized LR coefficients)")
    ax.set_xlabel("coefficient")
    fig.tight_layout()
    fig.savefig(out_dir / "feature_importance.png", dpi=110)
    plt.close(fig)

    # Net-of-cost equity (trade-all, ordered by ts)
    order = np.argsort(ts)
    pos = np.sign(proba[order] - 0.5)
    gross = np.cumsum(pos * fwd[order]) * 100
    netc = np.cumsum(pos * fwd[order] - cost[order]) * 100
    fig, ax = plt.subplots(figsize=(6, 4))
    ax.axhline(0, color="#64748b", lw=0.8)
    ax.plot(gross, color="#38bdf8", label="gross")
    ax.plot(netc, color="#22c55e", label="net of cost")
    ax.set_title("Hypothetical equity (OOS, signals NOT traded)")
    ax.set_xlabel("trade #")
    ax.set_ylabel("cumulative return %")
    ax.legend()
    fig.tight_layout()
    fig.savefig(out_dir / "equity_net.png", dpi=110)
    plt.close(fig)


if __name__ == "__main__":
    raise SystemExit(main())
