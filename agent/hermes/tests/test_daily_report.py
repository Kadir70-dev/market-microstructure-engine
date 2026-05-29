"""Unit tests for Hermes daily-report generation.

Zero third-party deps — uses the stdlib ``unittest`` runner and a real
temporary SQLite DB built with the engine's schema, so it also exercises the
read-only ``db.py`` access layer end to end.

Run from the repo root:

    python3 -m unittest agent.hermes.tests.test_daily_report -v
"""

from __future__ import annotations

import sqlite3
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

from agent.hermes import daily_report as dr
from agent.hermes import db


# Engine schema (mirrors storage/sqlite_logger.cpp). Kept here so the tests
# don't need a live engine to have run.
SCHEMA = """
CREATE TABLE ticks   (ts INTEGER, symbol TEXT, price REAL);
CREATE TABLE signals (ts INTEGER, symbol TEXT, momentum TEXT, vol_score REAL, vol_regime TEXT);
CREATE TABLE quality_scores (ts INTEGER, symbol TEXT, stale INTEGER, session TEXT,
                             confidence INTEGER, trade_quality INTEGER, grade TEXT);
"""


def ts_for(date_str: str, hour: int = 12, minute: int = 0, second: int = 0) -> int:
    d = datetime.strptime(date_str, "%Y-%m-%d").replace(
        hour=hour, minute=minute, second=second, tzinfo=timezone.utc
    )
    return int(d.timestamp())


class HermesReportTests(unittest.TestCase):
    DATE = "2026-03-04"  # a Wednesday

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.db_path = Path(self.tmp.name) / "engine.db"
        conn = sqlite3.connect(self.db_path)
        conn.executescript(SCHEMA)
        conn.commit()
        conn.close()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    # -- helpers -----------------------------------------------------------
    def _write(self, ticks=(), signals=(), quality=()):
        conn = sqlite3.connect(self.db_path)
        conn.executemany("INSERT INTO ticks VALUES (?,?,?)", ticks)
        conn.executemany("INSERT INTO signals VALUES (?,?,?,?,?)", signals)
        conn.executemany("INSERT INTO quality_scores VALUES (?,?,?,?,?,?,?)", quality)
        conn.commit()
        conn.close()

    def _ro_conn(self) -> sqlite3.Connection:
        return db.connect(self.db_path)

    # -- pure summary functions -------------------------------------------
    def test_summarize_signals_counts(self):
        signals = [
            {"momentum": "Bullish"},
            {"momentum": "Bullish"},
            {"momentum": "Bearish"},
            {"momentum": "Neutral"},
        ]
        s = dr.summarize_signals(signals)
        self.assertEqual(s["total"], 4)
        self.assertEqual(s["bullish"], 2)
        self.assertEqual(s["bearish"], 1)
        self.assertEqual(s["neutral"], 1)

    def test_pct_handles_zero_total(self):
        self.assertEqual(dr.pct(0, 0), "n/a")
        self.assertEqual(dr.pct(1, 4), "25.0%")

    # -- confidence accuracy mirrors backtest exclusion rules --------------
    def test_confidence_accuracy_correct_call(self):
        t0 = ts_for(self.DATE, 12, 0, 0)
        # Bullish high-confidence signal; price rises 60s later → correct.
        self._write(
            ticks=[
                (t0, "EUR/USD", 1.1000),          # baseline at signal ts
                (t0 + 60, "EUR/USD", 1.1010),     # future tick: up
            ],
            signals=[(t0, "EUR/USD", "Bullish", 0.0005, "LOW")],
            quality=[(t0, "EUR/USD", 0, "London", 80, 80, "A")],
        )
        conn = self._ro_conn()
        try:
            sigs = db.fetch_signals(conn, ts_for(self.DATE, 0), ts_for(self.DATE, 0) + 86400)
            qual = db.fetch_quality(conn, ts_for(self.DATE, 0), ts_for(self.DATE, 0) + 86400)
            bands = dr.confidence_accuracy(conn, sigs, qual)
        finally:
            conn.close()
        high = next(b for b in bands if b["band"].startswith("High"))
        self.assertEqual(high["n"], 1)
        self.assertEqual(high["accuracy_pct"], 100.0)

    def test_confidence_accuracy_excludes_stale_and_neutral(self):
        t0 = ts_for(self.DATE, 12, 0, 0)
        self._write(
            ticks=[(t0, "EUR/USD", 1.10), (t0 + 60, "EUR/USD", 1.11)],
            signals=[
                (t0, "EUR/USD", "Bullish", 0.0, "LOW"),   # but stale → excluded
                (t0, "XAU/USD", "Neutral", 0.0, "LOW"),   # neutral → excluded
            ],
            quality=[
                (t0, "EUR/USD", 1, "London", 80, 80, "A"),  # stale=1
                (t0, "XAU/USD", 0, "London", 80, 80, "A"),
            ],
        )
        conn = self._ro_conn()
        try:
            sigs = db.fetch_signals(conn, ts_for(self.DATE, 0), ts_for(self.DATE, 0) + 86400)
            qual = db.fetch_quality(conn, ts_for(self.DATE, 0), ts_for(self.DATE, 0) + 86400)
            bands = dr.confidence_accuracy(conn, sigs, qual)
        finally:
            conn.close()
        # Nothing should be counted in any band.
        self.assertTrue(all(b["n"] == 0 for b in bands))

    # -- problem detection -------------------------------------------------
    def test_detect_problems_frozen_feed(self):
        regime = {"sessions": {"London": 10}, "regimes": {"LOW": 10},
                  "stale": 8, "stale_pct": 80.0}
        sig = {"total": 10, "bullish": 5, "bearish": 5, "neutral": 0}
        problems = dr.detect_problems(sig, [], regime, [])
        self.assertTrue(any("Frozen data" in p for p in problems))

    def test_detect_problems_no_signals(self):
        regime = {"sessions": {}, "regimes": {}, "stale": 0, "stale_pct": 0.0}
        sig = {"total": 0, "bullish": 0, "bearish": 0, "neutral": 0}
        problems = dr.detect_problems(sig, [], regime, [])
        self.assertEqual(len(problems), 1)
        self.assertIn("engine may not have been running", problems[0])

    # -- db read-only boundary --------------------------------------------
    def test_db_connection_is_readonly(self):
        conn = self._ro_conn()
        try:
            with self.assertRaises(sqlite3.OperationalError):
                conn.execute("INSERT INTO ticks VALUES (1, 'X', 1.0)")
        finally:
            conn.close()

    # -- end-to-end: main() writes a report file --------------------------
    def test_main_writes_report(self):
        t0 = ts_for(self.DATE, 12, 0, 0)
        self._write(
            ticks=[(t0, "EUR/USD", 1.10), (t0 + 60, "EUR/USD", 1.11)],
            signals=[(t0, "EUR/USD", "Bullish", 0.0005, "LOW")],
            quality=[(t0, "EUR/USD", 0, "London", 80, 80, "A")],
        )
        out_dir = Path(self.tmp.name) / "reports"
        argv = ["daily_report", "--date", self.DATE,
                "--db", str(self.db_path), "--out", str(out_dir)]
        old_argv = sys.argv
        sys.argv = argv
        try:
            rc = dr.main()
        finally:
            sys.argv = old_argv
        self.assertEqual(rc, 0)
        report = out_dir / f"{self.DATE}.md"
        self.assertTrue(report.exists())
        text = report.read_text()
        self.assertIn("# Daily Trading Intelligence Report", text)
        self.assertIn("analysis-only", text)
        self.assertIn("EUR/USD", text)


if __name__ == "__main__":
    unittest.main()
