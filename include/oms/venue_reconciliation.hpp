#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "exec/paper_broker.hpp"
#include "oms/oms.hpp"
#include "oms/report_sequencer.hpp"

// Phase E — explicit execution-venue reconciliation interface.
//
// VenueAdapter is the seam a real venue integration would implement later;
// this phase ships exactly one concrete adapter, backed by PaperBroker, per
// "Use PaperBroker/test venue adapters only. Do not connect to a real venue."
// All methods write into a caller-owned, bounded buffer and return the count
// written -- no allocation here, so reconciliation at 1M orders costs one
// caller-side buffer, not a hot-path allocation.
//
// OrderReconciler does the per-order set comparison (local-only, venue-only,
// state/volume mismatch) that include/oms/reconciler.hpp's existing
// ReconcileView/reconcile_break() deliberately does not: that function is an
// aggregate decision ("is there a break, yes/no, and what kind") over counts
// the caller already computed. This is the thing that has to *produce* those
// counts, and the per-order detail besides. reconciler.hpp is unchanged and
// still reused for the aggregate roll-up in RecoveryWorkflow.

namespace oms {

struct OrderSnapshot final {
    exec::BrokerOrderRef ref{};
    exec::OrderState state{exec::OrderState::new_order};
    std::int64_t requested_volume{0};
    std::int64_t filled_volume{0};
};

struct PositionSnapshot final {
    std::uint64_t position_ticket{0};
    std::uint32_t symbol_id{0};
    exec::Side side{exec::Side::buy};
    std::int64_t volume{0};
};

class VenueAdapter {
public:
    virtual ~VenueAdapter() = default;
    [[nodiscard]] virtual std::size_t fetch_open_orders(OrderSnapshot* out,
                                                        std::size_t capacity) const noexcept = 0;
    [[nodiscard]] virtual std::size_t fetch_positions(PositionSnapshot* out,
                                                      std::size_t capacity) const noexcept = 0;
    // Reports strictly after `since_venue_seq` for each order, i.e. what a
    // real venue's "give me everything I missed" replay would return on
    // reconnect. Order within the returned set is not guaranteed to be
    // globally sorted -- ReportSequencer (Phase D) is what makes applying
    // them order-independent and exactly-once.
    [[nodiscard]] virtual std::size_t fetch_recent_reports(ExecReport* out,
                                                           std::size_t capacity) const noexcept = 0;
    [[nodiscard]] virtual exec::AccountState fetch_account() const noexcept = 0;
};

// Backed by a live PaperBroker. fetch_recent_reports derives ExecReports from
// PaperBroker's own exec::Journal (already-tested, Phase A/B infrastructure):
// each order's Nth relevant journal record (ack/fill/cancel/terminal
// order_state) becomes venue_seq N for that order -- PaperBroker has no
// native venue_seq concept (it is a simulator, not a real venue protocol),
// so occurrence-order-per-ref is the minimum-correct, honestly-derived
// substitute, consistent with Phase D's own sequencing model.
class PaperBrokerVenueAdapter final : public VenueAdapter {
public:
    explicit PaperBrokerVenueAdapter(const exec::PaperBroker& broker) noexcept : broker_(broker) {}

    [[nodiscard]] std::size_t fetch_open_orders(OrderSnapshot* out, std::size_t capacity) const noexcept override {
        std::size_t n = 0;
        // PaperBroker exposes live orders only through find_order(ref) and no
        // enumerator, so this walks its journal for distinct refs that are
        // still live per the last order_state/fill seen for them -- bounded
        // by the journal's own fixed capacity (exec::max_journal_records),
        // never unbounded.
        const auto& journal = broker_.journal();
        for (std::size_t i = 0; i < journal.size() && n < capacity; ++i) {
            const auto& rec = journal.at(i);
            if (rec.type != static_cast<std::uint16_t>(exec::JournalRecordType::command)) continue;
            const exec::BrokerOrderRef ref{rec.run_id, rec.logical_order_id};
            bool already = false;
            for (std::size_t k = 0; k < n; ++k) if (out[k].ref == ref) { already = true; break; }
            if (already) continue;
            const auto* order = broker_.find_order(ref);
            if (order == nullptr || exec::is_terminal(order->state)) continue;
            out[n++] = OrderSnapshot{ref, order->state, order->requested_volume, order->filled_volume};
        }
        return n;
    }

