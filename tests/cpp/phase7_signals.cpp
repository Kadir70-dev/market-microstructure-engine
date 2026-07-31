#include <cmath>
#include <iostream>

#include "strategy/signals.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}
bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

strategy::TopOfBook make(std::int64_t bid, std::int64_t ask, std::int64_t bid_size,
                         std::int64_t ask_size) {
    strategy::TopOfBook t{};
    t.bid_ticks = bid; t.ask_ticks = ask; t.bid_size = bid_size; t.ask_size = ask_size;
    t.valid = true;
    return t;
}

book::OrderBook quoted(std::int64_t bid, std::int64_t ask, std::int64_t bid_size,
                       std::int64_t ask_size) {
    book::OrderBook b(0);
    (void)b.apply_quote(bid, ask, bid_size, ask_size, 1'000);
    return b;
}
}

int main() {
    using namespace strategy;

    // ---- spread ------------------------------------------------------------
    check(make(100, 110, 1, 1).spread_ticks() == 10, "spread_ticks");

    // ---- queue imbalance ---------------------------------------------------
    check(near(queue_imbalance(make(100, 110, 30, 10)), 0.5), "queue_imbalance_bid_heavy");
    check(near(queue_imbalance(make(100, 110, 10, 30)), -0.5), "queue_imbalance_ask_heavy");
    check(near(queue_imbalance(make(100, 110, 20, 20)), 0.0), "queue_imbalance_balanced");
    check(near(queue_imbalance(make(100, 110, 0, 0)), 0.0), "queue_imbalance_no_size");
    check(near(queue_imbalance(make(100, 110, 5, 0)), 1.0), "queue_imbalance_bid_only");

    // ---- OFI, Cont / Kukanov / Stoikov -------------------------------------
    {
        // Bid size grows at an unchanged level: buying pressure.
        const auto e = order_flow_imbalance(make(100, 110, 10, 20), make(100, 110, 15, 20));
        check(near(e, 5.0), "ofi_bid_size_growth_positive");
    }
    {
        // Bid steps up: buying pressure even though displayed size fell.
        const auto e = order_flow_imbalance(make(100, 110, 10, 20), make(101, 110, 8, 20));
        check(near(e, 8.0), "ofi_bid_step_up_positive");
    }
    {
        // Ask steps down: selling pressure.
        const auto e = order_flow_imbalance(make(100, 110, 10, 20), make(100, 109, 10, 5));
        check(near(e, -5.0), "ofi_ask_step_down_negative");
    }
    {
        // Nothing moved.
        const auto e = order_flow_imbalance(make(100, 110, 10, 20), make(100, 110, 10, 20));
        check(near(e, 0.0), "ofi_static_book_zero");
    }
    {
        // An invalid previous snapshot yields no signal rather than a guess.
        strategy::TopOfBook cold{};
        check(near(order_flow_imbalance(cold, make(100, 110, 10, 20)), 0.0), "ofi_requires_previous");
    }

    // ---- book imbalance ----------------------------------------------------
    {
        book::OrderBook b(0);
        (void)b.apply_delta(book::Side::bid, 100, 30, book::DeltaAction::add, 1);
        (void)b.apply_delta(book::Side::bid, 99, 20, book::DeltaAction::add, 2);
        (void)b.apply_delta(book::Side::ask, 110, 10, book::DeltaAction::add, 3);
        (void)b.apply_delta(book::Side::ask, 111, 15, book::DeltaAction::add, 4);
        // bids 50, asks 25 -> (50-25)/75
        check(near(book_imbalance(b), 25.0 / 75.0), "book_imbalance_depth_weighted");
    }
    check(near(book_imbalance(quoted(100, 110, 0, 0)), 0.0), "book_imbalance_no_size");

    // ---- engine warm gating ------------------------------------------------
    {
        SignalEngine engine;
        features::FeatureEngine fe(2);

        // Sizes published: size-derived signals warm (OFI needs two snapshots).
        auto b = quoted(100, 110, 30, 10);
        auto fv = fe.compute(b, book::BookSource::dom_aggregated);
        auto s = engine.compute(b, fv, book::BookSource::dom_aggregated);
        check(s.ready(Signal::spread), "engine_spread_warm");
        check(s.ready(Signal::queue_imbalance), "engine_queue_warm");
        check(s.ready(Signal::book_imbalance), "engine_book_imbalance_warm_on_dom");
        check(!s.ready(Signal::ofi), "engine_ofi_cold_on_first_snapshot");

        auto b2 = quoted(100, 110, 35, 10);
        fv = fe.compute(b2, book::BookSource::dom_aggregated);
        s = engine.compute(b2, fv, book::BookSource::dom_aggregated);
        check(s.ready(Signal::ofi), "engine_ofi_warm_on_second_snapshot");
        check(near(s.spread, 10.0), "engine_spread_value");
    }

    // ---- L1 with no published size: cold, not zero -------------------------
    {
        SignalEngine engine;
        features::FeatureEngine fe(2);
        auto b = quoted(100, 110, 0, 0);   // MT5 forex: volume 0 at L1
        auto fv = fe.compute(b, book::BookSource::l1_only);
        auto s = engine.compute(b, fv, book::BookSource::l1_only);
        check(s.ready(Signal::spread), "l1_spread_still_warm");
        check(!s.ready(Signal::queue_imbalance), "l1_queue_cold_without_size");
        check(!s.ready(Signal::ofi), "l1_ofi_cold_without_size");
        check(!s.ready(Signal::book_imbalance), "l1_book_imbalance_cold");
    }

    // ---- depth signals require a depth-bearing source ----------------------
    {
        SignalEngine engine;
        features::FeatureEngine fe(2);
        auto b = quoted(100, 110, 30, 10);
        auto fv = fe.compute(b, book::BookSource::l1_only);
        auto s = engine.compute(b, fv, book::BookSource::l1_only);
        check(!s.ready(Signal::book_imbalance), "depth_signal_cold_on_l1_source");
        check(s.ready(Signal::queue_imbalance), "top_of_book_signal_warm_on_l1_source");
    }

    // ---- empty book warms nothing -----------------------------------------
    {
        SignalEngine engine;
        features::FeatureEngine fe(2);
        book::OrderBook empty(0);
        const auto fv = fe.compute(empty, book::BookSource::l1_only);
        const auto s = engine.compute(empty, fv, book::BookSource::l1_only);
        check(s.warm == 0, "empty_book_no_signals");
    }

    // ---- volatility and momentum arrive from the feature engine ------------
    {
        SignalEngine engine;
        features::FeatureEngine fe(2);
        SignalSet s{};
        for (int i = 0; i < 8; ++i) {
            auto b = quoted(100 + i, 110 + i, 5, 5);
            const auto fv = fe.compute(b, book::BookSource::l1_only);
            s = engine.compute(b, fv, book::BookSource::l1_only);
        }
        check(s.ready(Signal::momentum), "momentum_warms_from_features");
        check(s.ready(Signal::volatility), "volatility_warms_from_features");
        check(std::isfinite(s.momentum) && std::isfinite(s.volatility), "signals_finite");
    }

    return failures == 0 ? 0 : 1;
}
