"""MT5 sidecar (Phase 1 — read-only).

Exposes a tiny NDJSON-over-TCP protocol that the C++ engine connects to.
Operations: `ping`, `quote`. Order ops are deliberately absent — they arrive
in Phase 3 behind the DEMO_ONLY double gate.

Run on the Windows host where MT5 terminal is installed:

    python mt5_bridge.py

Environment:
    MT5_BRIDGE_BIND          bind host  (default 127.0.0.1)
    MT5_BRIDGE_PORT          bind port  (default 7777)
    MT5_BRIDGE_MOCK          'true' to skip MetaTrader5 import (Linux dev/testing)
    MT5_LOGIN/MT5_PASSWORD/MT5_SERVER   optional explicit login (else uses terminal's logged-in account)
    ENABLE_LIVE_TRADING      'true' is one half of the live-trading gate (Phase 3+)
    (./confirm_live_trading.txt must also exist for the other half)
"""

from __future__ import annotations

import json
import logging
import os
import socketserver
import sys
import threading
import time

BIND_HOST = os.environ.get("MT5_BRIDGE_BIND", "127.0.0.1")
BIND_PORT = int(os.environ.get("MT5_BRIDGE_PORT", "7777"))
MOCK_MODE = os.environ.get("MT5_BRIDGE_MOCK", "").lower() == "true"

logger = logging.getLogger("mt5_bridge")

# MetaTrader5 is Windows-only and not thread-safe. Serialize all calls.
_mt5_lock = threading.Lock()

mt5 = None  # populated by init_mt5() unless MOCK_MODE
_account_info_cache: dict = {}


def live_trading_enabled() -> bool:
    """Returns True only if BOTH gates are open. Phase 1 never exercises this path."""
    if os.environ.get("ENABLE_LIVE_TRADING", "").lower() != "true":
        return False
    return os.path.isfile("confirm_live_trading.txt")


def init_mt5() -> bool:
    """Connect to the MT5 terminal. In MOCK mode, sets up fake state instead."""
    global mt5, _account_info_cache

    if MOCK_MODE:
        logger.warning("MOCK mode — no MetaTrader5 import; serving synthetic quotes")
        _account_info_cache = {
            "login": 0,
            "server": "MOCK",
            "mode": "DEMO",
            "trade_mode_raw": "mock",
        }
        return True

    try:
        import MetaTrader5 as _mt5  # type: ignore
    except ImportError:
        logger.error(
            "MetaTrader5 package not installed. "
            "On Windows: pip install MetaTrader5. On Linux: set MT5_BRIDGE_MOCK=true."
        )
        return False
    mt5 = _mt5

    login = os.environ.get("MT5_LOGIN")
    password = os.environ.get("MT5_PASSWORD")
    server = os.environ.get("MT5_SERVER")

    if login and password and server:
        ok = mt5.initialize(login=int(login), password=password, server=server)
    else:
        ok = mt5.initialize()

    if not ok:
        logger.error(f"mt5.initialize failed: {mt5.last_error()}")
        return False

    info = mt5.account_info()
    if info is None:
        logger.error("mt5.account_info returned None — terminal not logged in?")
        mt5.shutdown()
        return False

    is_demo = info.trade_mode == mt5.ACCOUNT_TRADE_MODE_DEMO
    mode_label = "DEMO" if is_demo else (
        "LIVE" if info.trade_mode == mt5.ACCOUNT_TRADE_MODE_REAL else "CONTEST"
    )

    # Phase 1 hard guard: refuse to start against a non-demo account unless the
    # operator has explicitly opened the live-trading gate. Even then Phase 1
    # has no order ops, so this is purely defensive.
    if not is_demo and not live_trading_enabled():
        logger.error(
            f"Connected account #{info.login} is {mode_label}, not DEMO. "
            "Refusing to start. Set ENABLE_LIVE_TRADING=true AND create "
            "./confirm_live_trading.txt to override (you almost certainly should not)."
        )
        mt5.shutdown()
        return False

    _account_info_cache = {
        "login": int(info.login),
        "server": str(info.server),
        "mode": mode_label,
    }
    logger.info(
        f"MT5 initialized — account #{info.login} server={info.server} mode={mode_label}"
    )
    return True


