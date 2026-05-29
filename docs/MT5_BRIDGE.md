# MT5 Bridge — setup & operations

**MT5 is the engine's only data source.** Read-only architecture:

```
[Linux box]  engine + Hermes + SQLite
        │
        │  TCP NDJSON  (default 127.0.0.1:7777; MME_MT5_HOST for cross-host)
        ▼
   mt5_bridge.py  +  MetaTrader 5 terminal
   (same Linux box under Wine, OR a Windows host/VM)
        │
        ▼
   broker (demo account)
```

The engine connects automatically — there is no provider switch to set. Protocol
details: [`agent/mt5_bridge/protocol.md`](../agent/mt5_bridge/protocol.md).

## Linux + Wine setup (no Windows machine)

The `MetaTrader5` Python package is Windows-only, so on Linux both MT5 and the
bridge run **inside a Wine prefix** using Wine's bundled Python:

1. Install Wine, then the **MetaTrader 5** Windows installer inside a prefix; log
   the terminal into your broker's **demo** account (the bridge refuses non-demo).
2. Install **Windows** Python *inside the same Wine prefix* (not native Linux
   python3), e.g. `wine python-3.x.x.exe`.
3. Install the package and run the bridge with the Wine Python:
   ```bash
   wine python.exe -m pip install -r agent/mt5_bridge/requirements.txt
   wine python.exe agent/mt5_bridge/mt5_bridge.py     # keep this running
   ```
4. The native-Linux C++ `engine` connects to it over TCP at `127.0.0.1:7777` —
   no Wine needed on the engine side.

> The engine, Hermes, dashboard, and ML pipeline all run on native Linux; only
> the MT5 terminal + bridge live under Wine. If Wine-MT5 is flaky, use **mock
> mode** (below) to keep the rest of the system exercised.

## Windows host setup (alternative — separate Windows machine/VM)

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
export MME_MT5_HOST=192.168.1.50   # the Windows VM's IP
export MME_MT5_PORT=7777
./engine
```

There is no TLS or auth on this socket — only run it on a trusted private network
(LAN, VPN, WireGuard, etc.). For hostile networks, tunnel it through SSH.

## Engine integration

| Variable        | Default            | Meaning                                                  |
|-----------------|--------------------|----------------------------------------------------------|
| `MME_MT5_HOST`  | `127.0.0.1`        | Bridge host                                              |
| `MME_MT5_PORT`  | `7777`             | Bridge port                                              |
| `MME_DB_PATH`   | `../data/engine.db`| Override the SQLite path (e.g. a separate verification DB)|

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
MME_DB_PATH=../data/engine_mt5_test.db ./engine
```
