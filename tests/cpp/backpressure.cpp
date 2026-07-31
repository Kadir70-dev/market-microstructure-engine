#include <iostream>

#include "core/backpressure.hpp"

int main() {
    using core::BackpressureAction;
    using core::RingId;
    int failures = 0;
    if (core::evaluate_backpressure(RingId::market_data, 74, 100).action != BackpressureAction::none) ++failures;
    if (core::evaluate_backpressure(RingId::market_data, 75, 100).action != BackpressureAction::halt_new_entries) ++failures;
    if (core::evaluate_backpressure(RingId::market_data, 100, 100).action != BackpressureAction::halt_engine) ++failures;
    if (core::evaluate_backpressure(RingId::command, 8, 8).action != BackpressureAction::reject_resource_exhausted) ++failures;
    if (core::evaluate_backpressure(RingId::event, 8, 8).action != BackpressureAction::halt_engine) ++failures;
    if (core::evaluate_backpressure(RingId::wal, 8, 8).action != BackpressureAction::halt_engine) ++failures;
    if (core::evaluate_backpressure(RingId::journal, 8, 8).action != BackpressureAction::halt_engine) ++failures;
    if (core::evaluate_backpressure(RingId::telemetry, 8, 8).action != BackpressureAction::drop_telemetry) ++failures;
    std::cout << "backpressure failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
