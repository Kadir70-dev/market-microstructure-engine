"""MT5 bridge — sidecar that exposes the MetaTrader5 terminal over a TCP NDJSON protocol.

Read-only in Phase 1: only `ping` and `quote` ops. Order execution arrives in Phase 3
behind the ENABLE_LIVE_TRADING + confirm_live_trading.txt double gate.
"""
