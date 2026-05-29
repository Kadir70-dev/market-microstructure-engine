# MT5 Bridge — setup & operations

> ## ⚠️ Linux + Wine users: use the **file-export EA**, not the Python bridge
> The `mt5_bridge.py` sidecar below depends on the **`MetaTrader5` Python
> package, which is Windows-only**. On Ubuntu+Wine it does not run natively, and
> the Wine-Python IPC workaround is fragile (breaks on MT5 updates, hard to keep
> up 24/5). **The supported long-term MT5 path on Linux is the MQL5
> file-export Expert Advisor** ([`mt5_file_export.mq5`](../agent/mt5_bridge/mt5_file_export.mq5)):
> the EA runs *inside* the MT5 terminal (which runs fine under Wine) and writes
> quotes to a CSV the native-Linux engine reads. No Python-under-Wine, no
> cross-boundary sockets, staleness-tolerant.
>
> **Current status:** while MT5-under-Wine is being stood up, the engine
> collects via **TwelveData** (REST, native Linux, `MME_PROVIDER` default). MT5
> file-export is the long-term feed. The legacy Python/TCP bridge below is kept
> only for a real Windows host/VM.

## Linux + Wine: file-export EA (recommended)

```
[Linux box]  MT5 terminal + mt5_file_export.mq5   (under Wine)
                     │  writes MQL5/Files/mme_quotes.csv  (a normal Linux path)
                     ▼
             native-Linux C++ engine reads the CSV  →  SQLite
```

1. Install MT5 under Wine, log into your broker's **demo** account.
2. Copy `agent/mt5_bridge/mt5_file_export.mq5` into the terminal's
   `MQL5/Experts/` folder (inside the Wine prefix), compile in MetaEditor,
   attach it to any chart, and allow it to run. It writes `mme_quotes.csv` to
   `MQL5/Files/` every few seconds.
3. The Wine path is a normal Linux directory, e.g.
   `~/.mt5/drive_c/Program Files/<Terminal>/MQL5/Files/mme_quotes.csv`.
4. **Next engine step (not yet built):** a `FileProvider` that reads that CSV —
   deliberately deferred until a real MT5 terminal is producing the file, so it
   can be verified against real output rather than guessed.

The EA is read-only (no order calls) and logs a warning on a non-demo account.

---

## Legacy: Python/TCP bridge (Windows host/VM only — NOT native Linux)

Read-only architecture for a real Windows machine:

```
[Linux box: engine + Hermes + SQLite]
        │
        │  TCP NDJSON  (default 127.0.0.1:7777, set MME_MT5_HOST for cross-host)
        ▼
[Windows host / VM: MT5 terminal + mt5_bridge.py]
        │
        ▼
   broker (demo account)
```

Protocol details: see [`agent/mt5_bridge/protocol.md`](../agent/mt5_bridge/protocol.md).

## Windows host setup

1. Install **MetaTrader 5** terminal. Log it into your broker's **demo** account.
   The bridge will refuse to start against a non-demo account.
2. Install Python 3.10+ on the same Windows host.
3. From a fresh PowerShell:
   ```
   pip install -r agent\mt5_bridge\requirements.txt
   ```
4. Start the bridge:
   ```
   python agent\mt5_bridge\mt5_bridge.py
   ```
   The terminal must be running and logged in before you launch the bridge.

### Cross-host networking

By default the bridge binds `127.0.0.1` — engine and bridge must be on the same host.
For the recommended split (engine on Linux, bridge on Windows VM), bind to the LAN address:

```
set MT5_BRIDGE_BIND=0.0.0.0
python agent\mt5_bridge\mt5_bridge.py
```

Then from the Linux engine host:

```
export MME_PROVIDER=mt5
export MME_MT5_HOST=192.168.1.50   # the Windows VM's IP
export MME_MT5_PORT=7777
./engine
```

There is no TLS or auth on this socket — only run it on a trusted private network
(LAN, VPN, WireGuard, etc.). For hostile networks, tunnel it through SSH.

## Engine integration

| Variable        | Default            | Meaning                                                  |
|-----------------|--------------------|----------------------------------------------------------|
| `MME_PROVIDER`  | `twelvedata`       | `mt5` switches the engine to use the bridge              |
| `MME_MT5_HOST`  | `127.0.0.1`        | Bridge host                                              |
| `MME_MT5_PORT`  | `7777`             | Bridge port                                              |
| `MME_DB_PATH`   | `../data/engine.db`| Override the SQLite path (use `engine_mt5.db` for A/B)   |

The engine pings the bridge at startup, prints `demo_only=Y/N`, and refuses to start
if the bridge reports `demo_only=false` — Phase 1 is read-only and that flag being
clear is a tripwire, never expected.

## Live-trading gate (Phase 3+ — not active in Phase 1)

The bridge has no order ops yet. When they arrive in Phase 3, BOTH of the following
must be true for the bridge to enable them:

1. `ENABLE_LIVE_TRADING=true` in the bridge's environment, AND
2. `confirm_live_trading.txt` exists in the bridge's working directory.

Either one missing → bridge stays demo-only. The C++ engine echoes the bridge's
`demo_only` flag at startup so misconfiguration is visible in the engine log too.

## Health probe (Linux side)

```
ops/probe_mt5_bridge.sh                  # localhost
ops/probe_mt5_bridge.sh 192.168.1.50 7777
```

Exit codes:
- `0` — bridge alive, `demo_only=true`
- `1` — bridge unreachable
- `2` — bridge reachable but `demo_only=false` (investigate immediately)

## Mock mode (for testing without MT5)

Run the bridge on Linux with `MT5_BRIDGE_MOCK=true` — it skips the `MetaTrader5`
import and serves synthetic quotes. Useful for verifying the wire protocol end-to-end
on a single Linux host before the Windows VM is wired up.

```
MT5_BRIDGE_MOCK=true python3 agent/mt5_bridge/mt5_bridge.py &
MME_PROVIDER=mt5 MME_DB_PATH=../data/engine_mt5_test.db ./engine
```
