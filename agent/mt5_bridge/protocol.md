# MT5 Bridge Protocol (Phase 1 — read-only)

Wire format between the C++ engine (Linux) and the Python MT5 sidecar (Windows).

## Transport

- **TCP**, newline-delimited JSON (NDJSON). One JSON object per line, terminated by `\n`.
- Default bind: `127.0.0.1:7777`. Override via `MT5_BRIDGE_BIND` / `MT5_BRIDGE_PORT`.
- No TLS. Cross-host binding (anything other than `127.0.0.1`) is permitted only when the
  network between the engine host and the bridge host is trusted (LAN / private VPN).
- The C++ client opens **one persistent connection** and serializes requests on it.
  On any I/O error the client closes the socket and reconnects lazily on the next call,
  with exponential backoff capped at 30s.

## Request envelope

```json
{"op": "<operation>", "...op-specific fields..."}
```

## Response envelope

Success:
```json
{"ok": true, "...op-specific fields..."}
```

Failure:
```json
{"ok": false, "error": "<human-readable reason>"}
```

The bridge never throws across the wire — every request gets exactly one response line.

## Operations (Phase 1)

### `ping`

Heartbeat. Used by `ops/probe_mt5_bridge.sh` and by the engine on (re)connect.

Request:
```json
{"op": "ping"}
```

Response:
```json
{"ok": true, "pong": 1735689600, "demo_only": true, "account": {"login": 12345678, "server": "MetaQuotes-Demo", "mode": "DEMO"}}
```

- `pong` — server's Unix timestamp at reply time.
- `demo_only` — `true` unless **both** `ENABLE_LIVE_TRADING=true` and `confirm_live_trading.txt`
  are present at bridge startup. The C++ engine MUST verify `demo_only == true` until
  Phase 3 lands the execution path; refusing to operate otherwise.
- `account` — read-only descriptor of the connected MT5 account.

### `quote`

Fetch the current bid/ask for a symbol.

Request:
```json
{"op": "quote", "symbol": "EURUSD"}
```

Response (success):
```json
{"ok": true, "symbol": "EURUSD", "bid": 1.08502, "ask": 1.08504, "price": 1.08503, "ts": 1735689600}
```

- `price` is the mid `(bid + ask) / 2`. The C++ engine consumes `price` in Phase 1;
  `bid`/`ask` are forwarded for Phase 2 spread checks.
- `ts` is the broker-side tick timestamp (Unix seconds).

Response (failure — unknown symbol, MT5 returned no tick, etc.):
```json
{"ok": false, "error": "no quote for EURUSD"}
```

## Operations explicitly NOT in Phase 1

- `bars` — historical candles. Phase 2.
- `order_send`, `order_modify`, `order_close`, `positions`, `account_balance`.
  These do not exist in the bridge code yet. Phase 3 introduces them, gated on:
  1. `ENABLE_LIVE_TRADING=true` environment variable, AND
  2. `confirm_live_trading.txt` file present in the bridge working directory.

Any client that sends an unrecognized `op` gets `{"ok": false, "error": "unknown op: ..."}`.

## Symbol naming

Broker-dependent. The bridge does no name translation — whatever string the client sends
is passed verbatim to `MetaTrader5.symbol_info_tick(symbol)`. The engine-side symbol table
holds the canonical broker name (e.g. `EURUSD`, `XAUUSD`, `XTIUSD`) per provider.

## Mock mode

`MT5_BRIDGE_MOCK=true` skips `MetaTrader5` import entirely and returns synthetic quotes
from a hardcoded table. Used for end-to-end testing the wire protocol on Linux without
a real terminal. Mock mode forces `demo_only = true` and disables any future order ops.
