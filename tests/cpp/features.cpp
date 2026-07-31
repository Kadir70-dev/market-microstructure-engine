#include <cmath>
#include <iostream>

#include "features/feature_engine.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::cout << name << "=pass\n"; }
    else { std::cout << name << "=FAIL\n"; ++failures; }
}
bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }
using features::Feature;
using book::BookSource;
}

int main() {
    // ---- values against hand-computed fixtures ---------------------------
    {
        book::OrderBook order_book(1);
        features::FeatureEngine engine(4);
        order_book.apply_quote(100, 110, 4, 6, 1);
        const auto v = engine.compute(order_book, BookSource::l1_only);

        check(near(v.value(Feature::mid_price), 105.0, 1e-12), "feature_mid_hand_computed");
        check(near(v.value(Feature::spread_ticks), 10.0, 1e-12), "feature_spread_hand_computed");
        // Microprice weights each side by the OPPOSITE size:
        // (100*6 + 110*4) / 10 = 104.
        check(near(v.value(Feature::microprice), 104.0, 1e-12), "feature_microprice_hand_computed");
        // (4 - 6) / 10 = -0.2
        check(near(v.value(Feature::l1_imbalance), -0.2, 1e-12), "feature_imbalance_hand_computed");
    }

    // ---- feature x BookSource validity matrix ----------------------------
    {
        check(!features::is_supported(Feature::depth_imbalance, BookSource::l1_only),
              "matrix_depth_unsupported_on_l1");
        check(features::is_supported(Feature::depth_imbalance, BookSource::dom_aggregated),
              "matrix_depth_supported_on_dom");
        check(features::is_supported(Feature::depth_imbalance, BookSource::l2_exchange),
              "matrix_depth_supported_on_l2");
        check(features::is_supported(Feature::mid_price, BookSource::l1_only),
              "matrix_mid_supported_everywhere");

        book::OrderBook order_book(1);
        order_book.apply_delta(book::Side::bid, 100, 30, book::DeltaAction::add, 1);
        order_book.apply_delta(book::Side::bid, 99, 20, book::DeltaAction::add, 2);
        order_book.apply_delta(book::Side::ask, 110, 10, book::DeltaAction::add, 3);
        order_book.apply_delta(book::Side::ask, 111, 40, book::DeltaAction::add, 4);

        features::FeatureEngine l1_engine(4), dom_engine(4);
        const auto on_l1 = l1_engine.compute(order_book, BookSource::l1_only);
        check(!on_l1.warm(Feature::depth_imbalance), "matrix_depth_cold_on_l1_feed");

        const auto on_dom = dom_engine.compute(order_book, BookSource::dom_aggregated);
        check(on_dom.warm(Feature::depth_imbalance), "matrix_depth_warm_on_dom_feed");
        // bids 30+20=50, asks 10+40=50 -> perfectly balanced.
        check(near(on_dom.value(Feature::depth_imbalance), 0.0, 1e-12),
              "feature_depth_imbalance_hand_computed");
    }

    // ---- returns -----------------------------------------------------------
    {
        // A constant mid produces zero return and therefore zero volatility;
        // it must not produce NaN from log(1) or a division by zero.
        book::OrderBook order_book(1);
        features::FeatureEngine engine(2);
        for (int i = 0; i < 8; ++i) {
            order_book.apply_quote(100, 110, 1, 1, static_cast<std::uint64_t>(i));
            (void)engine.compute(order_book, BookSource::l1_only);
        }
        const auto v = engine.compute(order_book, BookSource::l1_only);
        check(v.warm(Feature::return_stddev), "feature_returns_warm");
        check(near(v.value(Feature::return_mean), 0.0, 1e-12), "feature_constant_mid_zero_return");
        check(near(v.value(Feature::return_stddev), 0.0, 1e-12), "feature_constant_mid_zero_vol");
    }

    // ---- NaN / Inf guard ---------------------------------------------------
    {
        // Every emitted value must be finite regardless of book contents. The
        // guard is on the single write path, so no feature can bypass it.
        book::OrderBook order_book(1);
        features::FeatureEngine engine(2);
        order_book.apply_quote(1, 2, 0, 0, 1);
        for (int i = 0; i < 8; ++i) {
            order_book.apply_quote(1 + i, 2 + i, 0, 0, static_cast<std::uint64_t>(i));
            (void)engine.compute(order_book, BookSource::l1_only);
        }
        const auto v = engine.compute(order_book, BookSource::l1_only);
        for (std::size_t i = 0; i < features::feature_count; ++i) {
            if (!std::isfinite(v.values[i])) {
                std::cout << "guard_non_finite_leaked index=" << i << "=FAIL\n";
                ++failures;
            }
        }
        check(true, "guard_all_values_finite");
        // A cold feature must read exactly zero, never stale or garbage.
        check(v.value(Feature::microprice) == 0.0, "guard_cold_feature_reads_zero");
    }

    return failures == 0 ? 0 : 1;
}