    [[nodiscard]] std::size_t fetch_positions(PositionSnapshot* out, std::size_t capacity) const noexcept override {
        const auto& positions = broker_.positions();
        std::size_t n = 0;
        for (std::size_t i = 0; i < positions.size() && n < capacity; ++i) {
            const auto& p = positions.at(i);
            if (p.state == exec::PositionState::closed) continue;
            out[n++] = PositionSnapshot{p.position_ticket, p.symbol_id, p.side, p.volume};
        }
        return n;
    }

    [[nodiscard]] std::size_t fetch_recent_reports(ExecReport* out, std::size_t capacity) const noexcept override {
        const auto& journal = broker_.journal();
        std::size_t n = 0;
        // occurrence[i] tracks the running per-ref sequence as the journal is
        // scanned in emission order -- O(seen refs) per record, bounded by
        // the journal's fixed capacity, not unbounded.
        struct Seen final { exec::BrokerOrderRef ref; std::uint64_t count; };
        static thread_local std::array<Seen, exec::max_journal_records> seen{};
        std::size_t seen_n = 0;

        const auto bump = [&](exec::BrokerOrderRef ref) -> std::uint64_t {
            for (std::size_t k = 0; k < seen_n; ++k)
                if (seen[k].ref == ref) return ++seen[k].count;
            if (seen_n < seen.size()) { seen[seen_n] = {ref, 1}; ++seen_n; return 1; }
            return 0;  // unreachable: seen_n bounded by journal.size() <= capacity
        };

        for (std::size_t i = 0; i < journal.size() && n < capacity; ++i) {
            const auto& rec = journal.at(i);
            const auto kind_type = static_cast<exec::JournalRecordType>(rec.type);
            ReportKind kind;
            std::int64_t volume = 0, price = 0;
            switch (kind_type) {
                case exec::JournalRecordType::acknowledgement: kind = ReportKind::ack; break;
                case exec::JournalRecordType::fill: kind = ReportKind::fill; volume = rec.b; break;
                case exec::JournalRecordType::cancel: kind = ReportKind::cancel_pending; break;
                case exec::JournalRecordType::order_state:
                    if (static_cast<exec::OrderState>(rec.b) == exec::OrderState::cancelled)
                        kind = ReportKind::cancelled;
                    else continue;
                    break;
                case exec::JournalRecordType::replace: kind = ReportKind::replace_ack; price = rec.b; volume = rec.d; break;
                default: continue;
            }
            const exec::BrokerOrderRef ref{rec.run_id, rec.logical_order_id};
            const auto seq = bump(ref);
            out[n++] = ExecReport{ref, seq, kind, volume, price};
        }
        return n;
    }

