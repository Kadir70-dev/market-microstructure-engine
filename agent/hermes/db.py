"""Read-only SQLite access for Hermes.

The connection is opened with ``mode=ro`` URI so write operations are rejected
by SQLite itself — the analysis-only boundary is enforced at the driver level,
not just by convention.
"""

from __future__ import annotations

import sqlite3
from pathlib import Path


def repo_root() -> Path:
    # agent/hermes/db.py -> repo root is two parents up.
    return Path(__file__).resolve().parents[2]


def default_db_path() -> Path:
    return repo_root() / "data" / "engine.db"


def connect(db_path: Path | str | None = None) -> sqlite3.Connection:
    path = Path(db_path) if db_path else default_db_path()
    if not path.exists():
        raise FileNotFoundError(f"engine.db not found at {path}")
    uri = f"file:{path}?mode=ro"
    conn = sqlite3.connect(uri, uri=True)
    conn.row_factory = sqlite3.Row
    return conn


def has_table(conn: sqlite3.Connection, name: str) -> bool:
    row = conn.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (name,)
    ).fetchone()
    return row is not None


def fetch_signals(conn: sqlite3.Connection, start_ts: int, end_ts: int) -> list[sqlite3.Row]:
    return conn.execute(
        "SELECT ts, symbol, momentum, vol_score, vol_regime "
        "FROM signals WHERE ts >= ? AND ts < ? ORDER BY ts ASC",
        (start_ts, end_ts),
    ).fetchall()


def fetch_quality(conn: sqlite3.Connection, start_ts: int, end_ts: int) -> list[sqlite3.Row]:
    if not has_table(conn, "quality_scores"):
        return []
    return conn.execute(
        "SELECT ts, symbol, stale, session, confidence, trade_quality, grade "
        "FROM quality_scores WHERE ts >= ? AND ts < ? ORDER BY ts ASC",
        (start_ts, end_ts),
    ).fetchall()


def fetch_ticks(conn: sqlite3.Connection, start_ts: int, end_ts: int) -> list[sqlite3.Row]:
    return conn.execute(
        "SELECT ts, symbol, price FROM ticks "
        "WHERE ts >= ? AND ts < ? ORDER BY symbol ASC, ts ASC",
        (start_ts, end_ts),
    ).fetchall()


def price_at_or_after(
    conn: sqlite3.Connection, symbol: str, target_ts: int, tolerance_s: int
) -> float | None:
    # Mirrors engine_backtest's future-price lookup: first tick at or after the
    # target within tolerance, else None (excluded rather than fabricated).
    row = conn.execute(
        "SELECT price FROM ticks WHERE symbol=? AND ts >= ? AND ts <= ? "
        "ORDER BY ts ASC LIMIT 1",
        (symbol, target_ts, target_ts + tolerance_s),
    ).fetchone()
    return float(row["price"]) if row else None


def price_at_or_before(
    conn: sqlite3.Connection, symbol: str, target_ts: int, tolerance_s: int
) -> float | None:
    row = conn.execute(
        "SELECT price FROM ticks WHERE symbol=? AND ts <= ? AND ts >= ? "
        "ORDER BY ts DESC LIMIT 1",
        (symbol, target_ts, target_ts - tolerance_s),
    ).fetchone()
    return float(row["price"]) if row else None