# Mock quote source — small random walk so the engine sees non-stale data.
_mock_state: dict[str, list[float]] = {
    "EURUSD": [1.0850, 1.0852],
    "XAUUSD": [2350.10, 2350.60],
    "XTIUSD": [78.50, 78.55],
}


def _mock_quote(symbol: str) -> dict | None:
    import random
    state = _mock_state.get(symbol)
    if state is None:
        return None
    drift = (random.random() - 0.5) * state[0] * 1e-4
    bid = state[0] + drift
    ask = state[1] + drift
    _mock_state[symbol] = [bid, ask]
    return {
        "symbol": symbol,
        "bid": round(bid, 5),
        "ask": round(ask, 5),
        "price": round((bid + ask) / 2.0, 5),
        "ts": int(time.time()),
    }


def fetch_quote(symbol: str) -> dict | None:
    if MOCK_MODE:
        return _mock_quote(symbol)
    with _mt5_lock:
        tick = mt5.symbol_info_tick(symbol)
    if tick is None or (tick.bid == 0 and tick.ask == 0):
        return None
    bid = float(tick.bid)
    ask = float(tick.ask)
    return {
        "symbol": symbol,
        "bid": bid,
        "ask": ask,
        "price": (bid + ask) / 2.0,
        "ts": int(tick.time),
    }


def handle_request(req: dict) -> dict:
    op = req.get("op")
    if op == "ping":
        return {
            "ok": True,
            "pong": int(time.time()),
            "demo_only": not live_trading_enabled() or MOCK_MODE,
            "account": _account_info_cache,
        }
    if op == "quote":
        sym = req.get("symbol")
        if not isinstance(sym, str) or not sym:
            return {"ok": False, "error": "missing or invalid 'symbol'"}
        q = fetch_quote(sym)
        if q is None:
            return {"ok": False, "error": f"no quote for {sym}"}
        return {"ok": True, **q}
    return {"ok": False, "error": f"unknown op: {op!r}"}


class _LineHandler(socketserver.StreamRequestHandler):
    # Cap each request line at 64KB — quote payload is ~120 bytes, so this is generous.
    rbufsize = 65536

    def handle(self) -> None:
        client = self.client_address
        logger.info(f"client connected: {client}")
        try:
            while True:
                line = self.rfile.readline(65536)
                if not line:
                    break
                line = line.strip()
                if not line:
                    continue
                try:
                    req = json.loads(line.decode("utf-8"))
                except json.JSONDecodeError as e:
                    self._reply({"ok": False, "error": f"json parse: {e}"})
                    continue
                try:
                    resp = handle_request(req)
                except Exception as e:  # never let a request take down the server
                    logger.exception("handler error")
                    resp = {"ok": False, "error": f"internal: {e}"}
                self._reply(resp)
        except (ConnectionResetError, BrokenPipeError):
            pass
        finally:
            logger.info(f"client disconnected: {client}")

    def _reply(self, obj: dict) -> None:
        self.wfile.write((json.dumps(obj) + "\n").encode("utf-8"))
        self.wfile.flush()


class _ThreadedServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main() -> int:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )

    if BIND_HOST != "127.0.0.1":
        logger.warning(
            f"Binding to non-localhost address {BIND_HOST}. "
            "Make sure the network path to the engine host is trusted (LAN/VPN); "
            "this protocol has no TLS or auth."
        )

    if not init_mt5():
        return 1

    gate = "OPEN (LIVE TRADING ENABLED)" if live_trading_enabled() else "CLOSED (demo-only)"
    logger.info(f"live-trading gate: {gate}")
    logger.info("Phase 1: read-only — only `ping` and `quote` ops are served.")

    try:
        with _ThreadedServer((BIND_HOST, BIND_PORT), _LineHandler) as srv:
            logger.info(f"listening on {BIND_HOST}:{BIND_PORT}")
            srv.serve_forever()
    except KeyboardInterrupt:
        logger.info("shutting down (SIGINT)")
    finally:
        if mt5 is not None and not MOCK_MODE:
            mt5.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
