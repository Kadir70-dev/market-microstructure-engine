# Research — honest predictive baseline

A **next-direction probability** model over the engine's own features, built to
quant standards and reported honestly. No deep learning, no fabricated results.

## TL;DR result (current run)

| | |
|---|---|
| Model | Logistic Regression + StandardScaler, **walk-forward** (expanding, time-ordered) |
| Out-of-sample AUC | **0.545** vs permutation baseline 0.498 ± 0.009, **p = 0.03** → above chance |
| Calibration | Brier-scored, reliability curve in `results/calibration.png` |
| Net of cost | gross **+8.5%**, **net −57%** over 3,932 trades → **does NOT survive costs** |
| Verdict | A weak, statistically-real directional signal that **transaction costs erase**. No deployable edge — which is the honest, correct finding. |

The model outputs **probabilities**, is **calibrated**, and is evaluated
**net of the per-symbol round-trip cost** — the only number that matters.

## Honesty note on data

The live `engine.db` is currently **not trainable** (too few rows, zero label
variance — every signal was Neutral during the short runs). The pipeline says so
explicitly:

```
$ python -m agent.research.train --source db
[train] live DB NOT trainable: no labeled rows ...
```

So the metrics above are computed on a **deterministic synthetic session**
(`synthetic.py`) whose generating process is fully disclosed (mild momentum
persistence φ=0.15 + costs). This is *not* a performance claim — it demonstrates
the methodology and proves the pipeline detects a known weak signal and correctly
rejects it after costs. **`--source db` switches to live data automatically once
enough is collected** — no code change.

## Run

```bash
python3 -m venv agent/research/.venv
agent/research/.venv/bin/pip install -r agent/research/requirements.txt

# from repo root:
agent/research/.venv/bin/python -m agent.research.train               # auto (live if trainable, else synthetic)
agent/research/.venv/bin/python -m agent.research.train --source synthetic
agent/research/.venv/bin/python -m agent.research.train --source db --db data/engine.db
```

Outputs `results/model_results.json` (consumed by the dashboard) plus
`calibration.png`, `feature_importance.png`, `equity_net.png`.

## Files

| File | Role |
|---|---|
| `quant.py` | Python port of the engine formulas (session, vol, confidence, cost) |
| `synthetic.py` | Labeled synthetic session (disclosed generating process) |
| `features.py` | Feature matrix + **look-ahead-safe** forward-60s label; trainability gate |
| `train.py` | Walk-forward LR, AUC/PR/calibration/importance, permutation test, net-of-cost |
| `research.ipynb` | EDA + findings notebook |

## Why these choices (not overengineering)

- **Logistic Regression, not LightGBM/XGBoost** — interpretable (coefficients =
  importance), naturally calibrated, won't overfit a small/weak-signal dataset.
  A GBM is unjustified without evidence of nonlinearity; adding it would be noise.
- **Walk-forward, not random CV** — random splits leak the future. Time-ordered
  expanding windows are mandatory for honest time-series evaluation.
- **Probability + calibration**, not binary — a 0.55 probability and a 0.95
  probability are different bets; calibration tells you if they're trustworthy.
- **Permutation test** — proves the AUC is (or isn't) above chance, not eyeballed.
- **Net-of-cost first** — a positive gross edge that dies after spread is not an
  edge. Reporting net is the difference between a quant and a backtest tourist.

## Features used (all already collected or cheaply derived)

`mom_ret`, `mom_sign`, `vol_score`, `vol_regime`, `confidence`, `trade_quality`,
`grade`, `roll_mean_5`, `roll_std_5`, `roll_ret_sum_3`, `cost`, and session
one-hots. No new data collection required.