    [[nodiscard]] exec::AccountState fetch_account() const noexcept override { return broker_.account(); }

private:
    const exec::PaperBroker& broker_;
};

enum class OrderReconcileStatus : std::uint8_t { matched, local_only, venue_only, state_mismatch, volume_mismatch };

struct OrderReconcileResult final {
    exec::BrokerOrderRef ref{};
    OrderReconcileStatus status{OrderReconcileStatus::matched};
};

class OrderReconciler final {
public:
    // Sort-merge comparison, O((local+venue) log(local+venue)); reconciliation
    // is explicitly not a hot path (Part -- "reconciliation time" is a
    // benchmarked, periodic operation, not a per-submit cost), so this is the
    // correctness-first choice over a hand-rolled hash index. Writes into
    // out[0..return value) which must be large enough for local_count +
    // venue_count in the worst case (no overlap).
    [[nodiscard]] std::size_t compare(const OrderSnapshot* local, std::size_t local_count,
                                      const OrderSnapshot* venue, std::size_t venue_count,
                                      OrderReconcileResult* out, std::size_t out_capacity) const {
        std::vector<const OrderSnapshot*> local_sorted(local_count), venue_sorted(venue_count);
        for (std::size_t i = 0; i < local_count; ++i) local_sorted[i] = &local[i];
        for (std::size_t i = 0; i < venue_count; ++i) venue_sorted[i] = &venue[i];
        const auto by_ref = [](const OrderSnapshot* a, const OrderSnapshot* b) {
            if (a->ref.run_id != b->ref.run_id) return a->ref.run_id < b->ref.run_id;
            return a->ref.logical_order_id < b->ref.logical_order_id;
        };
        std::sort(local_sorted.begin(), local_sorted.end(), by_ref);
        std::sort(venue_sorted.begin(), venue_sorted.end(), by_ref);

        const auto emit = [&](std::size_t& n, exec::BrokerOrderRef ref, OrderReconcileStatus status) {
            out[n].ref = ref;
            out[n].status = status;
            ++n;
        };

        std::size_t li = 0, vi = 0, n = 0;
        while (li < local_count && vi < venue_count && n < out_capacity) {
            const auto& l = *local_sorted[li];
            const auto& v = *venue_sorted[vi];
            if (l.ref.run_id == v.ref.run_id && l.ref.logical_order_id == v.ref.logical_order_id) {
                if (l.state != v.state)
                    emit(n, l.ref, OrderReconcileStatus::state_mismatch);
                else if (l.filled_volume != v.filled_volume || l.requested_volume != v.requested_volume)
                    emit(n, l.ref, OrderReconcileStatus::volume_mismatch);
                else
                    emit(n, l.ref, OrderReconcileStatus::matched);
                ++li; ++vi;
            } else if (by_ref(&l, &v)) {
                emit(n, l.ref, OrderReconcileStatus::local_only);
                ++li;
            } else {
                emit(n, v.ref, OrderReconcileStatus::venue_only);
                ++vi;
            }
        }
        while (li < local_count && n < out_capacity) emit(n, local_sorted[li++]->ref, OrderReconcileStatus::local_only);
        while (vi < venue_count && n < out_capacity) emit(n, venue_sorted[vi++]->ref, OrderReconcileStatus::venue_only);
        return n;
    }
};

// Position reconciliation: Oms tracks orders, not a position ledger (that is
// Pms/Portfolio's job, not touched in this phase), so "local" here is a
// simple per-symbol net-volume aggregate derived directly from local order
// fill data -- a minimum-correct proxy sufficient to *detect* a mismatch
// against the venue's actual position, not a full independent ledger.
// Bounded/preallocated: writes into a caller-sized array, one entry per
// distinct symbol_id seen (at most exec::max_symbols).
struct PositionMismatch final {
    std::uint32_t symbol_id{0};
    std::int64_t local_net_volume{0};
    std::int64_t venue_net_volume{0};
};

[[nodiscard]] inline std::size_t compare_positions(const PositionSnapshot* venue, std::size_t venue_count,
                                                    const std::int64_t* local_net_by_symbol,
                                                    std::size_t local_symbol_count, PositionMismatch* out,
                                                    std::size_t out_capacity) noexcept {
    std::array<std::int64_t, 64> venue_net{};
    for (std::size_t i = 0; i < venue_count && venue[i].symbol_id < venue_net.size(); ++i)
        venue_net[venue[i].symbol_id] += (venue[i].side == exec::Side::buy ? venue[i].volume : -venue[i].volume);

    std::size_t n = 0;
    for (std::uint32_t s = 0; s < local_symbol_count && s < venue_net.size() && n < out_capacity; ++s) {
        if (local_net_by_symbol[s] != venue_net[s]) {
            out[n].symbol_id = s;
            out[n].local_net_volume = local_net_by_symbol[s];
            out[n].venue_net_volume = venue_net[s];
            ++n;
        }
    }
    return n;
}

}  // namespace oms
