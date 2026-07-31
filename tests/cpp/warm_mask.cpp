#include <iostream>

#include "features/feature_engine.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}
using features::Feature;
}

int main() {
    // An empty book warms nothing. Part 10.2 makes "warm-mask satisfied" a
    // pre-trade check, so an unavailable feature must be structurally
    // untradable rather than silently reading zero.
    {
        book::OrderBook empty(1);
        features::FeatureEngine engine(4);
        const auto vector = engine.compute(empty, book::BookSource::l1_only);
        check(vector.warm_mask == 0, "warm_empty_book_warms_nothing");
        check(!vector.warm(Feature::mid_price), "warm_mid_cold_on_empty");
    }

    // Top-of-book features warm on the first two-sided quote.
    {
        book::OrderBook order_book(1);
        features::FeatureEngine engine(4);
        order_book.apply_quote(100, 110, 4, 6, 1);
        const auto vector = engine.compute(order_book, book::BookSource::l1_only);
        check(vector.warm(Feature::mid_price), "warm_mid_immediate");
        check(vector.warm(Feature::spread_ticks), "warm_spread_immediate");
        check(vector.warm(Feature::microprice), "warm_microprice_with_sizes");
        check(vector.warm(Feature::l1_imbalance), "warm_imbalance_with_sizes");
    }

    // Zero L1 size is the normal MT5 forex case. Size-derived features are then
    // undefined and must stay cold rather than degenerating to the mid.
    {
        book::OrderBook order_book(1);
        features::FeatureEngine engine(4);
        order_book.apply_quote(100, 110, 0, 0, 1);
        const auto vector = engine.compute(order_book, book::BookSource::l1_only);
        check(vector.warm(Feature::mid_price), "warm_mid_without_sizes");
        check(!vector.warm(Feature::microprice), "warm_microprice_cold_without_sizes");
        check(!vector.warm(Feature::l1_imbalance), "warm_imbalance_cold_without_sizes");
    }

    // Return features stay cold until min_return_samples is reached, then warm.
    {
        book::OrderBook order_book(1);
        features::FeatureEngine engine(4);
        features::FeatureVector vector{};
        for (int i = 0; i < 3; ++i) {
            order_book.apply_quote(100 + i, 110 + i, 1, 1, static_cast<std::uint64_t>(i));
            vector = engine.compute(order_book, book::BookSource::l1_only);
        }
        check(!vector.warm(Feature::return_stddev), "warm_returns_cold_before_threshold");
        for (int i = 3; i < 12; ++i) {
            order_book.apply_quote(100 + i, 110 + i, 1, 1, static_cast<std::uint64_t>(i));
            vector = engine.compute(order_book, book::BookSource::l1_only);
        }
        check(vector.warm(Feature::return_mean), "warm_return_mean_after_threshold");
        check(vector.warm(Feature::return_stddev), "warm_return_stddev_after_threshold");
        check(engine.return_samples() >= engine.min_return_samples(), "warm_sample_count");
    }

    // reset() must return the engine to cold, or a symbol re-subscription would
    // inherit another session's warm state.
    {
        book::OrderBook order_book(1);
        features::FeatureEngine engine(4);
        for (int i = 0; i < 12; ++i) {
            order_book.apply_quote(100 + i, 110 + i, 1, 1, static_cast<std::uint64_t>(i));
            (void)engine.compute(order_book, book::BookSource::l1_only);
        }
        engine.reset();
        check(engine.return_samples() == 0, "warm_reset_clears_samples");
        const auto vector = engine.compute(order_book, book::BookSource::l1_only);
        check(!vector.warm(Feature::return_stddev), "warm_reset_returns_cold");
    }

    return failures == 0 ? 0 : 1;
}
