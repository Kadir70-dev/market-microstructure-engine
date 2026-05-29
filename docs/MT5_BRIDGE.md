# MT5 feed — file-export (Linux + Wine)

**The engine's only data source.** Read-only, no trading, no API key, no Python.

```
[Ubuntu + Wine]  MetaTrader 5 terminal + mt5_file_export.mq5 (EA)
                        │  writes  MQL5/Files/mme_quotes.csv   (a normal Linux path)
                        ▼
              native-Linux C++ engine  (FileProvider reads the CSV)
                        │
                        ▼
                    SQLite (data/engine.db)
```

Why this design (not the Python `MetaTrader5` package): that package is
Windows-only and the Wine-Python IPC workaround is fragile. MT5 *itself* runs
fine under Wine, so we let a tiny **MQL5 Expert Advisor** — running inside the
terminal — write quotes to a file, and the native-Linux engine just reads it.
No cross-boundary sockets, no Python under Wine, staleness-tolerant.

## CSV schema (`mme_quotes.csv`)

One line per symbol, **overwritten** every interval (so file mtime = freshness):

```
symbol,epoch_seconds,bid,ask,mid
EURUSD,1780061874,1.08490,1.08500,1.08495
XAUUSD,1780061874,2350.40,2350.60,2350.50
XTIUSD,1780061874,78.50,78.54,78.52
```

- `symbol` — broker symbol (must match `SymbolDef.mt5` in `src/main.cpp`).
- `epoch_seconds` — broker tick time (informational; the engine uses file mtime
  for freshness).
- `mid` — what the engine ingests; if blank it falls back to `(bid+ask)/2`.
- MT5 writes CRLF line endings; the reader strips them.

## Engine integration

| Variable | Default | Meaning |
|---|---|---|
| `MME_QUOTES_CSV` | `../data/mme_quotes.csv` | Path to the EA's CSV (relative to `build/`). In production set the **absolute** Wine path. |
| `MME_FILE_STALE_S` | `120` | If the CSV's mtime is older than this, every quote is treated as a failure (engine skips, logs a warning). |
| `MME_DB_PATH` | `../data/engine.db` | SQLite output path. |

The engine logs `Provider: MT5 file-export (...)` at startup and warns (does not
exit) if the CSV is missing/stale — it retries every 30s cycle.

## Step-by-step: Ubuntu + Wine + MT5

1. **Wine + MT5:** install your broker's MT5 under Wine; log into a **demo** account.
2. **Install the EA:** copy `agent/mt5_bridge/mt5_file_export.mq5` into the
   terminal's `MQL5/Experts/` folder (inside the Wine prefix), open **MetaEditor**
   (F4 in the terminal), open the file, press **Compile** (F7).
3. **Attach it:** in the terminal, drag `mt5_file_export` onto any chart; in the
   dialog tick **Allow Algo Trading** (needed for file writes, *not* for orders —
   the EA has no order code), click OK. A smiley face = running.
4. **Find the CSV:** it appears at
   `<WinePrefix>/drive_c/.../MQL5/Files/mme_quotes.csv`. Verify with
   `ops/check_quotes.sh /abs/path/to/mme_quotes.csv`.
5. **Point the engine at it:** `export MME_QUOTES_CSV=/abs/path/.../mme_quotes.csv`
   then run the engine (or set it in `ops/.env` / the systemd unit).

### Adjust symbols to your broker

Brokers name symbols differently (e.g. crude oil is `XTIUSD` / `WTI` / `USOIL`).
Set `InpSymbols` in the EA **and** the `mt5` fields in `src/main.cpp`'s
`SymbolDef` to match exactly, then rebuild.

## Health & staleness

```
ops/check_quotes.sh                 # default path; exit 0 fresh / 1 missing / 2 stale
python3 ops/health_check.py         # includes a quotes_csv freshness check
```

Weekend/holiday: the market is closed, the CSV stops updating → `quotes_csv`
and `price_not_frozen` go WARN (logged, not paged). The engine resumes
automatically when quotes flow again.

## Safety

The EA is **read-only** — it has no order calls and logs a warning if attached
to a non-demo account. The engine only *reads* a file; there is no execution
path anywhere in the system.
