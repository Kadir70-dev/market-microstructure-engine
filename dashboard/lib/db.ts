// Read-only data source for the dashboard.
//
// If DASHBOARD_DB_PATH points at a readable engine.db with data, we open it
// READ-ONLY (mirroring the engine's cold-path boundary) and load the tables.
// Otherwise we fall back to the deterministic DEMO dataset so the app always
// renders. We NEVER open the DB for writing.

import fs from "fs";
import { demoDataset } from "./sample";
import type { Dataset, Quality, Signal, Tick } from "./types";

const DB_PATH = process.env.DASHBOARD_DB_PATH || "";

// Cache for the process lifetime of a dev/prod server. Live data refreshes on
// restart; for a true live feed you'd drop the cache TTL — kept simple here.
let cache: Dataset | null = null;

export function loadDataset(): Dataset {
  if (cache) return cache;

  if (DB_PATH && fs.existsSync(DB_PATH)) {
    try {
      cache = loadFromSqlite(DB_PATH);
      if (cache.ticks.length > 0) return cache;
    } catch (err) {
      // Native module missing or DB unreadable — fall through to demo.
      console.warn(
        `[dashboard] could not read ${DB_PATH} (${(err as Error).message}); using DEMO data`,
      );
    }
  }

  cache = demoDataset();
  return cache;
}

function loadFromSqlite(path: string): Dataset {
  // Lazy require so DEMO mode works even if the native module isn't built.
  // eslint-disable-next-line @typescript-eslint/no-var-requires
  const Database = require("better-sqlite3");
  const db = new Database(path, { readonly: true, fileMustExist: true });
  try {
    const ticks = db
      .prepare("SELECT ts, symbol, price FROM ticks ORDER BY ts ASC")
      .all() as Tick[];
    const signals = db
      .prepare(
        "SELECT ts, symbol, momentum, vol_score, vol_regime FROM signals ORDER BY ts ASC",
      )
      .all() as Signal[];
    const quality = tableExists(db, "quality_scores")
      ? (db
          .prepare(
            "SELECT ts, symbol, stale, session, confidence, trade_quality, grade " +
              "FROM quality_scores ORDER BY ts ASC",
          )
          .all() as Quality[])
      : [];
    return { ticks, signals, quality, source: "live", dbPath: path };
  } finally {
    db.close();
  }
}

function tableExists(db: any, name: string): boolean {
  const row = db
    .prepare("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?")
    .get(name);
  return !!row;
}
