#pragma once
#include <array>
#include <cstring>
#include <filesystem>
#include <memory>

#include "exec/journal.hpp"
#include "persist/segment_catalog.hpp"
#include "persist/wal_reader.hpp"
#include "persist/wal_writer.hpp"

// Phase E — durable persistence for exec::JournalRecord (order/execution
// events), reusing persist::WalWriter/WalReader/SegmentCatalog exactly as
// they already exist. Deliberately does *not* route through
// core::FixedEvent/EventType: that schema is defined (order_ack, fill,
// partial_fill, cancel_ack, position_update, account_update...) but has zero
// call sites anywhere in this codebase, and its 72-byte payload slot cannot
// hold an 80-byte JournalRecord without a lossy re-encoding that several
// JournalRecordType values (command, order_state, replace, pnl_update) have
// no existing EventType equivalent for at all. Writing JournalRecord frames
// directly through WalWriter::append() -- which is already generic over
// (type, payload, length), not FixedEvent-specific -- is the smaller,
// non-invasive change; WalReader::next_raw() (new, additive, next() itself
// untouched) is its read-side counterpart. Framing, CRC, segment rotation,
// boundary metadata and corruption handling are 100% reused, unmodified.

namespace persist {

class ExecutionWalWriter final {
public:
    [[nodiscard]] bool open(const WalConfig& config, const WalFileHeader& header,
                            std::uint64_t monotonic_now_ns) noexcept {
        return writer_.open(config, header, monotonic_now_ns);
    }

    // Fails closed (returns false) rather than silently dropping a record:
    // rotation failure or a write error must be visible to the caller, which
    // owns the decision of what "the WAL is unavailable" means for trading
    // (Part D of this phase: fail closed for new exposure).
    [[nodiscard]] bool append(const exec::JournalRecord& record, std::uint64_t monotonic_now_ns) noexcept {
        if (writer_.needs_rotation(monotonic_now_ns, sizeof(record))) {
            std::filesystem::path closed;
            if (!writer_.rotate(monotonic_now_ns, closed)) return false;
        }
        const auto result = writer_.append(record.type, 0, &record, sizeof(record));
        return result == AppendResult::committed;
    }

    [[nodiscard]] bool flush() noexcept { return writer_.flush(); }
    [[nodiscard]] bool finalize(std::filesystem::path& closed_segment) noexcept {
        return writer_.finalize(closed_segment);
    }
    [[nodiscard]] const std::filesystem::path& current_path() const noexcept {
        return writer_.current_path();
    }
    void close() noexcept { writer_.close(); }

private:
    WalWriter writer_;
};

// One-segment reader. Mirrors WalReader's own contract: sequential, strictly
// read-only, stops at the first malformed frame.
class ExecutionWalReader final {
public:
    [[nodiscard]] bool open(const std::filesystem::path& segment) noexcept { return reader_.open(segment); }

    [[nodiscard]] ReadStatus next(exec::JournalRecord& out) noexcept {
        std::uint32_t length = 0;
        const auto status = reader_.next_raw(reinterpret_cast<std::byte*>(&out), sizeof(out), length);
        if (status != ReadStatus::ok) return status;
        if (length != sizeof(out)) return ReadStatus::unexpected_payload_size;
        return ReadStatus::ok;
    }

    [[nodiscard]] const ReadStats& stats() const noexcept { return reader_.stats(); }
    void close() noexcept { reader_.close(); }

private:
    WalReader reader_;
};

// Multi-segment tailer for JournalRecord, following the same cursor/resume
// design as replay::WalTailer (include/replay/wal_tailer.hpp) but for an
// 80-byte record instead of a 128-byte FixedEvent -- kept as a separate,
// smaller class rather than templatizing WalTailer, which is a working,
// tested component this phase does not need to touch.
enum class ExecTailStatus : std::uint8_t { ok, idle, end_of_data, corrupt, catalog_error };

struct ExecTailCursor final {
    std::uint64_t segment_index{0};
    std::uint64_t records_consumed{0};
};

class ExecutionWalTailer final {
public:
    [[nodiscard]] bool open(const std::filesystem::path& directory) noexcept {
        directory_ = directory;
        cursor_ = ExecTailCursor{};
        corrupt_ = false;
        buffered_ = 0;
        buffer_position_ = 0;
        return rescan();
    }

