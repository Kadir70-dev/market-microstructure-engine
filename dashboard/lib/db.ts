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

// The engine appends a cycle every 30s, so a process-lifetime cache would pin
// the UI to whatever existed at boot. Cache is therefore bounded twice: by a
// short TTL *and* by the DB file's mtime — a write invalidates immediately,
// while a quiet DB still absorbs bursts of requests with one read.
const CACHE_TTL_MS = Number(process.env.DASHBOARD_CACHE_TTL_MS || 5000);

interface CacheEntry {
  ds: Dataset;
  loadedAtMs: number;
  mtimeMs: number | null;
}

let cache: CacheEntry | null = null;

function dbMtimeMs(): number | null {
  if (!DB_PATH) return null;
  try {
    return fs.statSync(DB_PATH).mtimeMs;
  } catch {
    return null;
  }
}

export function loadDataset(): Dataset {
  const nowMs = Date.now();
  const mtimeMs = dbMtimeMs();

  if (
    cache &&
    cache.mtimeMs === mtimeMs &&
    nowMs - cache.loadedAtMs < CACHE_TTL_MS
  ) {
    return cache.ds;
  }

  const ds = readDataset(mtimeMs);
  cache = { ds, loadedAtMs: nowMs, mtimeMs };
  return ds;
}

// Why the dataset ended up demo when live was requested — surfaced to the UI so
// "live" failing over to "demo" is visible instead of silent.
function readDataset(mtimeMs: number | null): Dataset {
  if (!DB_PATH) {
    return { ...demoDataset(), sourceReason: "DASHBOARD_DB_PATH is not set" };
  }
  if (mtimeMs === null) {
    return {
      ...demoDataset(),
      sourceReason: `DASHBOARD_DB_PATH=${DB_PATH} does not exist or is unreadable`,
    };
  }
  try {
    const live = loadFromSqlite(DB_PATH);
    if (live.ticks.length > 0) return live;
    return {
      ...demoDataset(),
      sourceReason: `${DB_PATH} has no ticks yet (engine has not collected data)`,
    };
  } catch (err) {
    // Native module missing or DB unreadable — fall through to demo.
    const message = (err as Error).message;
    console.warn(`[dashboard] could not read ${DB_PATH} (${message}); using DEMO data`);
    return { ...demoDataset(), sourceReason: `could not read ${DB_PATH}: ${message}` };
  }
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
    return { ticks, signals, quality, source: "live", dbPath: path, sourceReason: null };
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
