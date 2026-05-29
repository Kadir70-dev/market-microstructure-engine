"""Hermes — analysis-only intelligence layer for the market-microstructure-engine.

Read-only. Never places trades, never connects to brokers, never executes orders.
Reads SQLite (data/engine.db) and produces markdown reports.
"""
