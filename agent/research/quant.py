"""Quant formulas mirroring the C++ engine (validation.cpp / volatility.cpp /
evaluation/main.cpp) so synthetic data is derived identically to real data and
the feature pipeline treats both the same way."""

from __future__ import annotations

import math
from datetime import datetime, timezone

WINDOW = 10

# Per-symbol round-trip cost as a fraction of price (evaluation/main.cpp table).
COST = {"EUR/USD": 0.00015, "USO": 0.00020, "XAU/USD": 0.00015}
DEFAULT_COST = 0.00030


def round_trip_cost(symbol: str) -> float:
    return COST.get(symbol, DEFAULT_COST)


def detect_session(ts: int) -> str:
    """UTC sessions, no DST — mirrors validation.cpp::detectSession."""
    d = datetime.fromtimestamp(ts, tz=timezone.utc)
    wday = d.isoweekday()  # 1=Mon .. 7=Sun
    hour = d.hour
    # Forex weekend: Fri 22:00 -> Sun 22:00 UTC.
    if wday == 6:
        return "Closed"
    if wday == 5 and hour >= 22:
        return "Closed"
    if wday == 7 and hour < 22:
        return "Closed"
    if hour >= 22 or hour < 8:
        return "Asia"
    if hour < 13:
        return "London"
    if hour < 17:
        return "London+NY"
    return "NewYork"


def session_multiplier(session: str) -> float:
    return {
        "Closed": 0.0,
        "Asia": 0.6,
        "London": 1.0,
        "London+NY": 1.0,
        "NewYork": 0.95,
    }.get(session, 0.0)


def classify_volatility(stddev: float) -> str:
    if stddev > 0.001:
        return "HIGH"
    if stddev > 0.0003:
        return "MEDIUM"
    return "LOW"


def stddev_log_returns(prices: list[float]) -> float:
    if len(prices) < 2:
        return 0.0
    rets = [math.log(prices[i] / prices[i - 1]) for i in range(1, len(prices))]
    mean = sum(rets) / len(rets)
    var = sum((r - mean) ** 2 for r in rets) / len(rets)
    return math.sqrt(var)


def compute_confidence(
    stale: bool, sample_count: int, window_target: int, momentum: str, regime: str
) -> int:
    if stale:
        return 0
    base = 100.0
    if sample_count < window_target and window_target > 0:
        base *= sample_count / window_target
    if regime == "HIGH":
        base *= 0.5
    elif regime == "MEDIUM":
        base *= 0.8
    if momentum == "Neutral":
        base *= 0.3
    return max(0, min(100, round(base)))


def grade_for(score: int) -> str:
    if score >= 75:
        return "A"
    if score >= 50:
        return "B"
    if score >= 25:
        return "C"
    return "D"
