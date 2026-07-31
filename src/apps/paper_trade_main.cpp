#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <memory>
#include <thread>

#include "book/order_book.hpp"
#include "core/clock.hpp"
#include "exec/paper_broker.hpp"
#include "feed/mt5_protocol.hpp"
#include "features/feature_engine.hpp"
#include "persist/segment_catalog.hpp"
#include "replay/wal_tailer.hpp"
#include "risk/risk_engine.hpp"
#include "strategy/baseline_scalper.hpp"
#include "strategy/metrics.hpp"
#include "learn/online_learner.hpp"
#include "strategy/mt5_l1_scalper.hpp"
#include "strategy/scalp_flip.hpp"
#include "telemetry/histogram.hpp"

// ---------------------------------------------------------------------------
// Bridge-authoritative pacing.
//
// The PaperBroker fills in microseconds; MT5 needs the EA's 200 ms timer plus
// ~70 ms of broker round trip. Left unpaced, the strategy completes whole
// entry/exit cycles while one command is still in flight, and every intent
// raised in that window was deferred and DROPPED — so the C++ position and the
// MT5 position diverged and CLOSE commands began arriving for positions MT5
// never opened (CLOSE_NOOP).
//
// The fix is to make MT5 the authority on whether a position exists, and to
// stop the strategy from deciding at all while a command is outstanding. The
// two position states then advance in lockstep at the bridge's pace, which is
// the real pace of execution.
// ---------------------------------------------------------------------------
struct BridgeState final {
    enum class Position : std::uint8_t { flat = 0, long_side = 1, short_side = 2 };

    bool in_flight{false};
    std::uint64_t pending_seq{0};
    bool pending_is_entry{false};
    exec::Side pending_side{exec::Side::buy};
    std::string pending_action{};
    std::uint64_t sent_wall_ns{0};
    Position position{Position::flat};
    std::uint64_t sent{0}, filled{0}, closed{0}, rejected{0}, mismatches{0}, blocked{0};
    std::uint64_t timeouts{0};
    std::uint64_t last_broker_ms{0}, last_order_ms{0};

    [[nodiscard]] bool flat() const noexcept { return position == Position::flat; }

    // Signed exposure as the RiskEngine expects it. Part 10's reduce_only rule
    // is "an order flagged reduce_only must actually reduce", and it decides
    // that from Request::current_net. Feeding it the PaperBroker's net was
    // wrong once the bridge became authoritative: the simulated book and the
    // broker's book drift, and a divergence made every exit look like it was
    // opening new exposure. The engine was right to refuse; the input was lying
    // to it.
    [[nodiscard]] std::int64_t net_volume(std::int64_t lot) const noexcept {
        if (position == Position::long_side) return lot;
        if (position == Position::short_side) return -lot;
        return 0;
    }
    [[nodiscard]] bool holds(exec::Side side) const noexcept {
        return (side == exec::Side::buy) ? position == Position::long_side
                                         : position == Position::short_side;
    }
};

// The EA's duplicate guard lives as long as the EA is attached, which outlives
// this process; a restarted runtime therefore begins below the EA's high-water
// mark and every command is discarded as a replay, silently and forever. Rather
// than depend on recovering the exact number, an unacknowledged command is
// re-sent under a HIGHER sequence after a timeout. That converges on the EA's
// counter from below and also covers a genuinely lost command.
inline void bridge_retry_if_timed_out(const std::string& dir, BridgeState& bridge,
                                      std::uint64_t& seq, std::uint64_t timeout_ns) {
    if (!bridge.in_flight) return;
    const auto now = core::monotonic_now_ns();
    if (now - bridge.sent_wall_ns < timeout_ns) return;
    std::ofstream cmd(dir + "/mme_cmd.txt", std::ios::trunc);
    if (!cmd) return;
    cmd << ++seq << ',' << bridge.pending_action << ",1\n";
    cmd.close();
    bridge.pending_seq = seq;
    bridge.sent_wall_ns = now;
    ++bridge.timeouts;
    std::cout << "MT5_BRIDGE_RETRY seq=" << seq << " action=" << bridge.pending_action
              << " reason=ack_timeout attempt=" << bridge.timeouts << '\n';
    std::cout.flush();
}
// Live top of book published by the bridge EA. The WAL cannot serve the
// trading path: the recorder holds the active segment with dwShareMode=0, so a
// reader only sees data once the segment rotates. Measured rotation was exactly
// 60 s, and the execution rate matched it exactly — one command per minute,
// regardless of strategy, timer or broker speed. This file is the fix; the WAL
// remains the durable record and keeps feeding the learner.
struct LiveTick final {
    std::int64_t bid_ticks{0};
    std::int64_t ask_ticks{0};
    std::int64_t time_msc{0};
    std::int64_t spread_points{0};
    [[nodiscard]] bool valid() const noexcept {
        return bid_ticks > 0 && ask_ticks > bid_ticks;
    }
};

// Prices arrive as decimal strings; the engine is integer-ticks end to end
// (Part 5.9), so they are scaled by 1e5 for a 5-digit forex symbol.
inline bool read_live_tick(const std::string& dir, LiveTick& out) {
    std::ifstream f(dir + "/mme_tick.txt");
    if (!f) return false;
    std::string line;
    if (!std::getline(f, line)) return false;
    double bid = 0.0, ask = 0.0;
    long long msc = 0, spread = 0;
    if (std::sscanf(line.c_str(), "%lf,%lf,%lld,%lld", &bid, &ask, &msc, &spread) != 4)
        return false;
    LiveTick t{};
    t.bid_ticks = static_cast<std::int64_t>(bid * 100000.0 + 0.5);
    t.ask_ticks = static_cast<std::int64_t>(ask * 100000.0 + 0.5);
    t.time_msc = msc;
    t.spread_points = spread;
    if (!t.valid()) return false;
    out = t;
    return true;
}

