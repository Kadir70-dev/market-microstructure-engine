#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>

// Phase 5 — deterministic execution journal.
//
// Architecture Part 11.1 gives the journal the same framing as the WAL and
// requires "every order/position transition, fill, risk decision, keyed by
// corr_id". Phase 5 needs the record layer; the on-disk writer is Phase 6, so
// records are held in a bounded fixed buffer and committed to a rolling digest.
//
// Fixed layout, all integer, no padding surprises: the determinism gate compares
// the journal byte-for-byte, so any field whose representation could vary
// between runs (a double, an uninitialised pad byte, a pointer) would make the
// comparison meaningless. The digest is taken over the raw record bytes rather
// than over interpreted fields, so it detects exactly what a byte comparison
// would.

namespace exec {

enum class JournalRecordType : std::uint16_t {
    command = 1,
    acknowledgement = 2,
    rejection = 3,
    fill = 4,
    cancel = 5,
    replace = 6,
    order_state = 7,
    position_update = 8,
    pnl_update = 9,
    account_update = 10
};

#pragma pack(push, 1)
struct JournalRecord final {
    std::uint64_t seq{0};                 // journal sequence, dense and gapless
    std::uint64_t ts_ns{0};               // virtual time, never wall clock
    std::uint16_t type{0};
    std::uint16_t reserved{0};
    std::uint32_t symbol_id{0};
    std::uint64_t run_id{0};
    std::uint64_t logical_order_id{0};
    std::uint64_t position_ticket{0};
    std::int64_t a{0};                    // type-specific integer fields
    std::int64_t b{0};
    std::int64_t c{0};
    std::int64_t d{0};
};
#pragma pack(pop)

static_assert(sizeof(JournalRecord) == 80);
static_assert(std::is_trivially_copyable_v<JournalRecord>);
static_assert(std::is_standard_layout_v<JournalRecord>);

// 16k records x 80 B = 1.3 MiB, held by value inside PaperBroker so the hot
// path never allocates. Kept modest deliberately: a broker is routinely stack
// constructed in tests, and a multi-megabyte journal would overflow the stack
// before any assertion ran. Record volume scales with order activity, not with
// event count, so this bounds a long replay comfortably.
inline constexpr std::size_t max_journal_records = 1u << 14;

class Journal final {
public:
    // Returns false when full. Fail-closed: a journal that silently wraps would
    // make the determinism comparison depend on how much history happened to
    // still be resident, which is not a comparison at all.
    [[nodiscard]] bool append(JournalRecord record) noexcept {
        if (count_ >= max_journal_records) { overflowed_ = true; return false; }
        record.seq = count_;
        records_[count_++] = record;
        mix_bytes(record);
        return true;
    }

    [[nodiscard]] bool emit(JournalRecordType type, std::uint64_t ts_ns, std::uint64_t run_id,
                            std::uint64_t logical_order_id, std::uint32_t symbol_id,
                            std::uint64_t position_ticket, std::int64_t a = 0,
                            std::int64_t b = 0, std::int64_t c = 0, std::int64_t d = 0) noexcept {
        JournalRecord record{};
        record.ts_ns = ts_ns;
        record.type = static_cast<std::uint16_t>(type);
        record.symbol_id = symbol_id;
        record.run_id = run_id;
        record.logical_order_id = logical_order_id;
        record.position_ticket = position_ticket;
        record.a = a;
        record.b = b;
        record.c = c;
        record.d = d;
        return append(record);
    }

    [[nodiscard]] std::uint64_t digest() const noexcept { return digest_; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
    [[nodiscard]] const JournalRecord& at(std::size_t index) const noexcept {
        return records_[index];
    }

    // Byte-for-byte equality of the whole journal, which is what the Phase 5
    // gate actually requires. The digest is the fast path; this is the proof.
    [[nodiscard]] bool identical_to(const Journal& other) const noexcept {
        if (count_ != other.count_ || digest_ != other.digest_) return false;
        return std::memcmp(records_.data(), other.records_.data(),
                           count_ * sizeof(JournalRecord)) == 0;
    }

    void clear() noexcept {
        count_ = 0;
        digest_ = 1469598103934665603ULL;
        overflowed_ = false;
    }

private:
    void mix_bytes(const JournalRecord& record) noexcept {
        std::array<std::byte, sizeof(JournalRecord)> raw{};
        std::memcpy(raw.data(), &record, sizeof(record));
        for (const auto byte : raw) {
            digest_ ^= static_cast<std::uint64_t>(byte);
            digest_ *= 1099511628211ULL;
        }
    }

    std::array<JournalRecord, max_journal_records> records_{};
    std::size_t count_{0};
    std::uint64_t digest_{1469598103934665603ULL};
    bool overflowed_{false};
};

}  // namespace exec
