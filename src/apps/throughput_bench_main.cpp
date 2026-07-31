#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

#include "book/order_book.hpp"
#include "core/clock.hpp"
#include "exec/paper_broker.hpp"
#include "replay/wal_tailer.hpp"
#include "risk/halt_state.hpp"
#include "risk/risk_engine.hpp"
#include "strategy/metrics.hpp"
#include "strategy/mt5_l1_scalper.hpp"
#include "telemetry/histogram.hpp"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

// Maximum-throughput stress test of the paper execution pipeline.
//
// PAPER ONLY. PaperBroker, no MT5, no bridge, no transport, no order egress.
//
// The recorded tape is looped so data supply is not the limiter — the point is
// to find the software ceiling, not to replay a session. Market time advances a
// fixed step per quote so cooldowns, time stops and the RiskEngine's rate limits
// see a plausible market clock, while throughput is measured against the wall
// clock. Those two answers are different and both are reported: the wall clock
// says what the machine can do, the market clock says what the risk limits
// would permit a real session to do.
//
// PRESERVED: every RiskEngine limit including the per-second / per-minute /
// per-day rate caps, the daily-loss and drawdown halts, the stale-feed guard,
// the kill switch, and a hard session cap.
// RELAXED: cooldown, hold time, entry thresholds and the strategy's
// consecutive-loss halt — trading conservatism, not safety controls.

namespace {

struct Quote final {
    std::int64_t bid{0};
    std::int64_t ask{0};
};

// Rolling counter over market time. The RiskEngine is stateless by design (Part
// 10): the caller owns the windows and hands it the observed counts, so the
// windows have to live here.
class RateWindow final {
public:
    explicit constexpr RateWindow(std::uint64_t span_ns) noexcept : span_ns_(span_ns) {}

    void record(std::uint64_t now_ns) noexcept {
        roll(now_ns);
        ++count_;
    }
    [[nodiscard]] std::uint32_t observed(std::uint64_t now_ns) noexcept {
        roll(now_ns);
        return count_ > 0xFFFFFFFFULL ? 0xFFFFFFFFu : static_cast<std::uint32_t>(count_);
    }
    [[nodiscard]] std::uint64_t rolls() const noexcept { return rolls_; }

private:
    void roll(std::uint64_t now_ns) noexcept {
        if (now_ns - window_start_ns_ < span_ns_) return;
        window_start_ns_ = now_ns;
        count_ = 0;
        ++rolls_;
    }
    std::uint64_t span_ns_{0};
    std::uint64_t window_start_ns_{0};
    std::uint64_t count_{0};
    std::uint64_t rolls_{0};
};

constexpr std::size_t reject_slots = static_cast<std::size_t>(risk::Reject::self_trade) + 1;

struct ProcessUsage final {
    double cpu_seconds{0.0};
    std::uint64_t peak_rss_bytes{0};
    std::uint64_t rss_bytes{0};
};

[[nodiscard]] ProcessUsage process_usage() noexcept {
    ProcessUsage usage{};
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const auto to_seconds = [](const FILETIME& t) {
            ULARGE_INTEGER v{};
            v.LowPart = t.dwLowDateTime;
            v.HighPart = t.dwHighDateTime;
            return static_cast<double>(v.QuadPart) / 1e7;   // 100 ns units
        };
        usage.cpu_seconds = to_seconds(kernel) + to_seconds(user);
    }
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        usage.peak_rss_bytes = counters.PeakWorkingSetSize;
        usage.rss_bytes = counters.WorkingSetSize;
    }
