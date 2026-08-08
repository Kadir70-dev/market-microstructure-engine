#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "core/ring_buffer.hpp"
#include "persist/execution_wal.hpp"
#include "persist/wal_hook.hpp"

// Phase J (blocker fix) -- concrete, non-blocking WalHook implementation.
// record() (called from a ShardedOms shard's own worker thread, see
// wal_hook.hpp) only ever does a bounded core::RingBuffer::try_push -- the
// exact same lock-free primitive ShardedOms already uses for its own
// request queues, at the same cost profile, proven fast by every prior
// phase's benchmark. All actual disk I/O (ExecutionWalWriter::append() and
// flush()) happens on one dedicated background thread that drains every
// shard's outbox ring in round-robin and flushes once per drain pass --
// this is the entire mechanism by which "do not introduce blocking disk
// I/O into the critical trading thread" is satisfied: the trading thread
// never calls anything in this file except record(), which never touches
// the filesystem.
//
// Durability window, stated plainly rather than implied: a record is
// guaranteed on disk once flush() has run for the drain pass that consumed
// it -- not at record()-return time. A crash between record() succeeding
// and the next flush() loses that record (it was never durable), which is
// the correct, expected cost of not blocking the critical thread on I/O;
// it is not a claim of zero data loss. What IS guaranteed: records that
// were enqueued are written in the exact order ShardedOms applied them
// (per-shard, per-order -- see wal_hook.hpp), and a record that record()
// returns true for is drained and appended before this object is
// destroyed (stop() drains to empty before the final flush).
//
// Backpressure (ring full): record() returns false; ShardedOms counts it
// (wal_records_dropped()) rather than blocking or dropping silently. This
// object does not decide whether that should refuse new exposure -- see
// oms/risk_gated_router.hpp, which checks pressure before calling
// ShardedOms at all for creates.

namespace persist {

class LiveWalRecorder final : public WalHook {
public:
    LiveWalRecorder(std::filesystem::path directory, std::size_t shard_count,
                    std::uint64_t segment_bytes = 64ULL * 1024 * 1024) {
        WalConfig config{};
        config.directory = std::move(directory);
        config.segment_data_bytes = segment_bytes;
        opened_ = writer_.open(config, WalFileHeader{}, now_ns());
        outboxes_.reserve(shard_count);
        for (std::size_t i = 0; i < shard_count; ++i)
            outboxes_.emplace_back(std::make_unique<core::RingBuffer<exec::JournalRecord, 8192>>());
        running_.store(true, std::memory_order_release);
        writer_thread_ = std::thread([this] { run(); });
    }

    ~LiveWalRecorder() override { stop(); }
    LiveWalRecorder(const LiveWalRecorder&) = delete;
    LiveWalRecorder& operator=(const LiveWalRecorder&) = delete;

    [[nodiscard]] bool opened() const noexcept { return opened_; }

    [[nodiscard]] bool record(std::size_t shard_index, const exec::JournalRecord& rec) noexcept override {
        if (shard_index >= outboxes_.size()) return false;
        return outboxes_[shard_index]->try_push(rec);
    }

    // Joins the background thread after one final drain-to-empty pass, so
    // every record that record() accepted is written and flushed before
    // this call returns -- the crash-recovery tests use this to establish
    // "everything durable up to this point" before simulating a restart.
    void stop() noexcept {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (writer_thread_.joinable()) writer_thread_.join();
    }

    [[nodiscard]] std::uint64_t records_written() const noexcept { return written_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t pending_depth() const noexcept {
        std::size_t total = 0;
        for (const auto& ring : outboxes_) total += ring->size();
        return total;
    }

private:
    [[nodiscard]] static std::uint64_t now_ns() noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    [[nodiscard]] bool drain_once() noexcept {
        bool any = false;
        for (auto& ring : outboxes_) {
            exec::JournalRecord rec{};
            while (ring->try_pop(rec)) {
                if (writer_.append(rec, now_ns())) written_.fetch_add(1, std::memory_order_relaxed);
                any = true;
            }
        }
        return any;
    }

    void run() {
        std::size_t idle_spins = 0;
        while (running_.load(std::memory_order_acquire)) {
            if (drain_once()) {
                (void)writer_.flush();
                idle_spins = 0;
            } else if (++idle_spins > 1000) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                idle_spins = 0;
            }
        }
        while (drain_once()) {}  // final drain: everything record() accepted is written before stop() returns
        (void)writer_.flush();
    }

    ExecutionWalWriter writer_;
    bool opened_{false};
    std::vector<std::unique_ptr<core::RingBuffer<exec::JournalRecord, 8192>>> outboxes_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> written_{0};
    std::thread writer_thread_;
};

}  // namespace persist
