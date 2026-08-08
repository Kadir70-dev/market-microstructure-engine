#include <cstring>

#include "oms/oms.hpp"
#include "phase6_test.hpp"

// Phase B — OMS capacity/storage redesign. Covers exactly the properties the
// redesign added that tests/cpp/phase6_oms_states.cpp, phase6_idempotency.cpp
// and phase6_recovery*.cpp do not: capacity well beyond 256, O(1) lookup
// correctness under heavy hash collision, terminal-slot reclaim, and stale-ref
// rejection after reclaim. Existing OMS/recovery/replay tests are left
// untouched and are the regression proof that this redesign preserved
// behavior; this file is the new-capability proof.

namespace {

risk::Approval approval() {
    risk::RiskEngine e(risk::Limits{});
    risk::Request q{};
    q.volume = 1; q.risk_minor = 1; q.free_margin = 1'000'000; q.warm_mask = 1; q.session_open = true;
    return e.check(q).token;
}

}  // namespace

int main() {
    Phase6Test t;
    const auto token = approval();

    // ---- 1,000,000 simultaneously live orders ------------------------------
    {
        oms::Oms o(1'000'000);
        t.check(o.capacity() == 1'000'000, "1M: capacity honored");
        bool all_created = true;
        for (std::uint64_t i = 1; i <= 1'000'000; ++i) {
            exec::BrokerOrderRef ref{1, i};
            if (o.create(ref, static_cast<std::uint32_t>(i % 8), 1, token) == nullptr) {
                all_created = false;
                break;
            }
        }
        t.check(all_created, "1M: every create succeeded");
        t.check(o.size() == 1'000'000, "1M: size reflects full occupancy");

        // Spot-check O(1) find() correctness across the range, not just at the
        // edges -- a broken probe chain would still find early/late entries.
        bool all_found = true;
        for (std::uint64_t i = 1; i <= 1'000'000; i += 9973) {  // prime stride, irregular coverage
            const auto* order = o.find(exec::BrokerOrderRef{1, i});
            if (order == nullptr || order->ref.logical_order_id != i) { all_found = false; break; }
        }
        t.check(all_found, "1M: sampled find() correctness across full range");
        t.check(o.find(exec::BrokerOrderRef{1, 1'000'001}) == nullptr, "1M: absent ref correctly not found");
    }

    // ---- capacity boundary / fail-closed -----------------------------------
    {
        oms::Oms o(4);
        for (std::uint64_t i = 1; i <= 4; ++i)
            t.check(o.create(exec::BrokerOrderRef{1, i}, 0, 1, token) != nullptr, "boundary: fill to capacity");
        t.check(o.size() == 4, "boundary: size at capacity");
        t.check(o.create(exec::BrokerOrderRef{1, 5}, 0, 1, token) == nullptr, "boundary: 5th create rejected");
        t.check(o.size() == 4, "boundary: rejection did not grow size");
        // restore() must fail closed the same way.
        exec::Order extra{};
        extra.ref = exec::BrokerOrderRef{1, 6}; extra.requested_volume = 1; extra.state = exec::OrderState::sent;
        t.check(o.restore(extra) == nullptr, "boundary: restore() also fails closed at capacity");
    }

    // ---- heavy hash-collision stress ----------------------------------------
    // Fill to 100% of capacity (index table is sized 2x, so this is a 50%
    // load factor on the index -- collisions are frequent at that density).
    // Every one of these must still resolve to the exact right order.
    {
        constexpr std::uint64_t n = 50'000;
        oms::Oms o(n);
        for (std::uint64_t i = 1; i <= n; ++i)
            (void)o.create(exec::BrokerOrderRef{7, i * 2654435761ULL}, static_cast<std::uint32_t>(i % 8), 1, token);
        t.check(o.size() == n, "collision: full occupancy reached");
        bool ok = true;
        for (std::uint64_t i = 1; i <= n; ++i) {
            const auto ref = exec::BrokerOrderRef{7, i * 2654435761ULL};
            const auto* order = o.find(ref);
            if (order == nullptr || !(order->ref == ref)) { ok = false; break; }
        }
        t.check(ok, "collision: every order resolves to its exact ref under heavy probing");
        // A ref that was never inserted, but whose hash may probe through many
        // of the same occupied slots, must still correctly report absent.
        t.check(o.find(exec::BrokerOrderRef{7, 0}) == nullptr, "collision: negative lookup still correct");
    }

    // ---- terminal-slot reclamation, reuse, and stale-ref protection --------
    {
        oms::Oms o(2);
        exec::BrokerOrderRef ref_a{1, 100}, ref_b{1, 200};
        auto* a = o.create(ref_a, 0, 10, token);
        t.check(a != nullptr, "reclaim: order A created");
        t.check(o.transition(ref_a, exec::OrderState::sent), "reclaim: A -> sent");
        t.check(o.transition(ref_a, exec::OrderState::rejected), "reclaim: A -> rejected (terminal)");
        t.check(o.size() == 0, "reclaim: terminal transition freed the slot");
        t.check(o.reserved_exposure() == 0, "reclaim: exposure released on terminal");

        // Stale ref: A must no longer be findable or transitionable at all,
        // not merely "found but rejected by the state machine".
        t.check(o.find(ref_a) == nullptr, "reclaim: stale ref A not found");
        t.check(!o.transition(ref_a, exec::OrderState::sent), "reclaim: stale ref A cannot transition");
        t.check(!o.mark_unknown_on_restart(ref_a), "reclaim: stale ref A cannot be marked unknown");

        // The freed slot must be reusable by a different order, and that new
        // order must be fully correct (not confused with A's old data).
        auto* b = o.create(ref_b, 3, 25, token);
        t.check(b != nullptr, "reclaim: slot reused for order B");
        t.check(o.size() == 1, "reclaim: size reflects B only");
        t.check(o.find(ref_b) != nullptr && o.find(ref_b)->requested_volume == 25 &&
                    o.find(ref_b)->symbol_id == 3,
                "reclaim: B's data is correct, not A's stale bytes");
        t.check(o.find(ref_a) == nullptr, "reclaim: A still not found after B reuses its slot");

        // Fill to capacity again to prove the reclaimed capacity is genuinely
        // available, not permanently lost.
        t.check(o.create(exec::BrokerOrderRef{1, 300}, 0, 1, token) != nullptr,
                "reclaim: reclaimed capacity is usable, not leaked");
    }

    // ---- reclaim under swap-removal: dense [0,size()) with no holes --------
    // Three orders, remove the middle one -> the array must stay dense so
    // size()/at() enumeration (used by recovery.hpp and replay comparisons)
    // never walks a hole.
    {
        oms::Oms o(3);
        exec::BrokerOrderRef r1{1, 1}, r2{1, 2}, r3{1, 3};
        (void)o.create(r1, 0, 1, token);
        (void)o.create(r2, 0, 1, token);
        (void)o.create(r3, 0, 1, token);
        t.check(o.transition(r2, exec::OrderState::sent) && o.transition(r2, exec::OrderState::rejected),
                "dense: middle order reaches terminal");
        t.check(o.size() == 2, "dense: size reflects the two survivors");
        bool r1_found = false, r3_found = false;
        for (std::size_t i = 0; i < o.size(); ++i) {
            const auto& ord = o.at(i);
            if (ord.ref == r1) r1_found = true;
            if (ord.ref == r3) r3_found = true;
            t.check(!(ord.ref == r2), "dense: [0,size()) contains no trace of the reclaimed order");
        }
        t.check(r1_found && r3_found, "dense: both survivors present in [0,size())");
    }

    // ---- partial fill / idempotency at the Oms level (regression, direct) --
    {
        oms::Oms o(2);
        exec::BrokerOrderRef ref{9, 9};
        t.check(o.create(ref, 0, 10, token) != nullptr, "fill: created");
        t.check(o.create(ref, 0, 10, token) == nullptr, "fill: duplicate ref rejected (idempotency)");
        t.check(o.transition(ref, exec::OrderState::sent) && o.transition(ref, exec::OrderState::acknowledged),
                "fill: reaches acknowledged");
        t.check(o.fill(ref, 4), "fill: partial fill accepted");
        t.check(o.find(ref)->state == exec::OrderState::partially_filled, "fill: partially_filled after 4/10");
        t.check(o.fill(ref, 6), "fill: remainder filled");
        // The final fill's transition to `filled` (terminal) reclaims the slot,
        // so ref is correctly no longer find()-able -- same stale-ref contract
        // proven above. Its raw slot content is untouched by reclaim (only the
        // index entry and count_ change), so at(0) still shows the terminal
        // state, exactly as phase6_recovery_cases.cpp's "terminal preserved"
        // case already relies on for the identical reason.
        t.check(o.find(ref) == nullptr, "fill: terminal fill reclaims -- ref no longer findable");
        t.check(o.size() == 0, "fill: terminal fill reclaimed the slot");
        t.check(o.at(0).state == exec::OrderState::filled, "fill: reclaimed slot's raw content still shows filled");
    }

    // ---- deterministic replay: same op sequence, two instances, byte-equal -
    {
        constexpr std::uint64_t n = 5'000;
        auto run = [&](oms::Oms& o) {
            for (std::uint64_t i = 1; i <= n; ++i) {
                const exec::BrokerOrderRef ref{1, i};
                (void)o.create(ref, static_cast<std::uint32_t>(i % 8), static_cast<std::int64_t>(1 + i % 5), token);
                if (i % 3 == 0) {
                    (void)o.transition(ref, exec::OrderState::sent);
                    (void)o.transition(ref, exec::OrderState::rejected);  // terminal -> reclaims
                }
            }
        };
        oms::Oms first(n), second(n);
        run(first);
        run(second);
        t.check(first.size() == second.size(), "determinism: identical size after identical ops");
        t.check(first.reserved_exposure() == second.reserved_exposure(),
                "determinism: identical reserved exposure");
        bool byte_identical = true;
        for (std::size_t i = 0; i < first.size(); ++i)
            if (std::memcmp(&first.at(i), &second.at(i), sizeof(exec::Order)) != 0) { byte_identical = false; break; }
        t.check(byte_identical, "determinism: two independent runs of the same op sequence are byte-identical");
    }

    return t.result();
}
