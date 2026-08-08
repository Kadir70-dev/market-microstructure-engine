#pragma once
#include <cstdint>

#include "exec/exec_types.hpp"

// Phase I -- the minimal seam Phase F's VenueConnection (multi_venue.hpp)
// needs to become risk-gated without depending on risk_ledger.hpp (which in
// turn depends on multi_venue.hpp's VenueId/StrategyId aliases) and without
// changing route_create()/route_replace()'s existing signatures (both are
// Phase G's public API, unchanged by this file). A tiny abstract interface
// here, implemented by risk::RiskLedger (risk_ledger.hpp), is the entire
// coupling: VenueConnection holds an optional RiskGate*, defaulted to
// nullptr so every pre-Phase-I call site (all of Phase F/G/H's own tests)
// is completely unaffected; attaching a gate is what turns route_create/
// route_replace into mandatory-risk-checked entry points for symbol/venue/
// global exposure, closing the "raw submit_create reachable" gap Phase G
// and Phase H both flagged -- at its root, not via a parallel gated path
// that a caller could still route around.
//
// No StrategyId or exec::Side parameter here: route_create()/route_replace()
// carry neither (verified in the Phase I audit -- Oms::create() itself has
// no Side parameter at all), so a gate attached at this seam can only ever
// see venue/symbol/volume/ref. Strategy-level attribution and Side-aware
// position accounting require genuine strategy identity, which only exists
// one layer up, in the Phase H arbitration output -- that is what
// include/oms/risk_gated_router.hpp's RiskGatedRouter supplies, using this
// same RiskGate underneath plus its own explicit per-strategy reservation.

namespace risk {

using VenueId = std::uint64_t;  // mirrors oms::VenueId (multi_venue.hpp) exactly; kept as a
                                 // separate alias here specifically to avoid this header
                                 // depending on multi_venue.hpp.

class RiskGate {
public:
    virtual ~RiskGate() = default;

    // Called after VenueConnection's own session/connection-state/rate-
    // limiter gates already admitted the request, immediately before the
    // underlying ShardedOms::submit_create() call -- true admits, false
    // fails the whole route_create() call closed (Decision::rejected),
    // exactly as a rate-limit or session rejection already would.
    [[nodiscard]] virtual bool admit_create(VenueId venue, std::uint32_t symbol, std::int64_t volume,
                                            exec::BrokerOrderRef ref) noexcept = 0;
    [[nodiscard]] virtual bool admit_replace(VenueId venue, exec::BrokerOrderRef ref,
                                             std::int64_t new_volume) noexcept = 0;
};

}  // namespace risk