    [[nodiscard]] bool resume(const ExecTailCursor& cursor) noexcept {
        cursor_ = cursor;
        buffered_ = 0;
        buffer_position_ = 0;
        return true;
    }

    // A WalReader's committed boundary is fixed at open() time (Part --
    // resolve_boundary reads the .meta sidecar once, on open), so a reader
    // that stays open across calls can never learn about data the writer
    // commits afterward -- this is exactly why a *growing* segment must be
    // reopened to observe new records at all, which is what replay::WalTailer
    // already does. Where this class *can* still avoid the naive
    // reopen-every-call cost (a real problem: caught taking a benchmark from
    // low-microseconds-per-record to never finishing, the same class of
    // issue as Phase D's GapPool::drain() fix) is by batching: reopen once,
    // drain every currently-committed record from that reader into a bounded
    // buffer, and serve calls from the buffer until it's exhausted before
    // reopening again. Reopen frequency then scales with how often new data
    // actually arrives, not with how many records are read.
    [[nodiscard]] ExecTailStatus next(exec::JournalRecord& out) noexcept {
        if (corrupt_) return ExecTailStatus::corrupt;

        if (buffer_position_ < buffered_) {
            out = buffer_[buffer_position_++];
            ++cursor_.records_consumed;
            return ExecTailStatus::ok;
        }

        if (!rescan()) return ExecTailStatus::catalog_error;
        if (catalog_.empty()) return ExecTailStatus::catalog_error;
        if (cursor_.segment_index >= catalog_.segments().size()) return ExecTailStatus::end_of_data;

        const auto status = fill_from_current_segment();
        if (status == ExecTailStatus::corrupt) { corrupt_ = true; return ExecTailStatus::corrupt; }
        if (status == ExecTailStatus::ok) {
            out = buffer_[buffer_position_++];
            ++cursor_.records_consumed;
            return ExecTailStatus::ok;
        }

        // This segment yielded nothing new. Advance only if a later segment
        // already exists -- that is what proves this one is sealed.
        if (cursor_.segment_index + 1 < catalog_.segments().size()) {
            ++cursor_.segment_index;
            cursor_.records_consumed = 0;
            return next(out);
        }
        return ExecTailStatus::idle;
    }

    [[nodiscard]] const ExecTailCursor& cursor() const noexcept { return cursor_; }
    [[nodiscard]] bool corrupt() const noexcept { return corrupt_; }

private:
    [[nodiscard]] bool rescan() noexcept { return catalog_.scan(directory_); }

    // Opens a fresh reader on the current segment (so its committed boundary
    // reflects whatever the writer has flushed as of *now*), skips records
    // already delivered, and buffers everything newly available up to
    // buffer_capacity. One filesystem open per call to this, not per record.
    [[nodiscard]] ExecTailStatus fill_from_current_segment() noexcept {
        const auto& descriptor = catalog_.segments()[cursor_.segment_index];
        if (descriptor.encoding != SegmentEncoding::raw) return ExecTailStatus::corrupt;

        ExecutionWalReader reader;
        if (!reader.open(descriptor.path)) return ExecTailStatus::idle;

        buffered_ = 0;
        buffer_position_ = 0;
        std::uint64_t position = 0;
        for (;;) {
            exec::JournalRecord record{};
            const auto status = reader.next(record);
            if (status == ReadStatus::ok) {
                const auto index = position++;
                if (index < cursor_.records_consumed) continue;  // already delivered
                buffer_[buffered_++] = record;
                if (buffered_ >= buffer_capacity) break;
                continue;
            }
            if (is_clean_stop(status)) break;
            return ExecTailStatus::corrupt;
        }
        return buffered_ > 0 ? ExecTailStatus::ok : ExecTailStatus::idle;
    }

    static constexpr std::size_t buffer_capacity = 4096;

    std::filesystem::path directory_{};
    SegmentCatalog catalog_{};
    ExecTailCursor cursor_{};
    std::array<exec::JournalRecord, buffer_capacity> buffer_{};
    std::size_t buffered_{0};
    std::size_t buffer_position_{0};
    bool corrupt_{false};
};

}  // namespace persist
