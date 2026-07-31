#pragma once

#include <array>
#include <cstdint>

#include "exec/account.hpp"
#include "exec/exec_types.hpp"
#include "exec/journal.hpp"
#include "exec/models.hpp"
#include "exec/philox_rng.hpp"

// Phase 5 — Paper Execution Simulator.
//
// Default execution mode in every build (Part 9). Contains no broker API, no
// socket, no MT5 symbol, and no order transport of any kind: it is a pure
// function of the replayed event stream plus the run seed.
//
// ⚠ NOT_CALIBRATED — latency, slippage and rejection magnitudes come from
//   declared placeholders (see models.hpp). Mechanics are correct; magnitudes
//   are not evidence.
// ⚠ NOT_VALIDATED — on L1_ONLY / DOM_* sources there is no real queue to stand
//   in, so every queue-dependent result is tagged and is inadmissible for
//   promotion (Part 9.2).
//
// Fixed-capacity throughout. The hot path allocates nothing; exceeding capacity
// is a fail-closed rejection rather than a growth event.

namespace exec {

inline constexpr std::size_t max_live_orders = 256;

struct BrokerConfig final {
    std::uint64_t run_id{1};
    std::uint64_t run_seed{1};
    SimulationMode mode{SimulationMode::pessimistic};
    bool hedging{true};                       // Exness retail default (Part 8.1)
    std::int64_t initial_balance_minor{0};
    std::int64_t stop_out_level_bp{5'000};    // 50% margin level
    std::uint64_t stale_quote_ns{1'000'000'000};
    bool queue_validated{false};              // true only on L2_EXCHANGE / L3_MBO
};

struct OrderRequest final {
    std::uint64_t corr_id{0};
    std::uint32_t strategy_id{0};
    std::uint32_t symbol_id{0};
    Side side{Side::buy};
    OrderType type{OrderType::market};
    std::int64_t volume{0};
    std::int64_t limit_price_ticks{0};
    std::uint64_t seq_global{0};
    std::uint64_t expire_ns{0};               // 0 = good-till-cancelled
    bool reduce_only{false};                  // reduce/close only, never open
};

struct SubmitResult final {
    bool accepted{false};
    BrokerOrderRef ref{};
    RejectReason reason{RejectReason::none};
};

struct MarketState final {
    std::int64_t bid_ticks{0};
    std::int64_t ask_ticks{0};
    std::int64_t bid_size{0};
    std::int64_t ask_size{0};
    std::uint64_t ts_ns{0};
    bool valid{false};

    [[nodiscard]] std::int64_t spread_ticks() const noexcept { return ask_ticks - bid_ticks; }
};

class PaperBroker final {
public:
    explicit PaperBroker(BrokerConfig config) noexcept : config_(config) {
        account_.balance_minor = config.initial_balance_minor;
        account_.equity_minor = config.initial_balance_minor;
        account_.free_margin_minor = config.initial_balance_minor;
        for (std::size_t i = 0; i < specs_.size(); ++i)
            specs_[i].symbol_id = static_cast<std::uint32_t>(i);
    }

    void set_symbol(const SymbolSpec& spec) noexcept {
        if (spec.symbol_id < specs_.size()) specs_[spec.symbol_id] = spec;
    }

    // ---- market data ------------------------------------------------------

    void on_quote(std::uint32_t symbol_id, std::int64_t bid, std::int64_t ask,
                  std::int64_t bid_size, std::int64_t ask_size, std::uint64_t ts_ns) noexcept {
        if (symbol_id >= market_.size()) return;
        auto& market = market_[symbol_id];

        // Queue credit: a reduction in displayed size at our resting level is
        // partly other participants' fills and partly cancellations. Part 9.2
        // allows crediting "an estimated share of depth reductions"; the share
        // is drawn from Philox so it is reproducible rather than arbitrary.
        credit_queue(symbol_id, market, bid, ask, bid_size, ask_size);

        market.bid_ticks = bid;
        market.ask_ticks = ask;
        market.bid_size = bid_size;
        market.ask_size = ask_size;
        market.ts_ns = ts_ns;
        market.valid = bid > 0 && ask > 0 && ask > bid;

        process(ts_ns);
        recompute_account(ts_ns);
    }

    // ---- order entry ------------------------------------------------------

    [[nodiscard]] SubmitResult submit(const OrderRequest& request, std::uint64_t now_ns) noexcept {
        SubmitResult result{};
        result.ref = BrokerOrderRef{config_.run_id,
            make_logical_order_id(request.strategy_id, request.symbol_id, request.seq_global)};

        const auto reject = [&](RejectReason reason) {
            result.accepted = false;
            result.reason = reason;
            (void)journal_.emit(JournalRecordType::rejection, now_ns, config_.run_id,
                                result.ref.logical_order_id, request.symbol_id, 0,
                                static_cast<std::int64_t>(reason), request.volume,
                                request.limit_price_ticks, 0);
            return result;
        };

        // Stop and stop-limit have no defined fill mechanics in Architecture
        // Version 1.0 (Part 9.2 covers market and limit only). Refusing is the
        // honest behaviour; inventing a trigger rule would put unspecified
        // semantics into a determinism gate.
        if (request.type != OrderType::market && request.type != OrderType::limit)
            return reject(RejectReason::impossible_fill);

        if (now_ns < last_submit_ns_) return reject(RejectReason::invalid_timestamp);
        if (request.symbol_id >= specs_.size()) return reject(RejectReason::unknown_symbol);

        const auto& spec = specs_[request.symbol_id];
        if (!spec.tradable) return reject(RejectReason::market_closed);
        if (request.volume <= 0) return reject(RejectReason::invalid_volume);
        if (request.volume < spec.volume_min) return reject(RejectReason::volume_below_minimum);
        if (request.volume > spec.volume_max) return reject(RejectReason::volume_above_maximum);
        if (spec.volume_step <= 0 || (request.volume % spec.volume_step) != 0)
            return reject(RejectReason::volume_not_step_aligned);

        if (request.type == OrderType::limit) {
            if (request.limit_price_ticks <= 0) return reject(RejectReason::invalid_price);
            if (!spec.price_aligned(request.limit_price_ticks))
                return reject(RejectReason::price_not_tick_aligned);
        }

        // reduce_only must reduce an existing opposing position, never open one.
        // Rejected at submit rather than clipped at fill so the refusal is
        // visible in the journal instead of silently rewriting the request.
        if (request.reduce_only) {
            const auto reducible = reducible_volume(request.symbol_id, request.side);
            if (reducible <= 0) return reject(RejectReason::reduce_only_no_position);
            if (request.volume > reducible) return reject(RejectReason::reduce_only_excess_volume);
        }

        if (has_seen(result.ref)) return reject(RejectReason::duplicate_order_id);

        std::size_t slot = max_live_orders;
        for (std::size_t i = 0; i < live_count_; ++i) {
            if (is_terminal(orders_[i].state)) { slot = i; break; }
        }
        if (slot == max_live_orders) {
            if (live_count_ >= max_live_orders) return reject(RejectReason::corrupted_state);
            slot = live_count_++;
        }

        const auto& market = market_[request.symbol_id];
        if (!market.valid) return reject(RejectReason::stale_quote);
        if (now_ns >= market.ts_ns && (now_ns - market.ts_ns) > config_.stale_quote_ns)
            return reject(RejectReason::stale_quote);

        const auto reference = (request.type == OrderType::limit)
            ? request.limit_price_ticks
            : (request.side == Side::buy ? market.ask_ticks : market.bid_ticks);
        const auto margin = MarginModel::required_minor(spec, request.volume, reference);
        if (margin > account_.free_margin_minor) return reject(RejectReason::insufficient_margin);

        auto& order = orders_[slot];
        order = Order{};
        order_ticket_[slot] = 0;
        order.ref = result.ref;
        order.corr_id = request.corr_id;
        order.symbol_id = request.symbol_id;
        order.side = request.side;
        order.type = request.type;
        order.requested_volume = request.volume;
        order.limit_price_ticks = request.limit_price_ticks;
        order.ts_submit_ns = now_ns;
        order.expire_ns = request.expire_ns;
        order.reduce_only = request.reduce_only;
        order.queue_validated = config_.queue_validated;

        // Latency is sampled as lambda and added to t_send, never to t_decision
        // (Part 9.2).
        order.ts_effective_ns = now_ns + LatencyModel::sample_ns(
            config_.run_seed, order.corr_id, config_.mode, regime_of(request.symbol_id));

        // Queue position is fixed at placement: queue_ahead = size at level.
        if (order.type == OrderType::limit)
            order.queue_ahead = queue_at_placement(order, market);

        order.state = OrderState::pending_send;
        reserved_margin_minor_ += margin;
        order.commission_minor = 0;
        transition(order, OrderState::sent, now_ns);

        (void)journal_.emit(JournalRecordType::command, now_ns, config_.run_id,
                            order.ref.logical_order_id, order.symbol_id, 0,
                            static_cast<std::int64_t>(order.side), order.requested_volume,
                            order.limit_price_ticks,
                            static_cast<std::int64_t>(order.type));

        remember(order.ref);
        last_submit_ns_ = now_ns;
        ++account_.open_orders;
        refresh_account(now_ns);
        result.accepted = true;
        return result;
    }

    [[nodiscard]] bool cancel(BrokerOrderRef ref, std::uint64_t now_ns) noexcept {
        auto* order = find_live(ref);
        if (order == nullptr || is_terminal(order->state)) return false;
        // UNKNOWN never cancels: it holds full reserved exposure until
        // reconciliation resolves it (Part 8.3).
        if (order->state == OrderState::unknown) return false;
        transition(*order, OrderState::cancel_pending, now_ns);
        (void)journal_.emit(JournalRecordType::cancel, now_ns, config_.run_id,
                            ref.logical_order_id, order->symbol_id, 0, order->remaining());
        finish(*order, OrderState::cancelled, now_ns);
        return true;
    }

    // Replace is cancel-and-replace on the same logical order: the ref is
    // preserved so journal linkage survives, and queue position is forfeited,
    // which is what a real venue does on an amend.
    [[nodiscard]] bool replace(BrokerOrderRef ref, std::int64_t new_price_ticks,
                               std::int64_t new_volume, std::uint64_t now_ns) noexcept {
        auto* order = find_live(ref);
        if (order == nullptr || is_terminal(order->state)) return false;
        if (order->state == OrderState::unknown) return false;
        const auto& spec = specs_[order->symbol_id];
        if (new_volume <= order->filled_volume || !spec.volume_valid(new_volume)) return false;
        if (order->type == OrderType::limit &&
            (new_price_ticks <= 0 || !spec.price_aligned(new_price_ticks))) return false;

        const auto old_price = order->limit_price_ticks;
        const auto old_volume = order->requested_volume;
        order->limit_price_ticks = new_price_ticks;
        order->requested_volume = new_volume;
        if (order->type == OrderType::limit)
            order->queue_ahead = queue_at_placement(*order, market_[order->symbol_id]);

        (void)journal_.emit(JournalRecordType::replace, now_ns, config_.run_id,
                            ref.logical_order_id, order->symbol_id, 0,
                            old_price, new_price_ticks, old_volume, new_volume);
        return true;
    }

    // ---- accessors --------------------------------------------------------

    [[nodiscard]] const AccountState& account() const noexcept { return account_; }
    [[nodiscard]] const PositionBook& positions() const noexcept { return positions_; }
    [[nodiscard]] const Journal& journal() const noexcept { return journal_; }
    [[nodiscard]] Journal& journal() noexcept { return journal_; }
    [[nodiscard]] std::uint64_t fills() const noexcept { return fill_count_; }
    [[nodiscard]] bool halted() const noexcept { return halted_; }
    [[nodiscard]] RejectReason halt_reason() const noexcept { return halt_reason_; }
    [[nodiscard]] bool queue_results_validated() const noexcept { return config_.queue_validated; }

    [[nodiscard]] const Order* find_order(BrokerOrderRef ref) const noexcept {
        for (std::size_t i = 0; i < live_count_; ++i)
            if (orders_[i].ref == ref) return &orders_[i];
        return nullptr;
    }

private:
    // ---- lifecycle --------------------------------------------------------

    void transition(Order& order, OrderState next, std::uint64_t now_ns) noexcept {
        const auto previous = order.state;
        order.state = next;
        (void)journal_.emit(JournalRecordType::order_state, now_ns, config_.run_id,
                            order.ref.logical_order_id, order.symbol_id, 0,
                            static_cast<std::int64_t>(previous),
                            static_cast<std::int64_t>(next), order.filled_volume,
                            order.remaining());
    }

    void finish(Order& order, OrderState terminal, std::uint64_t now_ns) noexcept {
        transition(order, terminal, now_ns);
        order.ts_terminal_ns = now_ns;
        release_reservation(order);
        if (account_.open_orders > 0) --account_.open_orders;
        refresh_account(now_ns);
    }

    void release_reservation(const Order& order) noexcept {
        const auto& spec = specs_[order.symbol_id];
        const auto reference = (order.type == OrderType::limit)
            ? order.limit_price_ticks
            : (order.avg_fill_price_ticks > 0 ? order.avg_fill_price_ticks
                                              : market_[order.symbol_id].ask_ticks);
        const auto margin = MarginModel::required_minor(spec, order.requested_volume, reference);
        reserved_margin_minor_ = (reserved_margin_minor_ > margin)
            ? reserved_margin_minor_ - margin : 0;
    }

    void process(std::uint64_t now_ns) noexcept {
        if (halted_) return;
        // Index order is submission order, which is deterministic and
        // independent of any container's internal layout.
        for (std::size_t i = 0; i < live_count_; ++i) {
            auto& order = orders_[i];
            if (is_terminal(order.state)) continue;

            if (order.expire_ns != 0 && now_ns >= order.expire_ns) {
                finish(order, OrderState::expired, now_ns);
                continue;
            }
            if (now_ns < order.ts_effective_ns) continue;   // still in flight

            if (order.state == OrderState::sent) {
                if (RejectionModel::rejects(config_.run_seed, order.corr_id, config_.mode,
                                            regime_of(order.symbol_id))) {
                    (void)journal_.emit(JournalRecordType::rejection, now_ns, config_.run_id,
                                        order.ref.logical_order_id, order.symbol_id, 0,
                                        static_cast<std::int64_t>(RejectReason::probabilistic_reject));
                    finish(order, OrderState::rejected, now_ns);
                    continue;
                }
                transition(order, OrderState::acknowledged, now_ns);
                (void)journal_.emit(JournalRecordType::acknowledgement, now_ns, config_.run_id,
                                    order.ref.logical_order_id, order.symbol_id, 0,
                                    static_cast<std::int64_t>(order.ts_effective_ns));
            }

            attempt_fill(order, now_ns);
        }
    }

    // ---- fills ------------------------------------------------------------

    void attempt_fill(Order& order, std::uint64_t now_ns) noexcept {
        const auto& market = market_[order.symbol_id];
        if (!market.valid || order.remaining() <= 0) return;
        if (order.state != OrderState::acknowledged &&
            order.state != OrderState::partially_filled) return;

        std::int64_t price = 0;
        std::int64_t available = 0;

        if (order.type == OrderType::market) {
            // Crosses the spread. The touch price is bid or ask, never the mid —
            // Part 9.2 prohibits mid fills by construction.
            const auto slip = SlippageModel::sample_ticks(
                config_.run_seed, order.corr_id, config_.mode,
                regime_of(order.symbol_id), market.spread_ticks());
            price = (order.side == Side::buy) ? market.ask_ticks + slip
                                              : market.bid_ticks - slip;
            available = (order.side == Side::buy) ? market.ask_size : market.bid_size;
            // L1 feeds publish no size; with nothing to ration, the whole
            // remainder fills at the touch. Queue results are already tagged
            // NOT_VALIDATED on such sources.
            if (available <= 0) available = order.remaining();
        } else {
            // Limit orders fill only on genuine activity *through* the level,
            // never on touch (Part 9.2): strict inequality is the whole point.
            const bool through = (order.side == Side::buy)
                ? (market.ask_ticks < order.limit_price_ticks)
                : (market.bid_ticks > order.limit_price_ticks);
            if (!through) return;
            if (order.queue_ahead > 0) return;   // still queued
            price = order.limit_price_ticks;     // passive fill at our own price
            const auto aggressor = (order.side == Side::buy) ? market.ask_size : market.bid_size;
            available = aggressor > 0 ? aggressor : order.remaining();
        }

        if (price <= 0) { halt(RejectReason::invalid_price); return; }

        std::int64_t volume = order.remaining();
        if (available < volume) volume = available;
        if (volume <= 0) return;

        // Impossible-fill guard. Over-filling, non-positive volume, or a fill
        // price better than the touch would all be silent corruption of the
        // result rather than a visible error.
        if (volume > order.remaining()) { halt(RejectReason::impossible_fill); return; }
        if (order.type == OrderType::market) {
            const bool better_than_touch = (order.side == Side::buy)
                ? (price < market.ask_ticks) : (price > market.bid_ticks);
            if (better_than_touch) { halt(RejectReason::impossible_fill); return; }
        }
        if (market.spread_ticks() >= 2) {
            const auto mid_twice = market.bid_ticks + market.ask_ticks;
            if (price * 2 == mid_twice && order.type == OrderType::market) {
                halt(RejectReason::impossible_fill);
                return;
            }
        }

        apply_fill(order, price, volume, now_ns);
    }

    void apply_fill(Order& order, std::int64_t price, std::int64_t volume,
                    std::uint64_t now_ns) noexcept {
        const auto& spec = specs_[order.symbol_id];

        const auto weighted = order.avg_fill_price_ticks * order.filled_volume + price * volume;
        order.filled_volume += volume;
        order.avg_fill_price_ticks =
            (weighted + order.filled_volume / 2) / order.filled_volume;

        const auto commission = CommissionModel::charge_minor(spec, volume);
        order.commission_minor += commission;
        account_.commission_paid_minor += commission;
        account_.balance_minor -= commission;

        apply_to_position(order, price, volume, now_ns);

        ++fill_count_;
        const bool final_fill = order.remaining() <= 0;
        (void)journal_.emit(JournalRecordType::fill, now_ns, config_.run_id,
                            order.ref.logical_order_id, order.symbol_id, last_ticket_,
                            price, volume, commission, final_fill ? 1 : 0);

        if (final_fill) finish(order, OrderState::filled, now_ns);
        else if (order.state != OrderState::partially_filled)
            transition(order, OrderState::partially_filled, now_ns);

        refresh_account(now_ns);
    }

    void apply_to_position(Order& order, std::int64_t price, std::int64_t volume,
                           std::uint64_t now_ns) noexcept {
        const auto& spec = specs_[order.symbol_id];

        // Hedging reduce_only: reduce the opposing ticket and realise PnL on the
        // closed quantity. Previously this fell through to the entry path and
        // opened a fresh opposing ticket, so exits added exposure instead of
        // closing it and realised PnL stayed at zero.
        if (config_.hedging && order.reduce_only) {
            auto* position = find_reducible(order.symbol_id, order.side);
            if (position == nullptr) { halt(RejectReason::reduce_only_no_position); return; }
            const auto closing = (volume < position->volume) ? volume : position->volume;
            const auto move = (position->side == Side::buy)
                ? (price - position->avg_price_ticks) : (position->avg_price_ticks - price);
            const auto realized = move * closing * spec.tick_value_minor;
            position->realized_pnl_minor += realized;
            account_.realized_pnl_minor += realized;
            account_.balance_minor += realized;
            position->volume -= closing;
            if (position->volume == 0) {
                position->state = PositionState::closed;
                position->closed_ns = now_ns;
            }
            last_ticket_ = position->position_ticket;
            journal_position(*position, now_ns);
            return;
        }

        if (config_.hedging) {
            // Hedging: fills of one order accumulate into that order's own
            // ticket; opposing exposure coexists rather than netting.
            auto* position = (order_ticket_[index_of(order)] != 0)
                ? positions_.find(order_ticket_[index_of(order)]) : nullptr;
            if (position == nullptr) {
                const auto ticket = ++ticket_seq_;
                position = positions_.open(ticket, order.symbol_id, order.side, volume, price,
                                           now_ns);
                if (position == nullptr) { halt(RejectReason::corrupted_state); return; }
                order_ticket_[index_of(order)] = ticket;
            } else {
                positions_.add(*position, volume, price);
            }
            last_ticket_ = position->position_ticket;
            journal_position(*position, now_ns);
            return;
        }

        // Netting: one signed position per symbol.
        auto* position = find_symbol_position(order.symbol_id);
        if (position == nullptr) {
            const auto ticket = ++ticket_seq_;
            position = positions_.open(ticket, order.symbol_id, order.side, volume, price, now_ns);
            if (position == nullptr) { halt(RejectReason::corrupted_state); return; }
        } else if (position->side == order.side) {
            positions_.add(*position, volume, price);
        } else {
            const auto closing = (volume < position->volume) ? volume : position->volume;
            const auto move = (position->side == Side::buy)
                ? (price - position->avg_price_ticks) : (position->avg_price_ticks - price);
            const auto realized = move * closing * spec.tick_value_minor;
            position->realized_pnl_minor += realized;
            account_.realized_pnl_minor += realized;
            account_.balance_minor += realized;
            position->volume -= closing;
            if (position->volume == 0) {
                position->state = PositionState::closed;
                position->closed_ns = now_ns;
                const auto residual = volume - closing;
                if (residual > 0) {
                    const auto ticket = ++ticket_seq_;
                    position = positions_.open(ticket, order.symbol_id, order.side, residual,
                                               price, now_ns);
                    if (position == nullptr) { halt(RejectReason::corrupted_state); return; }
                }
            }
        }
        last_ticket_ = position->position_ticket;
        journal_position(*position, now_ns);
    }

    void journal_position(const Position& position, std::uint64_t now_ns) noexcept {
        (void)journal_.emit(JournalRecordType::position_update, now_ns, config_.run_id, 0,
                            position.symbol_id, position.position_ticket,
                            static_cast<std::int64_t>(position.side), position.volume,
                            position.avg_price_ticks, position.realized_pnl_minor);
    }

    // ---- account ----------------------------------------------------------

    // Mark-to-market must happen on every quote, or equity and margin go stale
    // between fills and a stop-out driven purely by adverse price movement would
    // be missed. Journaling every quote would flood the record, so computation
    // and journaling are separated: state is always current, the journal carries
    // material events only.
    void recompute_account(std::uint64_t now_ns) noexcept {
        std::int64_t unrealized = 0;
        std::int64_t margin = 0;
        for (std::size_t i = 0; i < positions_.size(); ++i) {
            const auto& position = positions_.at(i);
            if (position.state == PositionState::closed) continue;
            const auto& spec = specs_[position.symbol_id];
            const auto& market = market_[position.symbol_id];
            const auto mark = (position.side == Side::buy) ? market.bid_ticks : market.ask_ticks;
            if (mark > 0) unrealized += position_pnl_minor(position, spec, mark);
            margin += MarginModel::required_minor(spec, position.volume, position.avg_price_ticks);
        }
        account_.unrealized_pnl_minor = unrealized;
        account_.margin_used_minor = margin + reserved_margin_minor_;
        account_.equity_minor = account_.balance_minor + unrealized;
        account_.free_margin_minor = account_.equity_minor - account_.margin_used_minor;
        account_.open_positions = positions_.open_count();

        // Stop-out is a first-class simulated outcome (Part 9.2), not an error.
        // It is always journalled, because a run that stopped out and a run that
        // did not are different results and the record must say which.
        if (!account_.stopped_out &&
            MarginModel::stop_out(account_, config_.stop_out_level_bp)) {
            account_.stopped_out = true;
            (void)journal_.emit(JournalRecordType::account_update, now_ns, config_.run_id, 0, 0, 0,
                                account_.balance_minor, account_.equity_minor,
                                account_.margin_used_minor,
                                static_cast<std::int64_t>(RejectReason::insufficient_margin));
            halt(RejectReason::insufficient_margin);
        }
    }

    void refresh_account(std::uint64_t now_ns) noexcept {
        recompute_account(now_ns);
        (void)journal_.emit(JournalRecordType::account_update, now_ns, config_.run_id, 0, 0, 0,
                            account_.balance_minor, account_.equity_minor,
                            account_.margin_used_minor, account_.free_margin_minor);
        (void)journal_.emit(JournalRecordType::pnl_update, now_ns, config_.run_id, 0, 0, 0,
                            account_.realized_pnl_minor, account_.unrealized_pnl_minor,
                            account_.commission_paid_minor, 0);
    }

    // ---- helpers ----------------------------------------------------------

    void halt(RejectReason reason) noexcept {
        if (halted_) return;
        halted_ = true;
        halt_reason_ = reason;
    }

    [[nodiscard]] std::size_t index_of(const Order& order) const noexcept {
        return static_cast<std::size_t>(&order - orders_.data());
    }

    [[nodiscard]] Order* find_live(BrokerOrderRef ref) noexcept {
        for (std::size_t i = 0; i < live_count_; ++i)
            if (orders_[i].ref == ref && !is_terminal(orders_[i].state)) return &orders_[i];
        return nullptr;
    }

    // Order-identity dedupe (Part 8.2). This used to linear-scan the journal on
    // every submit, which made submit O(journal_size) — 16 us at a full ring,
    // two orders of magnitude above the decision path and the single largest
    // cost in the pipeline. It was also weaker than it looked: the journal is a
    // bounded ring, so the duplicate guarantee quietly expired once it rolled.
    //
    // A fixed open-addressed set gives O(1) and, being independent of the
    // journal, survives rotation. It is still a bounded window by construction —
    // the hot path may not allocate — but the window is now explicit and only
    // ever evicts the oldest occupant of a probe run, deterministically.
    //
    // Only the logical id is stored. Every ref this broker mints carries
    // config_.run_id, so run_id is invariant within an instance and comparing it
    // per slot would double the table for no discrimination; has_seen guards the
    // run explicitly instead. Sizing matters here: PaperBroker is held by value
    // in the tests and the runtime, and a 128 KiB table overflowed MSVC's 1 MiB
    // default stack in suites that keep two brokers live at once.
    static constexpr std::size_t seen_slots = 1024;   // power of two, 8 KiB
    static constexpr std::size_t seen_probe = 8;

    [[nodiscard]] static constexpr std::uint64_t mix(std::uint64_t x) noexcept {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }

    [[nodiscard]] bool has_seen(BrokerOrderRef ref) const noexcept {
        if (ref.run_id != config_.run_id || ref.logical_order_id == 0) return false;
        const auto home = static_cast<std::size_t>(mix(ref.logical_order_id)) & (seen_slots - 1);
        for (std::size_t step = 0; step < seen_probe; ++step) {
            const auto slot = seen_[(home + step) & (seen_slots - 1)];
            if (slot == 0) return false;                       // probe run ended
            if (slot == ref.logical_order_id) return true;
        }
        return false;
    }

    void remember(BrokerOrderRef ref) noexcept {
        if (ref.logical_order_id == 0) return;
        const auto home = static_cast<std::size_t>(mix(ref.logical_order_id)) & (seen_slots - 1);
        for (std::size_t step = 0; step < seen_probe; ++step) {
            auto& slot = seen_[(home + step) & (seen_slots - 1)];
            if (slot == 0 || slot == ref.logical_order_id) {
                slot = ref.logical_order_id;
                return;
            }
        }
        // Probe run full: evict its head. Deterministic, so two runs over the
        // same input evict identically and the determinism gate still holds.
        seen_[home] = ref.logical_order_id;
    }

    // A reduce_only BUY reduces a SHORT; a reduce_only SELL reduces a LONG.
    [[nodiscard]] Position* find_reducible(std::uint32_t symbol_id, Side order_side) noexcept {
        const auto target = (order_side == Side::buy) ? Side::sell : Side::buy;
        for (std::size_t i = 0; i < positions_.size(); ++i) {
            auto& position = positions_.at(i);
            if (position.symbol_id == symbol_id && position.side == target &&
                position.state != PositionState::closed && position.volume > 0)
                return &position;
        }
        return nullptr;
    }

    [[nodiscard]] std::int64_t reducible_volume(std::uint32_t symbol_id, Side order_side) noexcept {
        const auto target = (order_side == Side::buy) ? Side::sell : Side::buy;
        std::int64_t total = 0;
        for (std::size_t i = 0; i < positions_.size(); ++i) {
            const auto& position = positions_.at(i);
            if (position.symbol_id == symbol_id && position.side == target &&
                position.state != PositionState::closed)
                total += position.volume;
        }
        return total;
    }

    [[nodiscard]] Position* find_symbol_position(std::uint32_t symbol_id) noexcept {
        for (std::size_t i = 0; i < positions_.size(); ++i) {
            auto& position = positions_.at(i);
            if (position.symbol_id == symbol_id && position.state != PositionState::closed)
                return &position;
        }
        return nullptr;
    }

    [[nodiscard]] RegimeObservation regime_observation(std::uint32_t symbol_id) const noexcept {
        RegimeObservation observation{};
        const auto& market = market_[symbol_id];
        observation.spread_ticks = market.spread_ticks();
        observation.median_spread_ticks = 1;
        observation.events_per_second = 0.0;
        observation.realised_vol = 0.0;
        return observation;
    }

    [[nodiscard]] RegimeBucket regime_of(std::uint32_t symbol_id) const noexcept {
        return classify(regime_observation(symbol_id));
    }

    [[nodiscard]] std::int64_t queue_at_placement(const Order& order,
                                                  const MarketState& market) const noexcept {
        const auto profile = ModeProfile::for_mode(config_.mode);
        const auto size_at_level = (order.side == Side::buy) ? market.bid_size : market.ask_size;
        if (size_at_level <= 0) return 0;
        // Front / mid / back of queue per Part 9.4.
        return static_cast<std::int64_t>(
            static_cast<double>(size_at_level) * profile.queue_placement);
    }

    void credit_queue(std::uint32_t symbol_id, const MarketState& previous, std::int64_t bid,
                      std::int64_t ask, std::int64_t bid_size, std::int64_t ask_size) noexcept {
        if (!previous.valid) return;
        for (std::size_t i = 0; i < live_count_; ++i) {
            auto& order = orders_[i];
            if (order.symbol_id != symbol_id || order.type != OrderType::limit) continue;
            if (is_terminal(order.state) || order.queue_ahead <= 0) continue;

            const bool same_level = (order.side == Side::buy)
                ? (bid == previous.bid_ticks && order.limit_price_ticks == bid)
                : (ask == previous.ask_ticks && order.limit_price_ticks == ask);
            if (!same_level) continue;

            const auto before = (order.side == Side::buy) ? previous.bid_size : previous.ask_size;
            const auto after = (order.side == Side::buy) ? bid_size : ask_size;
            if (after >= before) continue;

            const auto reduction = before - after;
            // Only a share of the reduction is genuine trading; the rest is
            // cancellation ahead of us, which does not advance our position by
            // as much. The share is drawn deterministically.
            const auto share = Philox4x32::next_range(config_.run_seed, order.corr_id,
                                                      RngStage::queue_share, 1, 100);
            const auto credited = (reduction * static_cast<std::int64_t>(share)) / 100;
            order.queue_ahead -= credited;
            if (order.queue_ahead < 0) order.queue_ahead = 0;
        }
    }

    BrokerConfig config_{};
    AccountState account_{};
    PositionBook positions_{};
    Journal journal_{};

    std::array<Order, max_live_orders> orders_{};
    std::array<std::uint64_t, max_live_orders> order_ticket_{};
    std::array<std::uint64_t, seen_slots> seen_{};
    std::array<SymbolSpec, max_symbols> specs_{};
    std::array<MarketState, max_symbols> market_{};

    std::size_t live_count_{0};
    std::uint64_t ticket_seq_{0};
    std::uint64_t last_ticket_{0};
    std::uint64_t fill_count_{0};
    std::uint64_t last_submit_ns_{0};
    std::int64_t reserved_margin_minor_{0};
    bool halted_{false};
    RejectReason halt_reason_{RejectReason::none};
};

}  // namespace exec
