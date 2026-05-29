"""Daily intelligence report generator.

Usage:
    python -m agent.hermes.daily_report                  # today (UTC)
    python -m agent.hermes.daily_report --date 2026-05-24
    python -m agent.hermes.daily_report --date 2026-05-24 --db /alt/path/engine.db

Writes ``agent/hermes/reports/YYYY-MM-DD.md``. Read-only; never mutates the DB.
"""

from __future__ import annotations

import argparse
import sqlite3
from collections import Counter, defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path
from statistics import mean

from . import db

FORWARD_HORIZON_S = 60
FORWARD_TOLERANCE_S = 120  # one cycle of slack, same convention as engine_backtest
BASELINE_TOLERANCE_S = 120


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Generate Hermes daily report.")
    p.add_argument("--date", help="UTC date YYYY-MM-DD (default: today UTC)")
    p.add_argument("--db", help="Path to engine.db (default: <repo>/data/engine.db)")
    p.add_argument("--out", help="Output directory (default: agent/hermes/reports)")
    return p.parse_args()


def utc_day_window(date_str: str | None) -> tuple[datetime, int, int]:
    if date_str:
        day = datetime.strptime(date_str, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    else:
        day = datetime.now(timezone.utc).replace(hour=0, minute=0, second=0, microsecond=0)
    start = day
    end = day + timedelta(days=1)
    return day, int(start.timestamp()), int(end.timestamp())


def pct(n: int, total: int) -> str:
    return f"{(100.0 * n / total):.1f}%" if total else "n/a"


def summarize_signals(signals: list[sqlite3.Row]) -> dict:
    total = len(signals)
    counts = Counter(s["momentum"] for s in signals)
    return {
        "total": total,
        "bullish": counts.get("Bullish", 0),
        "bearish": counts.get("Bearish", 0),
        "neutral": counts.get("Neutral", 0),
    }


def per_symbol_breakdown(
    signals: list[sqlite3.Row], quality: list[sqlite3.Row]
) -> list[dict]:
    by_symbol: dict[str, list[sqlite3.Row]] = defaultdict(list)
    for s in signals:
        by_symbol[s["symbol"]].append(s)

    quality_by_ts_sym = {(q["ts"], q["symbol"]): q for q in quality}

    rows = []
    for sym in sorted(by_symbol):
        ss = by_symbol[sym]
        mom = Counter(s["momentum"] for s in ss)
        confs = [
            quality_by_ts_sym[(s["ts"], sym)]["confidence"]
            for s in ss
            if (s["ts"], sym) in quality_by_ts_sym
        ]
        quals = [
            quality_by_ts_sym[(s["ts"], sym)]["trade_quality"]
            for s in ss
            if (s["ts"], sym) in quality_by_ts_sym
        ]
        grades = Counter(
            quality_by_ts_sym[(s["ts"], sym)]["grade"]
            for s in ss
            if (s["ts"], sym) in quality_by_ts_sym
        )
        top_grade = grades.most_common(1)[0][0] if grades else "-"
        rows.append(
            {
                "symbol": sym,
                "signals": len(ss),
                "bull": mom.get("Bullish", 0),
                "bear": mom.get("Bearish", 0),
                "neutral": mom.get("Neutral", 0),
                "mean_conf": mean(confs) if confs else None,
                "mean_quality": mean(quals) if quals else None,
                "top_grade": top_grade,
            }
        )
    return rows


def confidence_accuracy(
    conn: sqlite3.Connection,
    signals: list[sqlite3.Row],
    quality: list[sqlite3.Row],
) -> list[dict]:
    """Forward 60s directional accuracy by confidence band.

    Excludes: stale signals, neutral momentum, observations with missing
    baseline or future price (same rules as the C++ backtest harness).
    """
    quality_by_ts_sym = {(q["ts"], q["symbol"]): q for q in quality}

    bands = {
        "High (60-100)": (60, 100),
        "Medium (30-59)": (30, 59),
        "Low (0-29)": (0, 29),
    }
    out = []
    for label, (lo, hi) in bands.items():
        n_total = 0
        n_correct = 0
        for s in signals:
            q = quality_by_ts_sym.get((s["ts"], s["symbol"]))
            if q is None or q["stale"]:
                continue
            if s["momentum"] not in ("Bullish", "Bearish"):
                continue
            c = q["confidence"]
            if not (lo <= c <= hi):
                continue
            base = db.price_at_or_before(conn, s["symbol"], s["ts"], BASELINE_TOLERANCE_S)
            fut = db.price_at_or_after(
                conn, s["symbol"], s["ts"] + FORWARD_HORIZON_S, FORWARD_TOLERANCE_S
            )
            if base is None or fut is None:
                continue
            n_total += 1
            went_up = fut > base
            if (s["momentum"] == "Bullish" and went_up) or (
                s["momentum"] == "Bearish" and not went_up
            ):
                n_correct += 1
        out.append(
            {
                "band": label,
                "n": n_total,
                "accuracy_pct": (100.0 * n_correct / n_total) if n_total else None,
            }
        )
    return out


def regime_insights(quality: list[sqlite3.Row], signals: list[sqlite3.Row]) -> dict:
    sessions = Counter(q["session"] for q in quality)
    regimes = Counter(s["vol_regime"] for s in signals)
    stale = sum(1 for q in quality if q["stale"])
    return {
        "sessions": dict(sessions),
        "regimes": dict(regimes),
        "stale": stale,
        "stale_pct": (100.0 * stale / len(quality)) if quality else 0.0,
    }


def detect_problems(
    sig_summary: dict,
    symbols: list[dict],
    regime: dict,
    confidence: list[dict],
) -> list[str]:
    problems = []
    total = sig_summary["total"]
    if total == 0:
        problems.append("No signals recorded for this day — engine may not have been running.")
        return problems

    if regime["stale_pct"] >= 50:
        problems.append(
            f"Frozen data: {regime['stale_pct']:.1f}% of quality records flagged stale "
            f"({regime['stale']}/{total}). Check feed health and symbol resolution."
        )
    neutral_pct = 100.0 * sig_summary["neutral"] / total
    if neutral_pct >= 80:
        problems.append(
            f"Over-triggering Neutral: {neutral_pct:.1f}% of signals are Neutral. "
            "Momentum threshold (±1e-7) may be too tight for the observed price scale, "
            "or the engine ran predominantly during low-activity periods."
        )
    closed_n = regime["sessions"].get("Closed", 0)
    if closed_n and closed_n == sum(regime["sessions"].values()):
        problems.append(
            "All signals occurred during Closed session — engine schedule is misaligned "
            "with market hours. Expected: London/NewYork overlap drives most useful data."
        )
    for row in symbols:
        if row["mean_conf"] is not None and row["mean_conf"] < 10:
            problems.append(
                f"Weak signal generation for {row['symbol']}: mean confidence "
                f"{row['mean_conf']:.1f}/100 across {row['signals']} signals."
            )
    for band in confidence:
        if band["band"].startswith("High") and band["n"] >= 20:
            if band["accuracy_pct"] is not None and band["accuracy_pct"] < 50:
                problems.append(
                    f"High-confidence band shows {band['accuracy_pct']:.1f}% directional "
                    f"accuracy over N={band['n']} — below coin flip. Confidence score is "
                    "not predictive at this sample size; investigate scoring formula."
                )
    return problems


def make_recommendations(
    sig_summary: dict,
    symbols: list[dict],
    regime: dict,
    problems: list[str],
) -> list[str]:
    recs = []
    if sig_summary["total"] == 0:
        recs.append("Run the engine continuously for at least one full London/NewYork session "
                    "before re-running this report.")
        return recs

    low_n = regime["regimes"].get("LOW", 0)
    total_regimes = sum(regime["regimes"].values())
    if total_regimes and low_n / total_regimes > 0.9:
        recs.append(
            "Volatility regime is ~100% LOW. Replace the hardcoded thresholds in "
            "`indicators/volatility.cpp` with empirical percentiles from `signals` history "
            "once you have a few thousand samples (CLAUDE.md flagged this as TODO)."
        )

    neutral_pct = 100.0 * sig_summary["neutral"] / sig_summary["total"]
    if neutral_pct >= 80:
        recs.append(
            "Loosen the momentum dead-zone in `indicators/momentum.cpp`, or — better — "
            "make the threshold scale with the symbol's recent volatility instead of a "
            "fixed ±1e-7 across all instruments."
        )

    closed_n = regime["sessions"].get("Closed", 0)
    sess_total = sum(regime["sessions"].values())
    if sess_total and closed_n / sess_total > 0.5:
        recs.append(
            "Deploy the engine on a schedule aligned with London/NewYork overlap "
            "(12:00–17:00 UTC) where signal density and reliability are highest."
        )

    if regime["stale_pct"] >= 30:
        recs.append(
            "Investigate stale-signal source: check TwelveData rate-limit headers in "
            "logs and confirm `market_fetcher.cpp` is parsing fresh responses, not cache."
        )

    if not recs:
        recs.append("No threshold changes warranted from today's data. Continue collecting.")
    return recs


def render_report(
    day: datetime,
    db_path: Path,
    sig_summary: dict,
    symbols: list[dict],
    confidence: list[dict],
    regime: dict,
    problems: list[str],
    recs: list[str],
) -> str:
    total = sig_summary["total"]
    lines: list[str] = []
    lines.append("# Daily Trading Intelligence Report")
    lines.append("")
    lines.append(f"**Date (UTC)**: {day.strftime('%Y-%m-%d')}  ")
    lines.append(f"**Window**: {day.isoformat()} → {(day + timedelta(days=1)).isoformat()}  ")
    lines.append(f"**DB**: `{db_path}`  ")
    lines.append("**Mode**: analysis-only (read-only SQLite, no broker connection)")
    lines.append("")

    lines.append("## Signal Summary")
    lines.append(f"- Total signals: **{total}**")
    lines.append(f"- Bullish: {sig_summary['bullish']} ({pct(sig_summary['bullish'], total)})")
    lines.append(f"- Bearish: {sig_summary['bearish']} ({pct(sig_summary['bearish'], total)})")
    lines.append(f"- Neutral: {sig_summary['neutral']} ({pct(sig_summary['neutral'], total)})")
    lines.append("")

    lines.append("## Symbol Performance")
    if not symbols:
        lines.append("_No signals._")
    else:
        lines.append("| Symbol | Signals | Bull | Bear | Neutral | Mean Conf | Mean Quality | Top Grade |")
        lines.append("|---|---:|---:|---:|---:|---:|---:|:---:|")
        for r in symbols:
            mc = f"{r['mean_conf']:.1f}" if r["mean_conf"] is not None else "-"
            mq = f"{r['mean_quality']:.1f}" if r["mean_quality"] is not None else "-"
            lines.append(
                f"| {r['symbol']} | {r['signals']} | {r['bull']} | {r['bear']} | "
                f"{r['neutral']} | {mc} | {mq} | {r['top_grade']} |"
            )
    lines.append("")

    lines.append("## Confidence Analysis")
    lines.append(f"Forward {FORWARD_HORIZON_S}s directional accuracy by confidence band  ")
    lines.append("_(excludes stale, neutral, and observations with missing baseline/future tick)_")
    lines.append("")
    lines.append("| Band | N | Accuracy |")
    lines.append("|---|---:|---:|")
    for b in confidence:
        acc = f"{b['accuracy_pct']:.1f}%" if b["accuracy_pct"] is not None else "n/a"
        lines.append(f"| {b['band']} | {b['n']} | {acc} |")
    lines.append("")

    lines.append("## Regime Insights")
    lines.append("**Session distribution** (from `quality_scores.session`):")
    if regime["sessions"]:
        for sess, n in sorted(regime["sessions"].items(), key=lambda kv: -kv[1]):
            lines.append(f"- {sess}: {n}")
    else:
        lines.append("- _no quality records_")
    lines.append("")
    lines.append("**Volatility regime distribution** (from `signals.vol_regime`):")
    if regime["regimes"]:
        for r, n in sorted(regime["regimes"].items(), key=lambda kv: -kv[1]):
            lines.append(f"- {r}: {n}")
    else:
        lines.append("- _no signal records_")
    lines.append("")
    lines.append(f"**Stale signals**: {regime['stale']} ({regime['stale_pct']:.1f}%)")
    lines.append("")

    lines.append("## Problems Found")
    if problems:
        for p in problems:
            lines.append(f"- {p}")
    else:
        lines.append("- None detected by Phase 1 rules.")
    lines.append("")

    lines.append("## Recommendations")
    for r in recs:
        lines.append(f"- {r}")
    lines.append("")

    lines.append("---")
    lines.append(
        "_Generated by Hermes (analysis-only). Hermes does not place trades, "
        "does not connect to brokers, and does not bypass risk management._"
    )
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    day, start_ts, end_ts = utc_day_window(args.date)

    db_path = Path(args.db) if args.db else db.default_db_path()
    out_dir = Path(args.out) if args.out else (db.repo_root() / "agent" / "hermes" / "reports")
    out_dir.mkdir(parents=True, exist_ok=True)

    conn = db.connect(db_path)
    try:
        signals = db.fetch_signals(conn, start_ts, end_ts)
        quality = db.fetch_quality(conn, start_ts, end_ts)

        sig_summary = summarize_signals(signals)
        symbols = per_symbol_breakdown(signals, quality)
        confidence = confidence_accuracy(conn, signals, quality)
        regime = regime_insights(quality, signals)
        problems = detect_problems(sig_summary, symbols, regime, confidence)
        recs = make_recommendations(sig_summary, symbols, regime, problems)

        report = render_report(
            day, db_path, sig_summary, symbols, confidence, regime, problems, recs
        )
    finally:
        conn.close()

    out_file = out_dir / f"{day.strftime('%Y-%m-%d')}.md"
    out_file.write_text(report, encoding="utf-8")
    print(f"Wrote {out_file} ({sig_summary['total']} signals analyzed)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