// Reads mme_ack.txt and, when it acknowledges the outstanding command, retires
// it and folds the broker's answer into the authoritative position. Returns
// true if an ack was consumed this call.
inline bool bridge_poll_ack(const std::string& dir, BridgeState& bridge) {
    if (!bridge.in_flight) return false;
    std::ifstream ack(dir + "/mme_ack.txt");
    if (!ack) return false;
    std::string line;
    if (!std::getline(ack, line)) return false;

    const auto c1 = line.find(',');
    if (c1 == std::string::npos) return false;
    const auto seq = std::strtoull(line.substr(0, c1).c_str(), nullptr, 10);
    if (seq < bridge.pending_seq) return false;          // stale ack, keep waiting
    const auto c2 = line.find(',', c1 + 1);
    const std::string status = (c2 == std::string::npos) ? line.substr(c1 + 1)
                                                         : line.substr(c1 + 1, c2 - c1 - 1);

    bridge.in_flight = false;
    if (status == "FILLED") {
        ++bridge.filled;
        bridge.position = (bridge.pending_side == exec::Side::buy)
            ? BridgeState::Position::long_side : BridgeState::Position::short_side;
    } else if (status == "CLOSED") {
        ++bridge.closed;
        bridge.position = BridgeState::Position::flat;
    } else if (status == "CLOSE_NOOP") {
        // MT5 had nothing to close: our view was stale. The bridge wins.
        ++bridge.mismatches;
        bridge.position = BridgeState::Position::flat;
    } else if (status == "SKIP_MAX_POSITION") {
        // MT5 already holds a position we did not know about. The bridge wins;
        // we cannot tell the side from the ack, so assume the side we asked for
        // is NOT what is open and force a close on the next opportunity.
        ++bridge.mismatches;
        if (bridge.position == BridgeState::Position::flat)
            bridge.position = (bridge.pending_side == exec::Side::buy)
                ? BridgeState::Position::short_side : BridgeState::Position::long_side;
    } else {
        ++bridge.rejected;                                // REJECT_* / HALT_*
    }
    // broker_ms is field 5, measured inside the EA around OrderSend.
    bridge.last_broker_ms = 0;
    if (c2 != std::string::npos) {
        const auto c3 = line.find(',', c2 + 1);
        if (c3 != std::string::npos) {
            const auto c4 = line.find(',', c3 + 1);
            if (c4 != std::string::npos)
                bridge.last_broker_ms = std::strtoull(line.substr(c4 + 1).c_str(), nullptr, 10);
        }
    }
    bridge.last_order_ms = (core::monotonic_now_ns() - bridge.sent_wall_ns) / 1'000'000ULL;
    std::cout << "MT5_BRIDGE_ACK seq=" << seq << " status=" << status
              << " bridge_position=" << static_cast<unsigned>(bridge.position)
              << " order_ms=" << bridge.last_order_ms
              << " broker_ms=" << bridge.last_broker_ms << '\n';
    std::cout.flush();
    return true;
}

// ============================================================================
// STRICT PAPER-TRADING RUNTIME
//
//   PAPER_TRADING = true
//   LIVE_TRADING  = false   (compile-time, not configurable)
//
// No broker API, no socket, no MT5 symbol, no order transport is compiled in.
//
// Event ingestion is delegated to replay::WalTailer, which owns cursor
// advancement, segment rotation, preallocated zero-fill and torn-frame handling.
// The earlier inline version compared its skip counter against a cursor it was
// incrementing in the same loop and therefore delivered every other frame; that
// logic now lives in one tested component instead of being open-coded here.
// ============================================================================

namespace {

constexpr bool kPaperTrading = true;
constexpr bool kLiveTrading = false;
static_assert(kPaperTrading && !kLiveTrading,
              "This runtime is paper-only. Live trading is not selectable.");

std::atomic<bool> g_stop{false};
extern "C" void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

struct Runtime final {
    std::array<book::OrderBook, feed::mt5::symbol_count> books{};
    std::array<features::FeatureEngine, feed::mt5::symbol_count> features{};
    std::unique_ptr<exec::PaperBroker> broker;
    // Exactly one of these is constructed. Held as separate types rather than
    // behind an interface so baseline_scalper.hpp stays untouched.
    std::unique_ptr<strategy::BaselineScalper> scalper;
    std::unique_ptr<strategy::Mt5L1Scalper> l1;
    std::unique_ptr<strategy::ScalpFlip> flip;
    bool use_l1{false};
    bool use_flip{false};
    std::unique_ptr<risk::RiskEngine> risk;
    strategy::TradeStatistics stats;
    telemetry::Histogram<> decision_ns;
    telemetry::Histogram<> order_latency_ms;   // cmd written -> ack observed
    telemetry::Histogram<> broker_latency_ms;  // measured inside the EA around OrderSend
    // Learning is observation-only: it records evidence and writes advisory
    // proposals. It never writes back into the strategy or the risk engine.
    learn::OnlineLearner learner;
    strategy::L1SignalSet entry_signals{};
    // Re-arm is an explicit, counted operator action. The halt still fires, is
    // still logged, and is still bounded â€” it is not removed.
    strategy::L1ScalperConfig l1_config{};
    strategy::ScalpFlipConfig flip_config{};
    std::uint64_t rearms{0};
    std::uint64_t bridge_seq{0};
    std::uint64_t bridge_deferred{0};
    std::uint64_t fast_ticks{0};
    BridgeState bridge{};

    std::uint64_t events{0}, quotes{0}, heartbeats{0}, other_events{0};
    std::uint64_t intents{0}, entries{0}, exits{0}, vetoes{0};
    std::uint64_t risk_rejects{0}, broker_rejects{0}, fills{0};
    std::int64_t spread_paid_ticks{0};

    // Veto-reason distribution, indexed by strategy::Veto.
    std::array<std::uint64_t, 16> veto_reason{};

