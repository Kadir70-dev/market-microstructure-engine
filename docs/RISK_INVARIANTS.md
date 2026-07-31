# Risk Invariants

This file restates the frozen Architecture Version 1.0, Part 10. Changes
require the same written approval as a live-trading decision.

1. No position may exist without a broker-side protective stop. A symbol that
   cannot carry one is untradable.
2. Sum of fills equals position volume per ticket and per symbol.
3. No order exists without a preceding risk approval.
4. No two orders share a `broker_order_ref`.
5. Reserved exposure is never below actual exposure; `UNKNOWN` orders reserve
   full exposure.
6. Kill state survives process death.
7. Live mode requires all nine independent architecture guards.
8. Exactly one engine instance may hold the trading lock; stale session epochs
   are rejected.
9. Effective risk limits must be hash-verified at startup and recorded in the
   journal header.
10. Startup self-tests must reject zero, inconsistent, or incorrectly ordered
    risk limits.

## Phase 6 effective limits

`config/limits.json` is the effective paper/replay limit set. Its SHA-256 is
recorded below and must match at startup; a mismatch fails closed unless the
architecture-required human `approved_by` override is present.

SHA-256: `ace90a9ce89f77c638cdd7885b28dc2ba93d6a8730f0c2b4e2d3524f3ce9b9dc`

Live trading remains disabled. Safety Rule #0 remains in force.
