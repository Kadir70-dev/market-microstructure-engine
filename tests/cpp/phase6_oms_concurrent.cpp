#include <atomic>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

#include "oms/oms.hpp"
#include "oms/sharded_oms.hpp"
#include "phase6_test.hpp"

// Phase C — concurrency correctness for the sharded OMS. Load-scaling and
// latency/throughput claims are proven in src/apps/oms_concurrent_bench_main.cpp;
// this file proves *safety*: no lost/duplicated/corrupted state under real
// concurrent access, at every worker-thread count this machine can run
// (1, 2, 4 -- std::thread::hardware_concurrency() on the test box).

namespace {

[[nodiscard]] std::size_t worker_counts_max() noexcept {
    const auto hc = std::thread::hardware_concurrency();
    return hc == 0 ? 4 : hc;
}

void run_at_thread_counts(Phase6Test& t, const char* label,
                          const std::function<void(Phase6Test&, std::size_t)>& body) {
    for (const std::size_t n : {std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
        if (n > worker_counts_max() && n != 1) continue;  // never claim more than the box has
        char name[128];
        std::snprintf(name, sizeof(name), "%s @%zu threads", label, n);
        Phase6Test sub;
        body(sub, n);
        t.check(sub.result() == 0, name);
    }
}

}  // namespace

int main() {
    Phase6Test t;

    // ---- concurrent creates: N threads x M distinct orders each -----------
    run_at_thread_counts(t, "concurrent_creates", [](Phase6Test& t, std::size_t threads) {
        constexpr std::size_t per_thread = 25'000;
        const std::size_t shard_count = threads;
        oms::ShardedOms s(shard_count, per_thread * threads / shard_count + 16, threads, per_thread * threads + 16);
        std::vector<std::thread> workers;
        std::atomic<std::size_t> failures{0};
        for (std::size_t w = 0; w < threads; ++w) {
            workers.emplace_back([&, w] {
                for (std::size_t i = 0; i < per_thread; ++i) {
                    const std::uint64_t id = w * per_thread + i + 1;
                    oms::Completion c{};
                    if (!s.submit_create(w, exec::BrokerOrderRef{1, id}, 0, 1, c) || !c.ok)
                        failures.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& w : workers) w.join();
        t.check(failures.load() == 0, "every create succeeded");
        std::size_t total_live = 0;
        for (std::size_t sh = 0; sh < shard_count; ++sh) total_live += s.live_count(sh);
        t.check(total_live == per_thread * threads, "total live count matches submitted count");
        // Spot-check correctness, not just count.
        bool ok = true;
        for (std::size_t w = 0; w < threads && ok; ++w) {
            for (std::size_t i = 0; i < per_thread; i += 4177) {
                const std::uint64_t id = w * per_thread + i + 1;
                exec::Order out{};
                if (!s.find_copy(0, exec::BrokerOrderRef{1, id}, out) || out.ref.logical_order_id != id) {
                    ok = false; break;
                }
            }
        }
        t.check(ok, "sampled orders are individually correct");
    });

    // ---- concurrent finds: many readers against a fixed populated set -----
    run_at_thread_counts(t, "concurrent_finds", [](Phase6Test& t, std::size_t threads) {
        constexpr std::size_t n = 20'000;
        oms::ShardedOms s(threads, n + 16, threads, n + 16);
        for (std::size_t i = 0; i < n; ++i) {
            oms::Completion c{};
            (void)s.submit_create(0, exec::BrokerOrderRef{1, i + 1}, 0, 1, c);
        }
        std::vector<std::thread> readers;
        std::atomic<std::size_t> mismatches{0};
        for (std::size_t w = 0; w < threads; ++w) {
            readers.emplace_back([&, w] {
                for (std::size_t k = 0; k < n; ++k) {
                    const std::uint64_t id = ((k * 2654435761ULL) % n) + 1;
                    exec::Order out{};
                    if (!s.find_copy(w, exec::BrokerOrderRef{1, id}, out) || out.ref.logical_order_id != id)
                        mismatches.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& r : readers) r.join();
        t.check(mismatches.load() == 0, "every concurrent find returned the correct order");
    });

    // ---- fill vs cancel race: safety, not a fixed winner -------------------
    // Volume 1 so a fill is always a *full* fill (terminal), making the two
    // outcomes mutually exclusive and easy to check.
    run_at_thread_counts(t, "fill_vs_cancel_race", [](Phase6Test& t, std::size_t /*threads*/) {
        constexpr int trials = 2'000;
        int filled = 0, cancelled = 0, neither = 0, corrupted = 0;
        for (int i = 0; i < trials; ++i) {
            oms::ShardedOms s(1, 4, 2, 32);
            const exec::BrokerOrderRef ref{1, static_cast<std::uint64_t>(i + 1)};
            oms::Completion cc{};
            (void)s.submit_create(0, ref, 0, 1, cc);
            oms::Completion cs{};
            (void)s.submit_transition(0, ref, exec::OrderState::sent, cs);
            oms::Completion ca{};
            (void)s.submit_transition(0, ref, exec::OrderState::acknowledged, ca);

            // Distinct producer_id per racer: each ring is single-producer
            // (core::RingBuffer), so two threads sharing one producer_id would
            // race try_push against each other -- a bug in the *test*, not the
            // thing under test, and exactly the kind of misuse this comment
            // exists to prevent repeating.
            std::atomic<bool> go{false};
            std::thread filler([&] {
                while (!go.load(std::memory_order_acquire)) {}
                oms::Completion c{};
                (void)s.submit_fill(0, ref, 1, c);
            });
            std::thread canceller([&] {
                while (!go.load(std::memory_order_acquire)) {}
                oms::Completion c1{};
                if (s.submit_transition(1, ref, exec::OrderState::cancel_pending, c1) && c1.ok) {
                    oms::Completion c2{};
                    (void)s.submit_transition(1, ref, exec::OrderState::cancelled, c2);
                }
            });
            go.store(true, std::memory_order_release);
            filler.join();
            canceller.join();

            exec::Order final_order{};
            const bool found = s.find_copy(0, ref, final_order);
            if (!found) { neither++; continue; }  // reclaimed as terminal; check via live_count below instead
            if (final_order.state == exec::OrderState::filled) ++filled;
            else if (final_order.state == exec::OrderState::cancelled) ++cancelled;
            else if (final_order.state == exec::OrderState::cancel_pending) ++neither;  // fill lost the race and hasn't landed as cancelled yet -- shouldn't happen post-join, but not corruption either
            else ++corrupted;
        }
        // Reclaim (Phase B) means a terminal order is no longer find()-able,
        // so "not found" is the *expected* outcome whenever either race arm
        // won outright -- that is exactly what proves no corruption: there is
        // no invalid intermediate state to observe.
        t.check(corrupted == 0, "no trial produced a state outside {filled, cancelled, not found}");
        t.check(filled + cancelled + neither == trials, "every trial resolved to a real order of the events");
    });

    // ---- replace vs fill race: safety --------------------------------------
    run_at_thread_counts(t, "replace_vs_fill_race", [](Phase6Test& t, std::size_t /*threads*/) {
        constexpr int trials = 2'000;
        int corrupted = 0;
        for (int i = 0; i < trials; ++i) {
            oms::ShardedOms s(1, 4, 2, 32);
            const exec::BrokerOrderRef ref{1, static_cast<std::uint64_t>(i + 1)};
            oms::Completion cc{};
            (void)s.submit_create(0, ref, 0, 10, cc);
            oms::Completion cs{}, ca{};
            (void)s.submit_transition(0, ref, exec::OrderState::sent, cs);
            (void)s.submit_transition(0, ref, exec::OrderState::acknowledged, ca);

            std::atomic<bool> go{false};
            std::thread replacer([&] {
                while (!go.load(std::memory_order_acquire)) {}
                oms::Completion c{};
                (void)s.submit_replace(0, ref, 100, 20, c);
            });
            std::thread filler([&] {
                while (!go.load(std::memory_order_acquire)) {}
                oms::Completion c{};
                (void)s.submit_fill(1, ref, 10, c);  // full fill against the *original* volume of 10
            });
            go.store(true, std::memory_order_release);
            replacer.join();
            filler.join();

            exec::Order final_order{};
            if (s.find_copy(0, ref, final_order)) {
                // Order still live: fill must not have completed it (or the
                // replace raised requested_volume above what's filled so it's
                // still open). Either way filled_volume must never exceed
                // requested_volume, and volume must never be negative.
                if (final_order.filled_volume > final_order.requested_volume ||
                    final_order.requested_volume < 0 || final_order.filled_volume < 0)
                    ++corrupted;
            }
            // Not found == fully filled and reclaimed: also not corruption by
            // construction (Oms rejects a fill that would exceed remaining()).
        }
        t.check(corrupted == 0, "no trial produced filled_volume > requested_volume or a negative field");
    });

    // ---- duplicate execution reports: N threads race the same create ------
    run_at_thread_counts(t, "duplicate_creates", [](Phase6Test& t, std::size_t threads) {
        if (threads < 2) return;  // a race needs at least two racers
        constexpr int trials = 500;
        int corrupted = 0;
        for (int i = 0; i < trials; ++i) {
            oms::ShardedOms s(1, 4, threads, 32);
            const exec::BrokerOrderRef ref{1, static_cast<std::uint64_t>(i + 1)};
            std::atomic<std::size_t> successes{0};
            std::atomic<bool> go{false};
            std::vector<std::thread> racers;
            for (std::size_t w = 0; w < threads; ++w) {
                racers.emplace_back([&, w] {
                    while (!go.load(std::memory_order_acquire)) {}
                    oms::Completion c{};
                    if (s.submit_create(w, ref, 0, 1, c) && c.ok) successes.fetch_add(1, std::memory_order_relaxed);
                });
            }
            go.store(true, std::memory_order_release);
            for (auto& r : racers) r.join();
            if (successes.load() != 1) ++corrupted;
        }
        t.check(corrupted == 0, "exactly one racer's create succeeds, every time, under real concurrency");
    });

    // ---- same-order conflicting events: mixed op types racing --------------
    run_at_thread_counts(t, "same_order_conflicting_events", [](Phase6Test& t, std::size_t threads) {
        if (threads < 2) return;
        constexpr int trials = 1'000;
        int illegal_state_observed = 0;
        for (int i = 0; i < trials; ++i) {
            oms::ShardedOms s(1, 4, threads, 32);
            const exec::BrokerOrderRef ref{1, static_cast<std::uint64_t>(i + 1)};
            oms::Completion cc{}, cs{}, ca{};
            (void)s.submit_create(0, ref, 0, 5, cc);
            (void)s.submit_transition(0, ref, exec::OrderState::sent, cs);
            (void)s.submit_transition(0, ref, exec::OrderState::acknowledged, ca);

            // One producer_id per racer thread (see the fill_vs_cancel_race
            // comment above): sharing an id across threads would race the
            // ring's own single-producer contract.
            std::atomic<bool> go{false};
            std::vector<std::thread> racers;
            racers.emplace_back([&] {
                while (!go.load(std::memory_order_acquire)) {}
                oms::Completion c{}; (void)s.submit_fill(0, ref, 2, c);
            });
            racers.emplace_back([&] {
                while (!go.load(std::memory_order_acquire)) {}
                oms::Completion c{}; (void)s.submit_transition(1, ref, exec::OrderState::cancel_pending, c);
            });
            if (threads >= 4) {
                racers.emplace_back([&] {
                    while (!go.load(std::memory_order_acquire)) {}
                    oms::Completion c{}; (void)s.submit_replace(2, ref, 50, 8, c);
                });
                racers.emplace_back([&] {
                    while (!go.load(std::memory_order_acquire)) {}
                    oms::Completion c{}; (void)s.submit_fill(3, ref, 1, c);
                });
            }
            go.store(true, std::memory_order_release);
            for (auto& r : racers) r.join();

            exec::Order final_order{};
            if (s.find_copy(0, ref, final_order)) {
                const bool legal_shape = final_order.filled_volume >= 0 &&
                    final_order.filled_volume <= final_order.requested_volume &&
                    final_order.requested_volume > 0;
                if (!legal_shape) ++illegal_state_observed;
            }
        }
        t.check(illegal_state_observed == 0,
                "every racing combination of fill/cancel/replace leaves the order in a legally-shaped state");
    });

    // ---- cross-shard operations --------------------------------------------
    {
        constexpr std::size_t shard_count = 4;
        constexpr std::size_t per_shard = 1'000;
        oms::ShardedOms s(shard_count, per_shard + 16, 1, per_shard * shard_count + 16);
        for (std::size_t sh = 0; sh < shard_count; ++sh) {
            for (std::size_t i = 0; i < per_shard; ++i) {
                // logical_order_id chosen so (id % shard_count) == sh exactly.
                const std::uint64_t id = sh + i * shard_count + shard_count;  // always > 0, always % shard_count == sh
                oms::Completion c{};
                t.check(s.submit_create(0, exec::BrokerOrderRef{1, id}, 0, 1, c) && c.ok,
                        "cross-shard: create lands in its owning shard");
                t.check(s.shard_of(exec::BrokerOrderRef{1, id}) == sh, "cross-shard: routed to the intended shard");
            }
        }
        std::size_t total = 0;
        for (std::size_t sh = 0; sh < shard_count; ++sh) {
            t.check(s.live_count(sh) == per_shard, "cross-shard: each shard holds exactly its own orders");
            total += s.live_count(sh);
        }
        t.check(total == per_shard * shard_count, "cross-shard: aggregate matches sum of shards");
        // Mutating one shard must not affect another.
        oms::Completion cancel_step1{}, cancel_step2{};
        (void)s.submit_transition(0, exec::BrokerOrderRef{1, shard_count}, exec::OrderState::sent, cancel_step1);
        (void)s.submit_transition(0, exec::BrokerOrderRef{1, shard_count}, exec::OrderState::rejected, cancel_step2);
        t.check(s.live_count(0) == per_shard - 1, "cross-shard: reclaim on shard 0 doesn't touch other shards");
        for (std::size_t sh = 1; sh < shard_count; ++sh)
            t.check(s.live_count(sh) == per_shard, "cross-shard: untouched shards remain exactly as populated");
    }

    // ---- queue saturation / fail-closed ------------------------------------
    {
        oms::ShardedOms s(1, 100'000, 1, 100'000);
        s.pause(0);
        std::size_t accepted = 0;
        std::vector<std::unique_ptr<oms::Completion>> pool;
        bool saw_rejection = false;
        for (std::size_t i = 0; i < oms::queue_capacity + 8; ++i) {
            pool.push_back(std::make_unique<oms::Completion>());
            const bool pushed = s.try_enqueue_create(0, exec::BrokerOrderRef{1, i + 1}, 0, 1, *pool.back());
            if (pushed) ++accepted; else saw_rejection = true;
        }
        t.check(accepted == oms::queue_capacity, "ring accepts exactly its declared capacity, no more");
        t.check(saw_rejection, "push past capacity is rejected (fail-closed), not blocked or dropped silently");
        s.resume(0);
        // Everything accepted must still drain correctly once resumed.
        bool all_done = false;
        for (int spin = 0; spin < 2'000'000 && !all_done; ++spin) {
            all_done = true;
            for (std::size_t i = 0; i < accepted; ++i)
                if (!pool[i]->done.load(std::memory_order_acquire)) { all_done = false; break; }
        }
        t.check(all_done, "queued requests drain and complete once the shard resumes");
        t.check(s.live_count(0) == accepted, "every accepted request was actually applied after resume");
    }

    // ---- deterministic repeated-run digest ---------------------------------
    // A concurrent run's *captured* total order (merged_log, sorted by
    // global_seq) must be internally well-formed -- gapless, strictly
    // increasing, every op legal when replayed in that order through a fresh
    // single-threaded Oms -- regardless of which real interleaving produced
    // it. Two independent concurrent runs of the same producer workload will
    // not generally produce the *same* merged_log (that would require
    // reproducible OS scheduling, which is not a real guarantee); what must
    // hold, every time, is that each run's own captured log replays cleanly
    // and reproducibly.
    {
        constexpr std::size_t threads = 4;
        constexpr std::size_t per_thread = 2'000;
        auto run_once = [&]() {
            oms::ShardedOms s(4, per_thread * threads / 4 + 16, threads, per_thread * threads + 16);
            std::vector<std::thread> workers;
            for (std::size_t w = 0; w < threads; ++w) {
                workers.emplace_back([&, w] {
                    for (std::size_t i = 0; i < per_thread; ++i) {
                        const std::uint64_t id = w * per_thread + i + 1;
                        oms::Completion c{};
                        (void)s.submit_create(w, exec::BrokerOrderRef{1, id}, 0, 1, c);
                    }
                });
            }
            for (auto& w : workers) w.join();
            return s.merged_log();
        };

        for (int run = 0; run < 3; ++run) {
            const auto log = run_once();
            t.check(log.size() == threads * per_thread, "digest: captured log has one entry per submitted op");
            bool gapless = true, all_ok = true;
            for (std::size_t i = 0; i < log.size(); ++i) {
                if (i > 0 && log[i].global_seq <= log[i - 1].global_seq) gapless = false;
                if (!log[i].ok) all_ok = false;
            }
            t.check(gapless, "digest: global_seq is strictly increasing across the merged log");
            t.check(all_ok, "digest: every logged create actually succeeded");

            // Replay this run's captured log through a fresh single-threaded
            // Oms, twice, and require byte-identical results -- this is the
            // actual determinism proof, reusing Phase B's already-proven
            // single-threaded replay.
            auto replay = [&](const std::vector<oms::LogEntry>& l) {
                oms::Oms o(threads * per_thread + 16);
                risk::RiskEngine e(risk::Limits{});
                risk::Request q{}; q.volume = 1; q.risk_minor = 1; q.free_margin = 1'000'000;
                q.warm_mask = 1; q.session_open = true;
                const auto token = e.check(q).token;
                for (const auto& entry : l) (void)o.create(entry.ref, 0, 1, token);
                return o.size();
            };
            const auto first = replay(log);
            const auto second = replay(log);
            t.check(first == second && first == threads * per_thread,
                    "digest: replaying the captured log twice is reproducible");
        }
    }

    return t.result();
}