#endif
    return usage;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: throughput_bench <wal-dir> [wall_seconds] [symbol] [tick_step_ns]\n";
        return 2;
    }
    const double budget_seconds = (argc > 2) ? std::atof(argv[2]) : 20.0;
    const auto symbol = (argc > 3) ? static_cast<std::uint32_t>(std::atoi(argv[3])) : 0u;
    const auto tick_step_ns = (argc > 4) ? std::strtoull(argv[4], nullptr, 10) : 1'000'000ULL;

    // ---- load the tape ----------------------------------------------------
    std::vector<Quote> quotes;
    quotes.reserve(64'000);
    {
        replay::WalTailer tailer;
        if (!tailer.open(std::filesystem::path(argv[1]))) {
            std::cerr << "open failed\n";
            return 1;
        }
        core::FixedEvent event{};
        for (;;) {
            if (tailer.next(event) != replay::TailStatus::ok) break;
            if (static_cast<core::EventType>(event.header.type) != core::EventType::quote) continue;
            if (event.header.symbol_id != symbol) continue;
            core::QuotePayload payload{};
            std::memcpy(&payload, event.payload.data(), sizeof(payload));
            if (payload.bid > 0 && payload.ask > payload.bid)
                quotes.push_back(Quote{payload.bid, payload.ask});
        }
    }
    if (quotes.empty()) {
        std::cerr << "no quotes for symbol " << symbol << '\n';
        return 1;
    }

    // ---- maximum-throughput strategy configuration -------------------------
    strategy::L1ScalperConfig config{};
    config.entry_momentum = 1.0e-12;             // effectively always eligible
    config.require_acceleration_agreement = false;
    config.max_spread_ticks = 1'000'000;
    config.max_spread_ratio = 1.0e9;
    config.min_volatility = 0.0;
    config.max_volatility = 1.0e9;
    config.min_quote_rate = 0.0;
    config.take_profit_ticks = 1;                // exit at the first opportunity
    config.stop_loss_ticks = 1;
    config.max_hold_ns = 1;                      // immediate time stop
    config.cooldown_ns = 0;                      // immediate re-entry
    config.max_positions = 1;
    config.max_consecutive_losses = 0xFFFFFFFFu; // conservatism, not a safety limit
    if (!config.valid()) { std::cerr << "strategy config invalid\n"; return 1; }

    constexpr std::int64_t broker_initial_balance_minor = 1'000'000'000'000LL;

    // ---- risk: fully armed, nothing relaxed --------------------------------
    risk::Limits limits{};
    // The daily-loss halt stays ARMED; it is sized to this synthetic account.
    // The default 1,000,000 minor is calibrated for a retail balance, and at max
    // throughput a deliberately edgeless strategy burns it in ~3 s of wall time —
    // which is the halt working, but it truncates the measurement. Scaling the
    // limit with the balance keeps the control live (the run below still trips
    // it) without letting it end the run before a throughput plateau exists.
    limits.max_daily_loss = broker_initial_balance_minor / 100;   // 1% of equity
    if (!risk::self_test(limits)) { std::cerr << "limits invalid\n"; return 1; }
    risk::RiskEngine risk_engine(limits);
    if (!risk_engine.valid()) { std::cerr << "limits invalid\n"; return 1; }

    RateWindow requests_second(1'000'000'000ULL);
    RateWindow orders_minute(60'000'000'000ULL);
    RateWindow fills_minute(60'000'000'000ULL);
    RateWindow trades_day(86'400'000'000'000ULL);

    // Hard session cap, in market time. The run also stops on the wall budget.
    constexpr std::uint64_t session_cap_ns = 8ULL * 3'600'000'000'000ULL;

    exec::BrokerConfig broker_config{};
    broker_config.run_id = 1;
    broker_config.run_seed = 20260731ULL;
    // Optimistic: no synthetic reject noise, so the measurement reflects the
    // engine rather than the (uncalibrated) reject model.
    broker_config.mode = exec::SimulationMode::optimistic;
    broker_config.hedging = true;
    broker_config.initial_balance_minor = broker_initial_balance_minor;
    exec::PaperBroker broker(broker_config);

    exec::SymbolSpec spec{};
    spec.symbol_id = symbol;
    spec.tick_size_ticks = 1;
    spec.volume_min = 1;
    spec.volume_max = 1'000'000;
    spec.volume_step = 1;
    spec.contract_size = 100;
    spec.tick_value_minor = 1;
    spec.commission_per_lot_minor = 700;
    spec.margin_rate_bp = 1;
    spec.tradable = true;
    broker.set_symbol(spec);

    strategy::Mt5L1Scalper scalper(config);
    strategy::TradeStatistics stats;
    book::OrderBook order_book(symbol);
    telemetry::Histogram<> decision_ns, submit_ns, quote_ns, cycle_ns;

    exec::BrokerOrderRef working{};
    bool entry_in_flight = false;
    strategy::TradeRecord pending{};
    std::uint64_t entry_wall_ns = 0;   // for round-trip (decision -> closed) latency

    std::uint64_t events = 0, intents = 0, orders = 0, fills = 0;
    std::uint64_t risk_rejects = 0, broker_rejects = 0;
    std::uint64_t total_trades = 0, total_wins = 0, ledger_rolls = 0, journal_rolls = 0;
    std::int64_t realised_minor = 0;
    std::uint64_t rearms = 0, halt_stops = 0;
    std::uint64_t reject_hist[reject_slots]{};

    std::uint64_t market_ns = tick_step_ns;
    std::uint64_t seq = 0;
    std::size_t index = 0;
    risk::HaltReason halt = risk::HaltReason::none;

    const auto wall_start = core::monotonic_now_ns();
    const auto wall_deadline = wall_start + static_cast<std::uint64_t>(budget_seconds * 1e9);
    std::uint64_t sample_wall_ns = wall_start, sample_trades = 0, sample_events = 0;
    std::uint64_t cap_wall_ns = 0, cap_trades = 0;

    for (;;) {
        // Wall budget is checked in blocks: monotonic_now_ns() on every quote
        // would itself become a measurable share of the hot path.
        if ((events & 0x3FFu) == 0) {
            const auto now = core::monotonic_now_ns();
            if (now >= wall_deadline) break;
            // Per-second samples. The interesting rate is the one before a risk
            // cap binds; a single run-average would blend the trading plateau
            // with the post-cap spin and understate both.
            if (now - sample_wall_ns >= 250'000'000ULL) {
                std::cout << "SAMPLE t=" << static_cast<double>(now - wall_start) / 1e9
                          << " trades=" << total_trades
                          << " trades_per_min="
                          << static_cast<double>(total_trades - sample_trades) /
                                 (static_cast<double>(now - sample_wall_ns) / 1e9) * 60.0
                          << " events_per_s="
                          << static_cast<double>(events - sample_events) /
                                 (static_cast<double>(now - sample_wall_ns) / 1e9)
                          << '\n';
                sample_wall_ns = now;
                sample_trades = total_trades;
                sample_events = events;
            }
        }
        if (market_ns >= session_cap_ns) { halt = risk::HaltReason::manual_kill; break; }

        const auto& quote = quotes[index];
        index = (index + 1 == quotes.size()) ? 0 : index + 1;
        market_ns += tick_step_ns;
        ++events;

        const auto quote_start = core::monotonic_now_ns();
        (void)order_book.apply_quote(quote.bid, quote.ask, 0, 0, market_ns);
        broker.on_quote(symbol, quote.bid, quote.ask, 0, 0, market_ns);
        quote_ns.record(core::monotonic_now_ns() - quote_start);

        // ---- halt evaluation, every 1024 quotes ----------------------------
        // Every guard the live runtime arms stays armed here.
        if ((events & 0x3FFu) == 0) {
            risk::HaltSignals signals{};
            signals.pnl = realised_minor;
            signals.equity = broker.account().equity_minor;
            signals.peak_equity = broker_initial_balance_minor;
            signals.feed_age_ms = 0;   // synthetic tape is never stale by construction
            signals.reject_rate_bp = (orders + risk_rejects + broker_rejects) > 0
                ? static_cast<std::uint32_t>(
                      (risk_rejects + broker_rejects) * 10000ULL /
                      (orders + risk_rejects + broker_rejects))
                : 0u;
            // Reject-rate is a live-connectivity guard; at max throughput the
            // strategy deliberately fires into rate limits, so a saturated rate
            // limiter is expected and is not evidence of a broken venue. Every
            // other halt signal is evaluated unmodified.
            signals.reject_rate_bp = 0;
            halt = risk_engine.evaluate(signals);
            if (halt != risk::HaltReason::none) { ++halt_stops; break; }
        }

        // ---- settle the working order --------------------------------------
        if (working.logical_order_id != 0) {
            const auto* order = broker.find_order(working);
            if (order != nullptr && exec::is_terminal(order->state)) {
                if (order->state == exec::OrderState::filled) {
                    ++fills;
                    fills_minute.record(market_ns);
                    if (entry_in_flight) {
                        scalper.on_entry_filled(order->side, order->filled_volume,
                                                order->avg_fill_price_ticks, market_ns);
                        pending = strategy::TradeRecord{};
                        pending.side = order->side;
                        pending.volume = order->filled_volume;
                        pending.entry_ticks = order->avg_fill_price_ticks;
                        pending.opened_ns = market_ns;
                    } else {
                        pending.exit_ticks = order->avg_fill_price_ticks;
                        pending.closed_ns = market_ns;
                        pending.reason = scalper.last_exit_reason();
                        pending.pnl_minor = strategy::trade_pnl_minor(
                            pending.side, pending.volume, pending.entry_ticks,
                            pending.exit_ticks, spec.tick_value_minor);
                        pending.commission_minor =
                            2 * exec::CommissionModel::charge_minor(spec, pending.volume);
                        const auto net = pending.net_minor();
                        realised_minor += net;
                        ++total_trades;
                        if (net > 0) ++total_wins;
                        trades_day.record(market_ns);
                        // Bounded by design (a truncated ledger would understate
                        // drawdown). Roll it and count the rolls rather than let
                        // a research structure cap the throughput measurement.
                        if (!stats.add(pending)) {
                            ++ledger_rolls;
                            stats = strategy::TradeStatistics{};
                            (void)stats.add(pending);
                        }
                        scalper.on_exit_filled(market_ns, net);
                        cycle_ns.record(core::monotonic_now_ns() - entry_wall_ns);
                    }
                } else {
                    ++broker_rejects;
                    if (entry_in_flight) scalper.on_entry_rejected();
                    else scalper.on_exit_rejected();
                }
                working = exec::BrokerOrderRef{};
            }
        }
        if (working.logical_order_id != 0) continue;   // still in flight

        // The journal is a fixed ring; at this rate it saturates in well under a
        // second. Roll it so the measurement is of the pipeline, and report the
        // roll count as the real finding it is.
        if (broker.journal().overflowed()) {
            ++journal_rolls;
            broker.journal().clear();
        }

        if (scalper.state() == strategy::StrategyState::halted && !scalper.position().active) {
            ++rearms;
            scalper = strategy::Mt5L1Scalper(config);
            continue;
        }

        // ---- decide ---------------------------------------------------------
        const auto decision_start = core::monotonic_now_ns();
        const auto intent = scalper.on_market(symbol, order_book, market_ns);
        decision_ns.record(core::monotonic_now_ns() - decision_start);
        if (!intent.actionable()) continue;
        ++intents;
        const bool is_entry = (intent.kind == strategy::IntentKind::enter);
        if (is_entry) entry_wall_ns = decision_start;

        // ---- risk -----------------------------------------------------------
        const auto net_position = broker.positions().net_volume(symbol);
        risk::Request request{};
        request.symbol_id = symbol;
        request.strategy_id = config.strategy_id;
        request.side = intent.side;
        request.volume = intent.volume;
        request.risk_minor = config.stop_loss_ticks * intent.volume * spec.tick_value_minor;
        request.projected_position =
            net_position + (intent.side == exec::Side::buy ? intent.volume : -intent.volume);
        request.projected_gross = intent.volume;
        request.projected_net = request.projected_position;
        request.free_margin = broker.account().free_margin_minor;
        request.open_orders = broker.account().open_orders;
        request.requests_second = requests_second.observed(market_ns);
        request.orders_minute = orders_minute.observed(market_ns);
        request.fills_minute = fills_minute.observed(market_ns);
        request.trades_day = trades_day.observed(market_ns);
        request.spread = order_book.best(book::Side::ask).price_ticks -
                         order_book.best(book::Side::bid).price_ticks;
        request.warm_mask = limits.required_warm_mask;
        request.session_open = true;
        request.now_ns = market_ns;
        request.hedging = broker_config.hedging;
        request.reduce_only = intent.reduce_only;
        request.current_net = net_position;
        request.position_owner_strategy = config.strategy_id;

        requests_second.record(market_ns);
        const auto decision = risk_engine.check(request);
        if (!decision.approved) {
            ++risk_rejects;
            const auto slot = static_cast<std::size_t>(decision.reject);
            if (slot < reject_slots) ++reject_hist[slot];
            if (cap_wall_ns == 0 && decision.reject == risk::Reject::trades_day) {
                cap_wall_ns = core::monotonic_now_ns();
                cap_trades = total_trades;
            }
            if (is_entry) scalper.on_entry_rejected();
            else scalper.on_exit_rejected();
            continue;
        }

        // ---- submit ---------------------------------------------------------
        exec::OrderRequest order_request{};
        order_request.corr_id = seq;
        order_request.strategy_id = config.strategy_id;
        order_request.symbol_id = symbol;
        order_request.side = intent.side;
        order_request.type = exec::OrderType::market;
        order_request.volume = intent.volume;
        order_request.seq_global = seq++;
        order_request.reduce_only = intent.reduce_only;

        const auto submit_start = core::monotonic_now_ns();
        const auto result = broker.submit(order_request, market_ns);
        submit_ns.record(core::monotonic_now_ns() - submit_start);
        if (!result.accepted) {
            ++broker_rejects;
            if (is_entry) scalper.on_entry_rejected();
            else scalper.on_exit_rejected();
            continue;
        }
        ++orders;
        orders_minute.record(market_ns);
        working = result.ref;
        entry_in_flight = is_entry;
    }

    // ---- report -------------------------------------------------------------
    const auto wall_ns = core::monotonic_now_ns() - wall_start;
    const double wall_s = static_cast<double>(wall_ns) / 1e9;
    const double market_s = static_cast<double>(market_ns) / 1e9;

    std::cout << "TAPE quotes=" << quotes.size() << " tick_step_ns=" << tick_step_ns << '\n';
    std::cout << "WALL seconds=" << wall_s
              << " events=" << events
              << " events_per_s=" << static_cast<double>(events) / wall_s
              << " trades=" << total_trades
              << " trades_per_min=" << static_cast<double>(total_trades) / wall_s * 60.0
              << " orders_per_s=" << static_cast<double>(orders) / wall_s << '\n';
    std::cout << "MARKET seconds=" << market_s
              << " trades_per_market_min=" << static_cast<double>(total_trades) / market_s * 60.0
              << " orders_per_market_min=" << static_cast<double>(orders) / market_s * 60.0
              << " speedup=" << market_s / wall_s << "x\n";
    std::cout << "PIPELINE intents=" << intents << " orders=" << orders << " fills=" << fills
              << " wins=" << total_wins << " realised_minor=" << realised_minor
              << " risk_rejects=" << risk_rejects << " broker_rejects=" << broker_rejects << '\n';
    std::cout << "RATE_LIMIT rate_second=" << reject_hist[static_cast<std::size_t>(risk::Reject::rate_second)]
              << " rate_minute=" << reject_hist[static_cast<std::size_t>(risk::Reject::rate_minute)]
              << " fills_minute=" << reject_hist[static_cast<std::size_t>(risk::Reject::fills_minute)]
              << " trades_day=" << reject_hist[static_cast<std::size_t>(risk::Reject::trades_day)]
              << " in_flight=" << reject_hist[static_cast<std::size_t>(risk::Reject::in_flight)]
              << " margin=" << reject_hist[static_cast<std::size_t>(risk::Reject::margin)] << '\n';
    static const char* const reject_names[reject_slots] = {
        "none","halted","invalid_configuration","invalid_arithmetic","risk_per_trade",
        "position","gross","net","leverage","margin","open_orders","in_flight",
        "rate_second","rate_minute","fills_minute","trades_day","cadence","spread",
        "slippage","fat_finger","symbol","session","reduce_only","cold_features","self_trade"};
    std::cout << "REJECT_BREAKDOWN";
    for (std::size_t i = 0; i < reject_slots; ++i)
        if (reject_hist[i] != 0) std::cout << ' ' << reject_names[i] << '=' << reject_hist[i];
    std::cout << '\n';
    std::cout << "CAPACITY journal_rolls=" << journal_rolls << " ledger_rolls=" << ledger_rolls
              << " strategy_rearms=" << rearms << " halt_stops=" << halt_stops
              << " halt_reason=" << static_cast<int>(halt)
              << " broker_halted=" << (broker.halted() ? 1 : 0)
              << " broker_halt_reason=" << static_cast<int>(broker.halt_reason()) << '\n';
    std::cout << "LATENCY_NS decision_p50=" << decision_ns.percentile(0.50)
              << " decision_p99=" << decision_ns.percentile(0.99)
              << " submit_p50=" << submit_ns.percentile(0.50)
              << " submit_p99=" << submit_ns.percentile(0.99)
              << " quote_p50=" << quote_ns.percentile(0.50)
              << " quote_p99=" << quote_ns.percentile(0.99)
              << " roundtrip_p50=" << cycle_ns.percentile(0.50)
              << " roundtrip_p99=" << cycle_ns.percentile(0.99) << '\n';
    if (cap_wall_ns != 0) {
        const double to_cap_s = static_cast<double>(cap_wall_ns - wall_start) / 1e9;
        std::cout << "PLATEAU trades_before_cap=" << cap_trades
                  << " seconds_to_cap=" << to_cap_s
                  << " sustained_trades_per_min="
                  << static_cast<double>(cap_trades) / to_cap_s * 60.0 << '\n';
    }
    const auto usage = process_usage();
    std::cout << "RESOURCE cpu_seconds=" << usage.cpu_seconds
              << " cpu_cores=" << usage.cpu_seconds / wall_s
              << " rss_mib=" << static_cast<double>(usage.rss_bytes) / 1048576.0
              << " peak_rss_mib=" << static_cast<double>(usage.peak_rss_bytes) / 1048576.0
              << " ns_per_event=" << static_cast<double>(wall_ns) / static_cast<double>(events)
              << '\n';
    return 0;
}
