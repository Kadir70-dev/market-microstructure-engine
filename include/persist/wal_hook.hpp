#pragma once
#include <cstddef>

#include "exec/journal.hpp"

// Phase J (blocker fix) -- the minimal seam ShardedOms (sharded_oms.hpp)
// needs to become durability-hooked without depending on the concrete WAL
// writer (persist::LiveWalRecorder, live_wal_recorder.hpp) or spawning any
// thread of its own. Mirrors risk::RiskGate's (risk_gate.hpp) established
// shape: a tiny abstract interface, an optional pointer defaulted to
// nullptr on ShardedOms so every existing Phase C-I call site is completely
// unaffected, and a concrete implementation living in a separate file this
// one does not need to know about.
//
// record() is called from inside ShardedOms::apply() -- i.e. from that
// shard's own single-writer worker thread, the exact same thread that just
// applied the operation this record describes. That is deliberate, not
// incidental: it is what makes WAL record ordering for a given order
// provably match the order ShardedOms actually applied its events, for
// free, using the single-writer-per-shard guarantee Phase C already
// established -- an outbox fed from any other thread (e.g. the caller of
// submit_create()) cannot make that guarantee, because two different
// callers' threads racing to touch the same order are only ever ordered by
// which one the shard's queue happens to drain first, and a WAL write
// issued from the *caller's* thread after its own call returns could be
// reordered relative to a concurrent caller's write in a way this shard's
// own apply()-time ordering never allows.
//
// record() itself MUST be non-blocking (never perform disk I/O): it pushes
// onto a preallocated, bounded outbox a background thread drains
// separately. Returning false means backpressure (the outbox was full) --
// ShardedOms surfaces this as an observable dropped-record counter; it does
// not itself fail the operation, which has already applied in memory by
// the time this hook runs. Whether backpressure here should refuse to
// *admit new exposure* going forward is a policy decision made one layer
// up (see oms/risk_gated_router.hpp, which checks durability pressure
// before calling ShardedOms at all for creates).

namespace persist {

class WalHook {
public:
    virtual ~WalHook() = default;
    [[nodiscard]] virtual bool record(std::size_t shard_index, const exec::JournalRecord& rec) noexcept = 0;
};

}  // namespace persist