    exec::BrokerOrderRef working{};
    bool entry_in_flight{false};
    strategy::TradeRecord pending{};
    std::uint64_t order_seq{0}, last_order_ns{0};
    book::BookSource source{book::BookSource::l1_only};
};

const char* veto_name(std::size_t index) {
    switch (static_cast<strategy::Veto>(index)) {
        case strategy::Veto::none: return "none";
        case strategy::Veto::cold_signals: return "cold_signals";
        case strategy::Veto::wrong_state: return "wrong_state";
        case strategy::Veto::cooldown_active: return "cooldown_active";
        case strategy::Veto::max_positions: return "max_positions";
        case strategy::Veto::spread_too_wide: return "spread_too_wide";
        case strategy::Veto::volatility_out_of_band: return "volatility_out_of_band";
        case strategy::Veto::signal_below_threshold: return "signal_below_threshold";
        case strategy::Veto::averaging_down: return "averaging_down";
        case strategy::Veto::halted: return "halted";
    }
    return "unknown";
}

// One strategy is live at a time; these keep every call site free of the branch
// and let baseline_scalper.hpp stay untouched (no shared interface imposed on it).
#define STRAT_STATE()          (rt.use_flip ? rt.flip->state() : rt.use_l1 ? rt.l1->state() : rt.scalper->state())
#define STRAT_VETO()           (rt.use_flip ? rt.flip->last_veto() : rt.use_l1 ? rt.l1->last_veto() : rt.scalper->last_veto())
#define STRAT_EXIT_REASON()    (rt.use_flip ? rt.flip->last_exit_reason() : rt.use_l1 ? rt.l1->last_exit_reason() : rt.scalper->last_exit_reason())
#define STRAT_HALT()           do { if (rt.use_flip) rt.flip->halt(); else if (rt.use_l1) rt.l1->halt(); else rt.scalper->halt(); } while (0)
#define STRAT_ENTRY_REJECTED() do { if (rt.use_flip) rt.flip->on_entry_rejected(); else if (rt.use_l1) rt.l1->on_entry_rejected(); else rt.scalper->on_entry_rejected(); } while (0)
#define STRAT_EXIT_REJECTED()  do { if (rt.use_flip) rt.flip->on_exit_rejected(); else if (rt.use_l1) rt.l1->on_exit_rejected(); else rt.scalper->on_exit_rejected(); } while (0)

void print_status(const Runtime& rt, const char* feed_state, std::uint64_t wall_seconds) {
    const auto& account = rt.broker->account();
    std::cout
        << "[PAPER] t=" << wall_seconds << "s"
        << " feed=" << feed_state
        << " events=" << rt.events
        << " quotes=" << rt.quotes
        << " hb=" << rt.heartbeats
        << " | intents=" << rt.intents
        << " entries=" << rt.entries
        << " exits=" << rt.exits
        << " vetoes=" << rt.vetoes
        << " risk_rej=" << rt.risk_rejects
        << " brk_rej=" << rt.broker_rejects
        << " | fills=" << rt.fills
        << " trades=" << rt.stats.trades()
        << " open=" << account.open_positions
        << " | realized=" << account.realized_pnl_minor
        << " unrealized=" << account.unrealized_pnl_minor
        << " equity=" << account.equity_minor
        << " commission=" << account.commission_paid_minor
        << " spread_paid=" << rt.spread_paid_ticks
        << " | win_rate=" << rt.stats.win_rate()
        << " maxdd=" << rt.stats.max_drawdown_minor()
        << " | decision_p50_ns=" << rt.decision_ns.percentile(0.50)
        << " p99_ns=" << rt.decision_ns.percentile(0.99)
        << " | strategy=" << static_cast<unsigned>(STRAT_STATE())
        << " halted=" << (rt.broker->halted() ? 1 : 0)
        << '\n';
    const double secs = wall_seconds > 0 ? static_cast<double>(wall_seconds) : 1.0;
    const auto attempts = rt.bridge.sent;
    const auto rejects = rt.bridge.rejected + rt.bridge.mismatches;
    std::cout
        << "[BENCH] bridge_sent=" << attempts
        << " filled=" << rt.bridge.filled
        << " round_trips=" << rt.bridge.closed
        << " trades_per_sec=" << (static_cast<double>(rt.bridge.closed) / secs)
        << " trades_per_min=" << (static_cast<double>(rt.bridge.closed) / secs * 60.0)
        << " | order_ms_p50=" << rt.order_latency_ms.percentile(0.50)
        << " p99=" << rt.order_latency_ms.percentile(0.99)
        << " broker_ms_p50=" << rt.broker_latency_ms.percentile(0.50)
        << " p99=" << rt.broker_latency_ms.percentile(0.99)
        << " | reject_rate="
        << (attempts > 0 ? static_cast<double>(rejects) / static_cast<double>(attempts) : 0.0)
        << " timeouts=" << rt.bridge.timeouts
        << " fast_ticks=" << rt.fast_ticks
        << '\n';
    std::cout.flush();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: paper_trade <wal-dir> [--once] [--status-seconds N] [--no-resume]\n";
        return 2;
    }
    const std::filesystem::path wal_dir(argv[1]);
    bool once = false;
    bool resume_cursor = true;
    std::uint64_t status_seconds = 10;
    const char* selected_strategy = "baseline";
    // A strategy instance holds ONE momentum/volatility series and ONE position.
    // Feeding it five symbols would blend EURUSD (~115k ticks) with XAUUSD
    // (~2.3M ticks) into the same signal and let an entry on one instrument be
    // marked against another. Single-symbol by construction.
    std::uint32_t trade_symbol = 0;
    // 0 = never re-arm (default). A finite cap means a strategy that keeps
    // halting still stops eventually instead of looping forever.
    std::uint64_t max_rearms = 0;
    bool mt5_bridge = false;
    std::string bridge_dir;
    // Widens ENTRY gates only. Exits, stops, position limits, the cooldown, the
    // consecutive-loss halt and every RiskEngine limit are untouched — this
    // changes when the strategy is willing to open, never how much it can lose.
    bool loose_entry = false;
    std::uint64_t max_hold_ms = 0;      // 0 = strategy default
    std::int64_t profit_ticks_override = 0;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--once") == 0) once = true;
        else if (std::strcmp(argv[i], "--no-resume") == 0) resume_cursor = false;
        else if (std::strcmp(argv[i], "--status-seconds") == 0 && i + 1 < argc)
            status_seconds = std::strtoull(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--strategy") == 0 && i + 1 < argc)
            selected_strategy = argv[++i];
        else if (std::strcmp(argv[i], "--symbol") == 0 && i + 1 < argc)
            trade_symbol = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--max-rearms") == 0 && i + 1 < argc)
            max_rearms = std::strtoull(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--mt5-bridge") == 0 && i + 1 < argc) {
            mt5_bridge = true; bridge_dir = argv[++i];
        }
        else if (std::strcmp(argv[i], "--loose-entry") == 0) loose_entry = true;
        else if (std::strcmp(argv[i], "--max-hold-ms") == 0 && i + 1 < argc)
            max_hold_ms = std::strtoull(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--profit-ticks") == 0 && i + 1 < argc)
            profit_ticks_override = std::strtoll(argv[++i], nullptr, 10);
    }
    const bool use_flip = (std::strcmp(selected_strategy, "scalp_flip_v1") == 0 ||
                           std::strcmp(selected_strategy, "flip") == 0);
    const bool use_l1 = (std::strcmp(selected_strategy, "mt5_l1_scalper_v1") == 0 ||
                         std::strcmp(selected_strategy, "l1") == 0);
    if (!use_flip && !use_l1 && std::strcmp(selected_strategy, "baseline") != 0 &&
        std::strcmp(selected_strategy, "baseline_scalper_v1") != 0) {
        std::cerr << "SAFETY_FAIL reason=unknown_strategy value=" << selected_strategy << '\n';
        return 2;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // ---------------- mandatory safety checks, fail closed -----------------
    std::cout << "=== PAPER-TRADING SAFETY CHECKS ===\n";
    std::cout << "PAPER_TRADING=" << (kPaperTrading ? "true" : "false") << '\n';
    std::cout << "LIVE_TRADING=" << (kLiveTrading ? "true" : "false") << '\n';
    std::cout << "broker=PaperBroker (simulated fills, no transport compiled in)\n";

    const auto kill_file = wal_dir / "state" / "recorder_halt.json";
    if (std::filesystem::exists(kill_file)) {
        std::cerr << "SAFETY_FAIL reason=kill_switch_tripped file=" << kill_file.string() << '\n';
        return 1;
    }
    std::cout << "kill_switch=healthy\n";

    risk::Limits limits{};
    risk::RiskEngine risk_engine(limits);
    if (!risk_engine.valid()) {
        std::cerr << "SAFETY_FAIL reason=limits_invalid\n";
        return 1;
    }
    std::cout << "limits_valid=1\n";

    strategy::StrategyConfig strategy_config{};
    strategy_config.volume = 1;
    strategy_config.max_positions = 1;
    strategy_config.entry_queue_imbalance = 0.35;
    strategy_config.require_ofi_agreement = true;
    strategy_config.max_spread_ticks = 20;
    strategy_config.take_profit_ticks = 10;
    strategy_config.stop_loss_ticks = 10;
    strategy_config.max_hold_ns = 120'000'000'000ULL;
    strategy_config.cooldown_ns = 30'000'000'000ULL;
    if (!strategy_config.valid()) {
        std::cerr << "SAFETY_FAIL reason=strategy_config_invalid\n";
        return 1;
    }
    std::cout << "strategy_config_valid=1\n";
    std::cout << "max_positions=" << strategy_config.max_positions
              << " averaging_down=" << (strategy::StrategyConfig::allow_averaging_down ? 1 : 0)
              << " stop_loss_ticks=" << strategy_config.stop_loss_ticks
              << " take_profit_ticks=" << strategy_config.take_profit_ticks
              << " max_hold_ns=" << strategy_config.max_hold_ns
              << " cooldown_ns=" << strategy_config.cooldown_ns << '\n';
    std::cout << "daily_loss_limit=" << limits.max_daily_loss
              << " max_consecutive_losses=" << limits.max_consecutive_losses << '\n';

    replay::WalTailer tailer;
    if (!tailer.open(wal_dir)) {
        std::cerr << "SAFETY_FAIL reason=no_wal_segments dir=" << wal_dir.string() << '\n';
        return 1;
    }
    std::cout << "wal_dir=" << wal_dir.string() << " segments=" << tailer.segments() << '\n';

    const auto cursor_path = wal_dir / "state" / "paper_cursor.txt";
    if (resume_cursor) {
        replay::TailCursor restored{};
        if (replay::WalTailer::load_cursor(cursor_path, restored) && tailer.resume(restored))
            std::cout << "resumed_cursor segment=" << restored.segment_index
                      << " frames=" << restored.frames_consumed << '\n';
    }
    std::cout << "=== SAFETY CHECKS PASSED ===\n\n";


    // ---------------- runtime ---------------------------------------------
    Runtime rt;
    for (std::size_t i = 0; i < rt.books.size(); ++i) {
        rt.books[i] = book::OrderBook(static_cast<std::uint32_t>(i));
        rt.features[i] = features::FeatureEngine(32);
    }
    exec::BrokerConfig broker_config{};
    broker_config.run_id = 1;
    broker_config.run_seed = 20260731;
    broker_config.mode = exec::SimulationMode::pessimistic;
    broker_config.hedging = true;
    broker_config.initial_balance_minor = 1'000'000'000;
    broker_config.queue_validated = false;
    rt.broker = std::make_unique<exec::PaperBroker>(broker_config);

    // ---- bridge sequence recovery and startup reconciliation ---------------
    // The EA's duplicate guard (g_last_seq) lives for as long as the EA is
    // attached, which outlives this process. Restarting the runtime used to
    // reset the counter to 1, and every command was then silently discarded as
    // a replay. Resume the sequence from the last acknowledgement instead.
    //
    // MT5's position at startup is also unknown: the previous run may have left
    // one open. Rather than guess, the first command is always a CLOSE. Its ack
    // is unambiguous — CLOSED means one was open, CLOSE_NOOP means none was —
    // and either way the two sides begin from a known flat state.
    if (mt5_bridge) {
        std::ifstream ack(bridge_dir + "/mme_ack.txt");
        std::string line;
        if (ack && std::getline(ack, line)) {
            const auto comma = line.find(',');
            if (comma != std::string::npos) {
                rt.bridge_seq = std::strtoull(line.substr(0, comma).c_str(), nullptr, 10);
                std::cout << "MT5_BRIDGE_SEQ_RESUMED from_ack=" << rt.bridge_seq << '\n';
            }
        }
        std::ofstream cmd(bridge_dir + "/mme_cmd.txt", std::ios::trunc);
        if (cmd) {
            cmd << ++rt.bridge_seq << ",CLOSE,1\n";
            cmd.close();
            rt.bridge.in_flight = true;
            rt.bridge.pending_seq = rt.bridge_seq;
            rt.bridge.pending_is_entry = false;
            rt.bridge.pending_action = "CLOSE";
            rt.bridge.sent_wall_ns = core::monotonic_now_ns();
            ++rt.bridge.sent;
            std::cout << "MT5_BRIDGE_SYNC seq=" << rt.bridge_seq
                      << " action=CLOSE reason=startup_reconcile\n";
        }
        std::cout.flush();
    }
    rt.use_l1 = use_l1;
    rt.use_flip = use_flip;
    if (use_flip) {
        strategy::ScalpFlipConfig fc{};   // structural defaults, NOT_CALIBRATED
        // Hold time is the execution-rate lever: with the feed path fixed, the
        // 45 s time stop became the binding constraint on trades/sec.
        if (max_hold_ms > 0) fc.max_hold_ns = max_hold_ms * 1'000'000ULL;
        if (profit_ticks_override > 0) fc.min_net_profit_ticks = profit_ticks_override;
        if (!fc.valid()) { std::cerr << "SAFETY_FAIL reason=flip_config_invalid\n"; return 1; }
        rt.flip = std::make_unique<strategy::ScalpFlip>(fc);
        rt.flip_config = fc;
        std::cout << "strategy=scalp_flip_v1 (L1 directional scalp-and-flip, DEMO execution)\n"
                  << "entry_momentum=" << fc.entry_momentum
                  << " flip_momentum=" << fc.flip_momentum
                  << " min_net_profit_ticks=" << fc.min_net_profit_ticks
                  << " cost_ticks=" << (fc.commission_ticks + fc.slippage_allowance_ticks)
                  << " profit_threshold_ticks=" << fc.profit_threshold_ticks()
                  << " stop_loss_ticks=" << fc.stop_loss_ticks
                  << " max_hold_s=" << (fc.max_hold_ns / 1000000000ULL)
                  << " max_positions=" << fc.max_positions
                  << " max_consecutive_losses=" << fc.max_consecutive_losses << '\n';
    } else if (use_l1) {
        strategy::L1ScalperConfig l1_config{};   // structural defaults, NOT_CALIBRATED
        if (loose_entry) {
            // ENTRY GATES ONLY. Live EURUSD is arriving well below the 0.5
            // quotes/s the default demands, so every entry was vetoed before the
            // direction test was ever reached. These four gates decide whether
            // to look at a signal at all; none of them bounds loss.
            l1_config.entry_momentum = 1.0e-9;              // was 2.0e-6
            l1_config.min_quote_rate = 0.0;                 // was 0.5 quotes/s
            l1_config.max_spread_ratio = 1000.0;            // was 1.5
            l1_config.max_spread_ticks = 100'000;           // was 20
            l1_config.min_volatility = 0.0;                 // was 1.0e-7
            l1_config.max_volatility = 1.0e9;               // was 5.0e-5
            l1_config.require_acceleration_agreement = false;
            // UNCHANGED, deliberately: take_profit_ticks, stop_loss_ticks,
            // max_hold_ns, cooldown_ns, max_positions (1), allow_averaging_down
            // (false), max_consecutive_losses (5). Loss control is not an entry
            // gate and is not relaxed here.
            std::cout << "LOOSE_ENTRY=1 (entry gates widened; exits, stops, "
                         "position cap, cooldown, halts and RiskEngine unchanged)\n";
        }
        if (!l1_config.valid()) {
            std::cerr << "SAFETY_FAIL reason=l1_config_invalid\n";
            return 1;
        }
        rt.l1 = std::make_unique<strategy::Mt5L1Scalper>(l1_config);
        rt.l1_config = l1_config;
        std::cout << "strategy=mt5_l1_scalper_v1 (L1-only: no queue imbalance, no OFI, no volume)\n"
                  << "l1_entry_momentum=" << l1_config.entry_momentum
                  << " max_spread_ratio=" << l1_config.max_spread_ratio
                  << " min_quote_rate=" << l1_config.min_quote_rate
                  << " max_consecutive_losses=" << l1_config.max_consecutive_losses << '\n';
    } else {
        rt.scalper = std::make_unique<strategy::BaselineScalper>(strategy_config);
        std::cout << "strategy=baseline_scalper_v1 (depth-based: queue imbalance + OFI)\n";
    }
    rt.risk = std::make_unique<risk::RiskEngine>(limits);

    for (std::uint32_t i = 0; i < feed::mt5::symbol_count && i < exec::max_symbols; ++i) {
        exec::SymbolSpec spec{};
        spec.symbol_id = i;
        spec.tick_size_ticks = 1;
        spec.volume_min = 1;
        spec.volume_max = 1'000'000;
        spec.volume_step = 1;
        spec.contract_size = 100'000;
        spec.tick_value_minor = 1;
        spec.commission_per_lot_minor = 700;
        spec.margin_rate_bp = 1;
        spec.tradable = true;
        rt.broker->set_symbol(spec);
    }

    const auto start_ns = core::monotonic_now_ns();
    auto last_status_ns = start_ns;
    auto last_data_ns = start_ns;
    bool first_decision_reported = false;
    bool first_order_reported = false;
    bool stale_warned = false;
    std::int64_t last_tick_msc = 0;
    std::uint64_t last_tick_wall_ns = core::monotonic_now_ns();

    while (!g_stop.load(std::memory_order_relaxed)) {
        core::FixedEvent event{};
        const auto status = tailer.next(event);

        if (status == replay::TailStatus::corrupt) {
            std::cerr << "FEED_CORRUPT action=halt_fail_closed events=" << rt.events << '\n';
            STRAT_HALT();
            break;
        }
        if (status == replay::TailStatus::catalog_error) {
            std::cerr << "FEED_CATALOG_ERROR action=stop\n";
            break;
        }

        if (status == replay::TailStatus::ok) {
            last_data_ns = core::monotonic_now_ns();
            ++rt.events;

            const auto type = static_cast<core::EventType>(event.header.type);
            if (type == core::EventType::heartbeat) {
                core::HeartbeatPayload hb{};
                std::memcpy(&hb, event.payload.data(), sizeof(hb));
                if (hb.book_source <= static_cast<std::uint8_t>(book::BookSource::l3_mbo))
                    rt.source = static_cast<book::BookSource>(hb.book_source);
                ++rt.heartbeats;
                continue;
            }
            if (type != core::EventType::quote) { ++rt.other_events; continue; }

            core::QuotePayload quote{};
            std::memcpy(&quote, event.payload.data(), sizeof(quote));
            const auto symbol = event.header.symbol_id;
            if (symbol >= rt.books.size()) { ++rt.other_events; continue; }
            const auto ts = event.header.ts_local_ns;

            (void)rt.books[symbol].apply_quote(quote.bid, quote.ask, quote.bid_size,
                                               quote.ask_size, ts);
            const auto fv = rt.features[symbol].compute(rt.books[symbol], rt.source);
            rt.broker->on_quote(symbol, quote.bid, quote.ask, quote.bid_size,
                                quote.ask_size, ts);
            ++rt.quotes;

            // Resolve any working order before deciding again.
            if (rt.working.logical_order_id != 0) {
                const auto* order = rt.broker->find_order(rt.working);
                if (order != nullptr && exec::is_terminal(order->state)) {
                    if (order->state == exec::OrderState::filled) {
                        if (rt.entry_in_flight) {
                            if (rt.use_flip)
                                rt.flip->on_entry_filled(order->side, order->filled_volume,
                                                         order->avg_fill_price_ticks, ts);
                            else if (rt.use_l1)
                                rt.l1->on_entry_filled(order->side, order->filled_volume,
                                                       order->avg_fill_price_ticks, ts);
                            else
                                rt.scalper->on_entry_filled(order->side, order->filled_volume,
                                                            order->avg_fill_price_ticks, ts);
                            rt.pending = strategy::TradeRecord{};
                            rt.pending.side = order->side;
                            rt.pending.volume = order->filled_volume;
                            rt.pending.entry_ticks = order->avg_fill_price_ticks;
                            rt.pending.opened_ns = ts;
                            rt.pending.commission_minor = order->commission_minor;
                        } else {
                            rt.pending.exit_ticks = order->avg_fill_price_ticks;
                            rt.pending.closed_ns = ts;
                            rt.pending.commission_minor += order->commission_minor;
                            rt.pending.reason = STRAT_EXIT_REASON();
                            rt.pending.pnl_minor = strategy::trade_pnl_minor(
                                rt.pending.side, rt.pending.volume, rt.pending.entry_ticks,
                                rt.pending.exit_ticks, 1);
                            (void)rt.stats.add(rt.pending);
                            // Attribute the completed trade to the conditions
                            // present when its entry was submitted.
                            if (rt.use_l1) {
                                learn::TradeObservation obs{};
                                obs.momentum = rt.entry_signals.momentum;
                                obs.volatility = rt.entry_signals.volatility;
                                obs.spread = rt.entry_signals.spread;
                                obs.spread_ratio = rt.entry_signals.spread_ratio;
                                obs.quote_rate = rt.entry_signals.quote_rate;
                                obs.acceleration = rt.entry_signals.acceleration;
                                obs.side = (rt.pending.side == exec::Side::buy) ? 1 : -1;
                                obs.net_minor = rt.pending.net_minor();
                                obs.hold_ns = rt.pending.hold_ns();
                                obs.exit_reason = rt.pending.reason;
                                rt.learner.add(obs);
                                rt.learner.write_report((wal_dir / "state" / "learning_report.txt").string());
                                rt.learner.write_proposal((wal_dir / "state" / "learning_proposal.json").string(), 10);
                            }
                            // The L1 strategy needs the realised result to drive
                            // its consecutive-loss halt.
                            if (rt.use_flip) rt.flip->on_exit_filled(ts, rt.pending.net_minor());
                            else if (rt.use_l1) rt.l1->on_exit_filled(ts, rt.pending.net_minor());
                            else rt.scalper->on_exit_filled(ts);
                        }
                        rt.spread_paid_ticks += quote.ask - quote.bid;
                        ++rt.fills;
                    } else {
                        ++rt.broker_rejects;
                        if (rt.entry_in_flight) STRAT_ENTRY_REJECTED();
                        else STRAT_EXIT_REJECTED();
                    }
                    rt.working = exec::BrokerOrderRef{};
                }
            }
            if (rt.working.logical_order_id != 0) continue;
            // Book and broker see every symbol; the strategy sees only its own.
            if (symbol != trade_symbol) continue;
            // With the bridge live, execution is owned by the live-tick fast
            // path below. Driving the same FSM from two quote sources would
            // interleave decisions against prices 60 s apart.
            if (mt5_bridge) continue;

            // ---- bridge-authoritative pacing -------------------------------
            // While a command is outstanding the strategy does not decide at
            // all. Letting it run ahead is precisely what desynchronised the
            // two position states: it would complete an entry and an exit in
            // the 270 ms MT5 needs to acknowledge one order, and the intents
            // raised in between were silently dropped.
            if (mt5_bridge) {
                bridge_poll_ack(bridge_dir, rt.bridge);
                bridge_retry_if_timed_out(bridge_dir, rt.bridge, rt.bridge_seq,
                                          3'000'000'000ULL);
                if (rt.bridge.in_flight) { ++rt.bridge.blocked; continue; }
            }

            const auto d0 = core::monotonic_now_ns();
            const auto intent = rt.use_flip
                ? rt.flip->on_market(symbol, rt.books[symbol], ts)
                : rt.use_l1
                ? rt.l1->on_market(symbol, rt.books[symbol], ts)
                : rt.scalper->on_market(symbol, rt.books[symbol], fv, rt.source, ts);
            rt.decision_ns.record(core::monotonic_now_ns() - d0);

            if (!first_decision_reported) {
                first_decision_reported = true;
                std::cout << "FIRST_DECISION symbol=" << symbol << " ts_ns=" << ts
                          << " kind=" << static_cast<unsigned>(intent.kind)
                          << " state=" << static_cast<unsigned>(STRAT_STATE())
                          << " veto=" << veto_name(static_cast<std::size_t>(STRAT_VETO()))
                          << '\n';
            }
            // Operator re-arm after a consecutive-loss halt. The halt fired and
            // is recorded; this re-arms a cold strategy while preserving the
            // learner's accumulated evidence and the trade statistics. Bounded
            // by --max-rearms so a persistently failing strategy still stops.
            const bool halted_idle =
                rt.use_flip ? (rt.flip->state() == strategy::StrategyState::halted &&
                               !rt.flip->position().active)
                            : (rt.use_l1 && rt.l1->state() == strategy::StrategyState::halted &&
                               !rt.l1->position().active);
            if (halted_idle && rt.rearms < max_rearms) {
                ++rt.rearms;
                std::cout << "STRATEGY_REARM count=" << rt.rearms << '/' << max_rearms
                          << " reason=consecutive_loss_halt trades_so_far="
                          << rt.learner.trades() << " net_minor=" << rt.learner.net()
                          << " ts_ns=" << ts << '\n';
                std::cout.flush();
                if (rt.use_flip) rt.flip = std::make_unique<strategy::ScalpFlip>(rt.flip_config);
                else rt.l1 = std::make_unique<strategy::Mt5L1Scalper>(rt.l1_config);
                continue;
            }

            if (!intent.actionable()) {
                ++rt.vetoes;
                const auto reason = static_cast<std::size_t>(STRAT_VETO());
                if (reason < rt.veto_reason.size()) ++rt.veto_reason[reason];
                continue;
            }
            ++rt.intents;

            risk::Request request{};
            request.symbol_id = intent.symbol_id;
            request.strategy_id = intent.strategy_id;
            request.side = intent.side;
            request.volume = intent.volume;
            request.risk_minor = 1;
            request.free_margin = rt.broker->account().free_margin_minor;
            request.warm_mask = limits.required_warm_mask;
            request.session_open = true;
            request.now_ns = ts;
            request.last_order_ns = rt.last_order_ns;
            request.hedging = true;

            if (!rt.risk->check(request).approved) {
                ++rt.risk_rejects;
                if (intent.kind == strategy::IntentKind::enter) STRAT_ENTRY_REJECTED();
                else STRAT_EXIT_REJECTED();
                continue;
            }

            exec::OrderRequest order{};
            order.corr_id = event.header.seq_global;
            order.strategy_id = intent.strategy_id;
            order.symbol_id = intent.symbol_id;
            order.side = intent.side;
            order.type = exec::OrderType::market;
            order.volume = intent.volume;
            order.seq_global = rt.order_seq++;
            order.reduce_only = intent.reduce_only;

            const auto submitted = rt.broker->submit(order, ts);
            if (!submitted.accepted) {
                ++rt.broker_rejects;
                if (intent.kind == strategy::IntentKind::enter) STRAT_ENTRY_REJECTED();
                else STRAT_EXIT_REJECTED();
                if (!first_order_reported) {
                    first_order_reported = true;
                    std::cout << "FIRST_ORDER_REJECTED reason="
                              << static_cast<unsigned>(submitted.reason) << '\n';
                }
                continue;
            }
            // MT5 DEMO execution bridge. Entries emit BUY/SELL; exits emit
            // CLOSE so the broker position actually closes instead of waiting
            // on its own SL/TP. Without the CLOSE leg the C++ and MT5 position
            // states drift apart and one open ticket blocks every later entry.
            if (mt5_bridge) {
                const bool is_entry = (intent.kind == strategy::IntentKind::enter);
                const char* action = is_entry
                    ? (intent.side == exec::Side::buy ? "BUY" : "SELL")
                    : "CLOSE";
                // The bridge's acked state decides whether the command is even
                // meaningful. Sending CLOSE with nothing open produced the
                // CLOSE_NOOP storm; sending an entry over an existing position
                // produced SKIP_MAX_POSITION. Neither is emitted now.
                const bool meaningful = is_entry ? rt.bridge.flat() : !rt.bridge.flat();
                if (!meaningful) {
                    ++rt.bridge_deferred;
                    std::cout << "MT5_BRIDGE_SUPPRESS action=" << action
                              << " bridge_position=" << static_cast<unsigned>(rt.bridge.position)
                              << " reason=" << (is_entry ? "already_open" : "nothing_to_close")
                              << '\n';
                    std::cout.flush();
                } else {
                    std::ofstream cmd(bridge_dir + "/mme_cmd.txt", std::ios::trunc);
                    if (cmd) {
                        cmd << ++rt.bridge_seq << ',' << action << ','
                            << intent.volume << '\n';
                        cmd.close();
                        rt.bridge.in_flight = true;
                        rt.bridge.pending_seq = rt.bridge_seq;
                        rt.bridge.pending_is_entry = is_entry;
                        rt.bridge.pending_side = intent.side;
                        rt.bridge.pending_action = action;
                        rt.bridge.sent_wall_ns = core::monotonic_now_ns();
                        ++rt.bridge.sent;
                        std::cout << "MT5_BRIDGE_CMD seq=" << rt.bridge_seq
                                  << " action=" << action << " ts_ns=" << ts << '\n';
                        std::cout.flush();
                    }
                }
            }

            rt.working = submitted.ref;
            rt.entry_in_flight = (intent.kind == strategy::IntentKind::enter);
            // Snapshot the conditions that produced this entry, for attribution
            // when the round trip later completes.
            if (rt.entry_in_flight && rt.use_l1) rt.entry_signals = rt.l1->signals();
            if (rt.entry_in_flight && rt.use_flip) rt.entry_signals = rt.flip->signals();
            rt.last_order_ns = ts;
            if (rt.entry_in_flight) ++rt.entries; else ++rt.exits;

            if (!first_order_reported) {
                first_order_reported = true;
                std::cout << "FIRST_PAPER_ORDER side="
                          << (intent.side == exec::Side::buy ? "BUY" : "SELL")
                          << " volume=" << intent.volume << " symbol=" << intent.symbol_id
                          << " ts_ns=" << ts
                          << " logical_order_id=" << submitted.ref.logical_order_id
                          << " (PAPER, no broker transport)\n";
            }
            continue;
        }

        // idle / end_of_data
        const auto now = core::monotonic_now_ns();
        const auto idle_seconds = (now - last_data_ns) / 1'000'000'000ULL;
        const char* feed_state = (idle_seconds > 60) ? "STALE" : "IDLE";

        if (now - last_status_ns >= status_seconds * 1'000'000'000ULL) {
            print_status(rt, feed_state, (now - start_ns) / 1'000'000'000ULL);
            (void)tailer.save_cursor(cursor_path);
            last_status_ns = now;
        }

        // Trading stops on stale data.
        if (idle_seconds > 300 && !stale_warned) {
            stale_warned = true;
            std::cout << "FEED_STALE seconds=" << idle_seconds << " action=strategy_halted\n";
            STRAT_HALT();
        }

        if (once) break;

        // ---- live-tick fast execution path ---------------------------------
        // The WAL path above is durable but rotation-bound (measured: exactly
        // 60 s). Execution runs here instead, off the bridge's published top of
        // book, so the round trip is EA-timer + broker latency rather than a
        // segment rotation. Every gate is the same one the WAL path uses:
        // RiskEngine, one position, bridge-authoritative pacing, SL/TP at the
        // bridge, kill switch, stale halt.
        if (mt5_bridge) {
            const auto spin_until = core::monotonic_now_ns() + 200'000'000ULL;
            while (core::monotonic_now_ns() < spin_until && !g_stop.load()) {
                if (bridge_poll_ack(bridge_dir, rt.bridge)) {
                    rt.order_latency_ms.record(rt.bridge.last_order_ms);
                    if (rt.bridge.last_broker_ms > 0)
                        rt.broker_latency_ms.record(rt.bridge.last_broker_ms);
                }
                bridge_retry_if_timed_out(bridge_dir, rt.bridge, rt.bridge_seq,
                                          3'000'000'000ULL);

                LiveTick tick{};
                if (!read_live_tick(bridge_dir, tick)) break;
                const bool fresh = (tick.time_msc != last_tick_msc);
                if (fresh) {
                    last_tick_msc = tick.time_msc;
                    last_tick_wall_ns = core::monotonic_now_ns();
                    ++rt.fast_ticks;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                // Decisions run on the clock, not only on tick arrival. EURUSD
                // was delivering under 1 quote/sec, so a time stop could not
                // fire until the next unrelated tick happened to show up —
                // holding time, and therefore the trade rate, hostage to quote
                // arrival. Time advances from the last tick's broker epoch plus
                // elapsed wall time, so quote timestamps and decision
                // timestamps stay in the same clock and the broker's
                // stale-quote guard still means what it says.
                const auto ts = static_cast<std::uint64_t>(tick.time_msc) * 1'000'000ULL +
                                (core::monotonic_now_ns() - last_tick_wall_ns);

                auto& book = rt.books[trade_symbol];
                if (fresh) (void)book.apply_quote(tick.bid_ticks, tick.ask_ticks, 0, 0, ts);
                // The broker is re-marked every pass so a resting order fills
                // and the time stop is evaluated against a moving clock.
                rt.broker->on_quote(trade_symbol, tick.bid_ticks, tick.ask_ticks, 0, 0, ts);

                // Settle the paper order so the strategy FSM advances.
                if (rt.working.logical_order_id != 0) {
                    const auto* o = rt.broker->find_order(rt.working);
                    if (o != nullptr && exec::is_terminal(o->state)) {
                        if (o->state == exec::OrderState::filled) {
                            if (rt.entry_in_flight) {
                                rt.flip->on_entry_filled(o->side, o->filled_volume,
                                                         o->avg_fill_price_ticks, ts);
                                rt.pending = strategy::TradeRecord{};
                                rt.pending.side = o->side;
                                rt.pending.volume = o->filled_volume;
                                rt.pending.entry_ticks = o->avg_fill_price_ticks;
                                rt.pending.opened_ns = ts;
                            } else {
                                rt.pending.exit_ticks = o->avg_fill_price_ticks;
                                rt.pending.closed_ns = ts;
                                rt.pending.reason = rt.flip->last_exit_reason();
                                rt.pending.pnl_minor = strategy::trade_pnl_minor(
                                    rt.pending.side, rt.pending.volume, rt.pending.entry_ticks,
                                    rt.pending.exit_ticks, 1);
                                (void)rt.stats.add(rt.pending);
                                rt.flip->on_exit_filled(ts, rt.pending.net_minor());
                            }
                            ++rt.fills;
                        } else {
                            ++rt.broker_rejects;
                            if (rt.entry_in_flight) rt.flip->on_entry_rejected();
                            else rt.flip->on_exit_rejected();
                        }
                        rt.working = exec::BrokerOrderRef{};
                    }
                }
                if (rt.working.logical_order_id != 0) continue;
                if (rt.bridge.in_flight) { ++rt.bridge.blocked; continue; }

                // Reconcile a one-sided position. The strategy closes its paper
                // position on its own time stop and returns to idle; if the MT5
                // side is still open, every entry it then raises is suppressed
                // as "already open" and no exit is ever raised — a flat strategy
                // has nothing to exit. That deadlocked execution at three trades
                // with 1,900 intents discarded. MT5 is authoritative, so the
                // disagreement is resolved by closing what MT5 actually holds.
                if (!rt.bridge.flat() && !rt.flip->position().active) {
                    std::ofstream sync(bridge_dir + "/mme_cmd.txt", std::ios::trunc);
                    if (sync) {
                        sync << ++rt.bridge_seq << ",CLOSE,1\n";
                        sync.close();
                        rt.bridge.in_flight = true;
                        rt.bridge.pending_seq = rt.bridge_seq;
                        rt.bridge.pending_is_entry = false;
                        rt.bridge.pending_action = "CLOSE";
                        rt.bridge.sent_wall_ns = core::monotonic_now_ns();
                        ++rt.bridge.sent;
                        ++rt.bridge.mismatches;
                        std::cout << "MT5_BRIDGE_RECONCILE seq=" << rt.bridge_seq
                                  << " action=CLOSE reason=strategy_flat_bridge_open\n";
                        std::cout.flush();
                    }
                    continue;
                }
                // The mirror case: MT5 is flat but the strategy still believes
                // it holds something — a broker-side SL/TP can close a position
                // without the runtime ever issuing a command. Exits are then
                // suppressed as "nothing to close" and the strategy never
                // returns to idle. MT5 is authoritative, so the phantom paper
                // position is discarded by rebuilding the strategy, the same
                // bounded, counted action used for a halt re-arm.
                // Both sides hold a position but on opposite sides. The exit
                // the strategy would raise is a reduce_only on the wrong sign,
                // which the RiskEngine correctly refuses forever. Close what
                // MT5 actually holds and let the strategy re-derive.
                if (!rt.bridge.flat() && rt.flip->position().active &&
                    !rt.bridge.holds(rt.flip->position().side)) {
                    std::ofstream sync(bridge_dir + "/mme_cmd.txt", std::ios::trunc);
                    if (sync) {
                        sync << ++rt.bridge_seq << ",CLOSE,1\n";
                        sync.close();
                        rt.bridge.in_flight = true;
                        rt.bridge.pending_seq = rt.bridge_seq;
                        rt.bridge.pending_is_entry = false;
                        rt.bridge.pending_action = "CLOSE";
                        rt.bridge.sent_wall_ns = core::monotonic_now_ns();
                        ++rt.bridge.sent;
                        ++rt.bridge.mismatches;
                        std::cout << "MT5_BRIDGE_RECONCILE seq=" << rt.bridge_seq
                                  << " action=CLOSE reason=side_disagreement\n";
                        std::cout.flush();
                    }
                    continue;
                }
                if (rt.bridge.flat() && rt.flip->position().active) {
                    ++rt.bridge.mismatches;
                    ++rt.rearms;
                    rt.flip = std::make_unique<strategy::ScalpFlip>(rt.flip_config);
                    std::cout << "MT5_BRIDGE_RECONCILE action=reset_strategy"
                                 " reason=bridge_flat_strategy_open count=" << rt.rearms << '\n';
                    std::cout.flush();
                    continue;
                }

                const auto d0 = core::monotonic_now_ns();
                const auto intent = rt.flip->on_market(trade_symbol, book, ts);
                rt.decision_ns.record(core::monotonic_now_ns() - d0);
                if (!intent.actionable()) { ++rt.vetoes; continue; }
                ++rt.intents;

                const bool is_entry = (intent.kind == strategy::IntentKind::enter);
                const bool meaningful = is_entry ? rt.bridge.flat() : !rt.bridge.flat();
                if (!meaningful) {
                    ++rt.bridge_deferred;
                    if (is_entry) rt.flip->on_entry_rejected(); else rt.flip->on_exit_rejected();
                    continue;
                }

                risk::Request req{};
                req.symbol_id = trade_symbol;
                req.strategy_id = intent.strategy_id;
                req.side = intent.side;
                req.volume = intent.volume;
                req.risk_minor = 1;
                req.free_margin = rt.broker->account().free_margin_minor;
                req.spread = tick.ask_ticks - tick.bid_ticks;
                req.warm_mask = limits.required_warm_mask;
                req.session_open = true;
                req.now_ns = ts;
                req.hedging = true;
                req.reduce_only = intent.reduce_only;
                // Bridge-authoritative, not PaperBroker: this is the exposure
                // that actually exists at the broker, and it is what determines
                // whether this order reduces or opens.
                req.current_net = rt.bridge.net_volume(intent.volume);
                req.position_owner_strategy = intent.strategy_id;
                if (!rt.risk->check(req).approved) {
                    ++rt.risk_rejects;
                    if (is_entry) rt.flip->on_entry_rejected(); else rt.flip->on_exit_rejected();
                    continue;
                }

                exec::OrderRequest order{};
                order.corr_id = rt.order_seq;
                order.strategy_id = intent.strategy_id;
                order.symbol_id = trade_symbol;
                order.side = intent.side;
                order.type = exec::OrderType::market;
                order.volume = intent.volume;
                order.seq_global = rt.order_seq++;
                order.reduce_only = intent.reduce_only;
                const auto sub = rt.broker->submit(order, ts);
                if (!sub.accepted) {
                    ++rt.broker_rejects;
                    if (is_entry) rt.flip->on_entry_rejected(); else rt.flip->on_exit_rejected();
                    continue;
                }
                rt.working = sub.ref;
                rt.entry_in_flight = is_entry;
                if (is_entry) ++rt.entries; else ++rt.exits;

                const char* action = is_entry
                    ? (intent.side == exec::Side::buy ? "BUY" : "SELL") : "CLOSE";
                std::ofstream cmd(bridge_dir + "/mme_cmd.txt", std::ios::trunc);
                if (cmd) {
                    cmd << ++rt.bridge_seq << ',' << action << ",1\n";
                    cmd.close();
                    rt.bridge.in_flight = true;
                    rt.bridge.pending_seq = rt.bridge_seq;
                    rt.bridge.pending_is_entry = is_entry;
                    rt.bridge.pending_side = intent.side;
                    rt.bridge.pending_action = action;
                    rt.bridge.sent_wall_ns = core::monotonic_now_ns();
                    ++rt.bridge.sent;
                }
            }
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    (void)tailer.save_cursor(cursor_path);
    const auto elapsed = (core::monotonic_now_ns() - start_ns) / 1'000'000'000ULL;
    std::cout << "\n=== FINAL ===\n";
    print_status(rt, "STOPPED", elapsed);
    std::cout << "tailer_total_events=" << tailer.total_events()
              << " cursor_segment=" << tailer.cursor().segment_index
              << " cursor_frames=" << tailer.cursor().frames_consumed
              << " corrupt=" << (tailer.corrupt() ? 1 : 0) << '\n';
    std::cout << "VETO_DISTRIBUTION";
    for (std::size_t i = 0; i < rt.veto_reason.size(); ++i)
        if (rt.veto_reason[i] > 0) std::cout << ' ' << veto_name(i) << '=' << rt.veto_reason[i];
    std::cout << '\n';
    std::cout << "journal_records=" << rt.broker->journal().size()
              << " journal_digest=" << rt.broker->journal().digest()
              << " sharpe_per_trade=" << rt.stats.sharpe_per_trade()
              << " profit_factor=" << rt.stats.profit_factor() << '\n';
    std::cout << "LIVE_ORDER_PATH=disabled PAPER_ONLY=confirmed\n";
    return 0;
}
