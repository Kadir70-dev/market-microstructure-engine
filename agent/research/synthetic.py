"""Deterministic LABELED synthetic session — identical generating process to the
dashboard demo (lib/sample.ts). Mild momentum persistence (phi=0.15) gives a
weak, *known* directional signal so the pipeline has something honest to detect;
per-symbol costs make the net-of-cost test meaningful.

This is NOT used to claim real performance. It exists so the production pipeline
produces reproducible, honestly-labeled metrics until enough LIVE data is
collected (`--source db` switches over automatically once the real data is
trainable). The generating process is fully disclosed here.
"""

from __future__ import annotations

import math
from datetime import datetime, timezone

import numpy as np
import pandas as pd

from . import quant

STEP_S = 30
N_CYCLES = 14 * 60 * 2  # 14h × 120/h = 1680
DAY_START = int(datetime(2026, 5, 27, 7, 0, tzinfo=timezone.utc).timestamp())

SYMBOLS = [
    {"symbol": "EUR/USD", "start": 1.0850, "sigma": 0.00009, "freeze": None},
    {"symbol": "XAU/USD", "start": 2350.0, "sigma": 0.00022, "freeze": None},
    {"symbol": "USO", "start": 78.50, "sigma": 0.00035, "freeze": (880, 892)},
]
PHI = 0.15  # momentum persistence (the planted, weak, honestly-disclosed signal)


def _build_symbol(cfg: dict) -> tuple[list, list, list]:
    seed = sum(ord(c) for c in cfg["symbol"]) * 7919
    rng = np.random.default_rng(seed)

    ticks, signals, quality = [], [], []
    window: list[float] = []
    price = cfg["start"]
    prev_ret = 0.0
    vol_mult = 1.0
    decimals = 5 if cfg["symbol"] == "EUR/USD" else 3

    for i in range(N_CYCLES):
        ts = DAY_START + i * STEP_S
        frozen = cfg["freeze"] is not None and cfg["freeze"][0] <= i < cfg["freeze"][1]

        if not frozen:
            vol_mult = max(0.4, vol_mult * 0.97 + 0.03 + (rng.random() - 0.5) * 0.4)
            ret = PHI * prev_ret + cfg["sigma"] * vol_mult * rng.standard_normal()
            prev_ret = ret
            price = price * math.exp(ret)

        rounded = round(price, decimals)
        ticks.append({"ts": ts, "symbol": cfg["symbol"], "price": rounded})

        window.append(rounded)
        if len(window) > quant.WINDOW:
            window.pop(0)

        momentum = "Neutral"
        if len(window) >= 2:
            diff = window[-1] - window[-2]
            if diff > 1e-7:
                momentum = "Bullish"
            elif diff < -1e-7:
                momentum = "Bearish"

        vol = quant.stddev_log_returns(window)
        regime = quant.classify_volatility(vol)

        stale = False
        if len(window) >= 3:
            a, b, c = window[-1], window[-2], window[-3]
            stale = abs(a - b) <= 1e-9 and abs(a - c) <= 1e-9

        conf = quant.compute_confidence(stale, len(window), quant.WINDOW, momentum, regime)
        session = quant.detect_session(ts)
        tq = max(0, min(100, round(conf * quant.session_multiplier(session))))

        signals.append(
            {"ts": ts, "symbol": cfg["symbol"], "momentum": momentum,
             "vol_score": vol, "vol_regime": regime}
        )
        quality.append(
            {"ts": ts, "symbol": cfg["symbol"], "stale": int(stale), "session": session,
             "confidence": conf, "trade_quality": tq, "grade": quant.grade_for(tq)}
        )

    return ticks, signals, quality


def load_synthetic() -> tuple[pd.DataFrame, pd.DataFrame, pd.DataFrame]:
    ticks, signals, quality = [], [], []
    for cfg in SYMBOLS:
        t, s, q = _build_symbol(cfg)
        ticks += t
        signals += s
        quality += q
    return (
        pd.DataFrame(ticks).sort_values("ts").reset_index(drop=True),
        pd.DataFrame(signals).sort_values("ts").reset_index(drop=True),
        pd.DataFrame(quality).sort_values("ts").reset_index(drop=True),
    )
