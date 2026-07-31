#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

#include "book/order_book.hpp"
#include "exec/paper_broker.hpp"
#include "replay/wal_tailer.hpp"
#include "strategy/metrics.hpp"
#include "strategy/mt5_l1_scalper.hpp"

// Parameter sweep for mt5_l1_scalper_v1 over recorded data.
//
// Broker, risk, recorder and execution code are untouched; this only constructs
// strategy configs and replays them. Risk halts stay armed — a configuration is
// not allowed to buy trade count by disabling its own safety.
//
// Split is by TIME, not by shuffling: train is the earlier fraction of the
// recording, validation the later. The best train config is then run ONCE on
// validation and reported separately. Validation is never used for selection.

namespace {

struct Quote final { std::int64_t bid, ask; std::uint64_t ts; };

struct Result final {
    std::size_t trades{0};
    std::int64_t net{0}, gross_win{0}, gross_loss{0}, max_dd{0};
    std::int64_t commission{0}, spread_cost{0};
    double win_rate{0.0}, profit_factor{0.0};
    bool halted{false};
};

exec::SimulationMode g_mode = exec::SimulationMode::pessimistic;
bool g_trace = false;

Result run(const std::vector<Quote>& quotes, std::size_t from, std::size_t to,
           const strategy::L1ScalperConfig& config) {
    Result r{};
    strategy::Mt5L1Scalper scalper(config);
    strategy::TradeStatistics stats;
    book::OrderBook book(0);

    exec::BrokerConfig bc{};
    bc.run_id = 1; bc.run_seed = 20260731;
    bc.mode = g_mode;
    bc.hedging = true;
    bc.initial_balance_minor = 1'000'000'000;
    exec::PaperBroker broker(bc);

    exec::SymbolSpec spec{};
    spec.symbol_id = 0; spec.tick_size_ticks = 1;
    spec.volume_min = 1; spec.volume_max = 1'000'000; spec.volume_step = 1;
    // contract_size 100 with 700 minor/lot => 7 minor per side, 14 per round
    // trip, against a ~10-tick spread. Costs are deliberately material.
    spec.contract_size = 100; spec.tick_value_minor = 1;
    spec.commission_per_lot_minor = 700; spec.margin_rate_bp = 1; spec.tradable = true;
    broker.set_symbol(spec);

    exec::BrokerOrderRef working{};
    bool entry_in_flight = false;
    strategy::TradeRecord pending{};
    std::uint64_t seq = 0;
    std::uint64_t rearms = 0;
    constexpr std::uint64_t max_rearms = 200;

    for (std::size_t i = from; i < to; ++i) {
        const auto& q = quotes[i];
        (void)book.apply_quote(q.bid, q.ask, 0, 0, q.ts);
        broker.on_quote(0, q.bid, q.ask, 0, 0, q.ts);

        if (working.logical_order_id != 0) {
            const auto* o = broker.find_order(working);
            if (o != nullptr && exec::is_terminal(o->state)) {
                if (o->state == exec::OrderState::filled) {
                    if (entry_in_flight) {
                        scalper.on_entry_filled(o->side, o->filled_volume,
                                                o->avg_fill_price_ticks, q.ts);
                        pending = strategy::TradeRecord{};
                        pending.side = o->side; pending.volume = o->filled_volume;
                        pending.entry_ticks = o->avg_fill_price_ticks;
                        pending.opened_ns = q.ts;
                        pending.commission_minor = o->commission_minor;
                    } else {
                        pending.exit_ticks = o->avg_fill_price_ticks;
                        pending.closed_ns = q.ts;
                        pending.commission_minor += o->commission_minor;
                        pending.reason = scalper.last_exit_reason();
                        pending.pnl_minor = strategy::trade_pnl_minor(
                            pending.side, pending.volume, pending.entry_ticks,
                            pending.exit_ticks, 1);
                        (void)stats.add(pending);
                        r.commission += pending.commission_minor;
                        r.spread_cost += (q.ask - q.bid);
                        if (g_trace)
                            std::cout << "TRADE side=" << (pending.side == exec::Side::buy ? "B" : "S")
                                      << " entry=" << pending.entry_ticks
                                      << " exit=" << pending.exit_ticks
                                      << " pnl=" << pending.pnl_minor
                                      << " hold_ms=" << (pending.hold_ns() / 1000000)
                                      << " reason=" << static_cast<int>(pending.reason)
                                      << " quote=" << q.bid << '/' << q.ask << '\n';
                        scalper.on_exit_filled(q.ts, pending.net_minor());
                    }
                } else {
                    if (entry_in_flight) scalper.on_entry_rejected();
                    else scalper.on_exit_rejected();
                }
                working = exec::BrokerOrderRef{};
            }
        }
        if (working.logical_order_id != 0) continue;

        // Mirror the live runtime's bounded operator re-arm. Without it every
        // config stops at the 5-loss halt and can never reach the 30-trade
        // minimum, so the sweep would be comparing truncated samples rather
        // than configurations. The halt still fires and is still bounded.
        if (scalper.state() == strategy::StrategyState::halted &&
            !scalper.position().active && rearms < max_rearms) {
            ++rearms;
            scalper = strategy::Mt5L1Scalper(config);
            continue;
        }

        const auto intent = scalper.on_market(0, book, q.ts);
        if (!intent.actionable()) continue;

        exec::OrderRequest req{};
        req.corr_id = i; req.strategy_id = 2; req.symbol_id = 0;
        req.side = intent.side; req.type = exec::OrderType::market;
        req.volume = intent.volume; req.seq_global = seq++;
        req.reduce_only = intent.reduce_only;
        const auto sub = broker.submit(req, q.ts);
        if (!sub.accepted) {
            if (intent.kind == strategy::IntentKind::enter) scalper.on_entry_rejected();
            else scalper.on_exit_rejected();
            continue;
        }
        working = sub.ref;
        entry_in_flight = (intent.kind == strategy::IntentKind::enter);
    }

    r.trades = stats.trades();
    r.net = stats.net_pnl_minor();
    r.gross_win = stats.gross_win_minor();
    r.gross_loss = stats.gross_loss_minor();
    r.max_dd = stats.max_drawdown_minor();
    r.win_rate = stats.win_rate();
    r.profit_factor = stats.profit_factor();
    r.halted = (scalper.state() == strategy::StrategyState::halted);
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: l1_tune <wal-dir> [symbol]\n"; return 2; }
    const std::uint32_t symbol = (argc > 2) ? static_cast<std::uint32_t>(std::atoi(argv[2])) : 0;
    if (argc > 3) g_mode = static_cast<exec::SimulationMode>(std::atoi(argv[3]));
    if (argc > 4) g_trace = (std::atoi(argv[4]) != 0);

    // ---- load quotes for one symbol -------------------------------------
    std::vector<Quote> quotes;
    quotes.reserve(200'000);
    {
        replay::WalTailer tailer;
        if (!tailer.open(std::filesystem::path(argv[1]))) { std::cerr << "open failed\n"; return 1; }
        core::FixedEvent e{};
        for (;;) {
            const auto s = tailer.next(e);
            if (s != replay::TailStatus::ok) break;
            if (static_cast<core::EventType>(e.header.type) != core::EventType::quote) continue;
            if (e.header.symbol_id != symbol) continue;
            core::QuotePayload p{};
            std::memcpy(&p, e.payload.data(), sizeof(p));
            if (p.bid > 0 && p.ask > p.bid) quotes.push_back({p.bid, p.ask, e.header.ts_local_ns});
        }
    }
    if (quotes.size() < 1000) { std::cerr << "insufficient quotes=" << quotes.size() << '\n'; return 1; }

    const std::size_t split = (quotes.size() * 70) / 100;   // time-ordered 70/30
    std::cout << "quotes=" << quotes.size() << " train=" << split
              << " validation=" << (quotes.size() - split) << '\n';

    // ---- conservative grid ------------------------------------------------
    const double momentum[]   = {1.0e-6, 2.0e-6, 5.0e-6};
    const std::int64_t spread[] = {15, 25};
    const double vol_max[]    = {2.0e-5, 5.0e-5};
    const double qrate[]      = {0.0, 0.5};
    // Round-trip cost is ~10 ticks spread + 14 minor commission = ~24 minor.
    // A take-profit at or below that is unwinnable arithmetically, so the grid
    // starts above it. Stops are sized against the target, not independently.
    // Stops must clear spread + slippage (audit: a stop inside that band is hit
    // on the first tick). Targets must clear round-trip cost. Holds lengthened
    // because the time stop was ending trades before the target was reachable.
    const std::int64_t sl[]   = {40, 60, 90};
    const std::int64_t tp[]   = {60, 100, 150};
    const std::uint64_t hold[] = {120'000'000'000ULL, 300'000'000'000ULL};
    const std::uint64_t cool[] = {5'000'000'000ULL, 30'000'000'000ULL};

    constexpr std::size_t min_trades = 30;
    const std::int64_t max_drawdown_allowed = 5'000;   // minor units

    strategy::L1ScalperConfig best{};
    Result best_train{};
    bool found = false;
    std::size_t evaluated = 0, passed = 0;

    for (double m : momentum) for (std::int64_t sp : spread) for (double vm : vol_max)
    for (double qr : qrate) for (std::int64_t s : sl) for (std::int64_t t : tp)
    for (std::uint64_t h : hold) for (std::uint64_t c : cool) {
        strategy::L1ScalperConfig cfg{};
        cfg.entry_momentum = m;
        cfg.max_spread_ticks = sp;
        cfg.max_volatility = vm;
        cfg.min_volatility = 1.0e-7;
        cfg.min_quote_rate = qr;
        cfg.stop_loss_ticks = s;
        cfg.take_profit_ticks = t;
        cfg.max_hold_ns = h;
        cfg.cooldown_ns = c;
        cfg.require_acceleration_agreement = true;
        // max_consecutive_losses left at its default: halts stay armed.
        if (!cfg.valid()) continue;
        ++evaluated;

        const auto r = run(quotes, 0, split, cfg);
        if (r.trades < min_trades) continue;
        if (r.profit_factor <= 1.0) continue;
        if (r.net <= 0) continue;
        if (r.max_dd > max_drawdown_allowed) continue;
        ++passed;
        if (!found || r.net > best_train.net) { found = true; best = cfg; best_train = r; }
    }

    std::cout << "evaluated=" << evaluated << " passed_filters=" << passed << '\n';
    if (!found) {
        std::cout << "NO_CONFIG_PASSED min_trades=" << min_trades
                  << " require: profit_factor>1, net>0, maxdd<=" << max_drawdown_allowed << '\n';
        // Report the best-by-trade-count so the failure is diagnosable.
        strategy::L1ScalperConfig probe{};
        probe.entry_momentum = 1.0e-6; probe.max_spread_ticks = 25;
        probe.max_volatility = 5.0e-5; probe.min_volatility = 1.0e-7;
        probe.min_quote_rate = 0.0; probe.stop_loss_ticks = 30;
        probe.take_profit_ticks = 40; probe.max_hold_ns = 120'000'000'000ULL;
        probe.cooldown_ns = 5'000'000'000ULL;
        const auto r = run(quotes, 0, split, probe);
        std::cout << "LOOSEST_PROBE trades=" << r.trades << " net=" << r.net
                  << " pf=" << r.profit_factor << " maxdd=" << r.max_dd
                  << " win_rate=" << r.win_rate << " halted=" << (r.halted ? 1 : 0)
                  << " commission=" << r.commission << " spread_cost=" << r.spread_cost << '\n';
        return 0;
    }

    const auto validation = run(quotes, split, quotes.size(), best);
    std::cout << "BEST_TRAIN momentum=" << best.entry_momentum
              << " spread<=" << best.max_spread_ticks
              << " vol<=" << best.max_volatility
              << " qrate>=" << best.min_quote_rate
              << " sl=" << best.stop_loss_ticks << " tp=" << best.take_profit_ticks
              << " hold_s=" << (best.max_hold_ns / 1'000'000'000ULL)
              << " cool_s=" << (best.cooldown_ns / 1'000'000'000ULL) << '\n';
    std::cout << "TRAIN trades=" << best_train.trades << " net=" << best_train.net
              << " pf=" << best_train.profit_factor << " win_rate=" << best_train.win_rate
              << " maxdd=" << best_train.max_dd << " commission=" << best_train.commission
              << " spread_cost=" << best_train.spread_cost
              << " halted=" << (best_train.halted ? 1 : 0) << '\n';
    std::cout << "VALIDATION trades=" << validation.trades << " net=" << validation.net
              << " pf=" << validation.profit_factor << " win_rate=" << validation.win_rate
              << " maxdd=" << validation.max_dd << " commission=" << validation.commission
              << " spread_cost=" << validation.spread_cost
              << " halted=" << (validation.halted ? 1 : 0) << '\n';
    const bool val_ok = validation.trades >= min_trades && validation.profit_factor > 1.0 &&
                        validation.net > 0 && validation.max_dd <= max_drawdown_allowed;
    std::cout << "VALIDATION_VERDICT " << (val_ok ? "PASS" : "FAIL") << '\n';
    return 0;
}
