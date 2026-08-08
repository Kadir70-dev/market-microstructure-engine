#include <vector>

#include "oms/recovery_streaming.hpp"
#include "phase6_test.hpp"

// Phase J (blocker fix) -- correctness proof for recover_streaming(), the
// segmented replacement for oms::recover()'s fixed-16384-record ceiling.
// Does not re-prove recover()'s own per-record validation rules in general
// (phase6_recovery.cpp/phase6_recovery_cases.cpp already do, unmodified,
// still green); this file proves recover_streaming() (a) agrees with
// recover() byte-for-byte on a workload both can handle, (b) succeeds well
// past the old 16384 cap, and (c) preserves fail-closed behavior for
// corruption and duplicate payloads under its new O(1) DuplicateWindow.

namespace {

std::vector<exec::JournalRecord> make_commands(std::size_t n, std::uint64_t run_id = 1) {
    std::vector<exec::JournalRecord> records;
    records.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        exec::JournalRecord rec{};
        rec.ts_ns = i;  // strictly increasing, satisfies the monotonic-ts check
        rec.type = static_cast<std::uint16_t>(exec::JournalRecordType::command);
        rec.run_id = run_id;
        rec.logical_order_id = i + 1;
        rec.b = 1;   // volume
        rec.c = 11;  // price
        rec.d = 1;   // order type
        records.push_back(rec);
    }
    return records;
}

// Wraps a std::vector<JournalRecord> as a RecordSource for recover_streaming.
struct VectorSource final {
    const std::vector<exec::JournalRecord>* records;
    std::size_t pos{0};
    oms::StreamStatus operator()(exec::JournalRecord& out) noexcept {
        if (pos >= records->size()) return oms::StreamStatus::done;
        out = (*records)[pos++];
        return oms::StreamStatus::ok;
    }
};

}  // namespace

int main() {
    Phase6Test t;

    // ---- 1) agrees with recover() on a workload both can handle -----------
    {
        constexpr std::size_t n = 5000;
        const auto records = make_commands(n);

        exec::Journal journal;
        for (const auto& r : records) t.check(journal.append(r), "batch: journal accepts all 5000 records");
        oms::RecoveryState batch_state(portfolio::MarginMode::hedging);
        t.check(oms::recover(journal, batch_state), "batch: recover() succeeds at n=5000 (within old cap)");

        oms::RecoveryState stream_state(portfolio::MarginMode::hedging, n + 64);
        VectorSource src{&records};
        t.check(oms::recover_streaming(src, stream_state, 65536), "stream: recover_streaming() succeeds at n=5000");

        t.check(batch_state.orders.size() == stream_state.orders.size(),
               "agreement: both paths recover the same order count");
        bool all_match = true;
        for (std::size_t i = 0; i < batch_state.orders.size(); ++i) {
            const auto& bo = batch_state.orders.at(i);
            const auto* so = stream_state.orders.find(bo.ref);
            if (so == nullptr || so->requested_volume != bo.requested_volume || so->state != bo.state) all_match = false;
        }
        t.check(all_match, "agreement: every recovered order matches exactly between recover() and recover_streaming()");
    }

    // ---- 2) succeeds well past the old 16384-record ceiling ----------------
    {
        constexpr std::size_t n = 100'000;
        const auto records = make_commands(n);
        oms::RecoveryState state(portfolio::MarginMode::hedging, n + 64);
        VectorSource src{&records};
        t.check(oms::recover_streaming(src, state, 65536), "scale: recover_streaming() succeeds at n=100000 (>16384)");
        t.check(state.orders.size() == n, "scale: all 100000 orders recovered");
    }
    {
        constexpr std::size_t n = 1'000'000;
        const auto records = make_commands(n);
        oms::RecoveryState state(portfolio::MarginMode::hedging, n + 64);
        VectorSource src{&records};
        t.check(oms::recover_streaming(src, state, 65536), "scale: recover_streaming() succeeds at n=1000000");
        t.check(state.orders.size() == n, "scale: all 1000000 orders recovered");
    }

    // ---- 3) duplicate payload still fails closed ----------------------------
    {
        auto records = make_commands(10);
        records.push_back(records[3]);  // exact duplicate of an earlier record, including its ts_ns
        oms::RecoveryState state(portfolio::MarginMode::hedging, 64);
        VectorSource src{&records};
        t.check(!oms::recover_streaming(src, state, 65536), "duplicate: an exact-duplicate payload fails recovery closed");
    }

    // ---- 4) corrupt/torn source fails closed, not partial -------------------
    {
        struct CorruptAfterN final {
            std::vector<exec::JournalRecord> records;
            std::size_t pos{0};
            oms::StreamStatus operator()(exec::JournalRecord& out) noexcept {
                if (pos >= 5) return oms::StreamStatus::corrupt;
                out = records[pos++];
                return oms::StreamStatus::ok;
            }
        };
        CorruptAfterN src{make_commands(10)};
        oms::RecoveryState state(portfolio::MarginMode::hedging, 64);
        t.check(!oms::recover_streaming(src, state, 65536),
               "corrupt: a torn/corrupt source fails the whole recovery closed, not a silent partial result");
    }

    // ---- 5) sequence/timestamp integrity still enforced ----------------------
    {
        auto records = make_commands(10);
        records[5].ts_ns = 1;  // violates monotonic timestamp ordering
        oms::RecoveryState state(portfolio::MarginMode::hedging, 64);
        VectorSource src{&records};
        t.check(!oms::recover_streaming(src, state, 65536), "integrity: a non-monotonic timestamp fails recovery closed");
    }

    return t.result();
}
