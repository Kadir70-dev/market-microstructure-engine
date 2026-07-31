#pragma once
#include <cstdint>
namespace oms {
enum class ReconcileBreak : std::uint8_t { none, order_state, filled_quantity,
 position_quantity, position_ticket, orphan_order, orphan_position, balance,
 equity, margin, margin_mode, unresolved_unknown, invalid_tolerance };
struct ReconcileView {
 std::uint32_t engine_orders{0},broker_orders{0},engine_positions{0},broker_positions{0};
 std::int64_t engine_balance{0},broker_balance{0},engine_equity{0},broker_equity{0};
 std::int64_t engine_margin{0},broker_margin{0};
 bool order_state_mismatch{false},filled_quantity_mismatch{false};
 bool position_quantity_mismatch{false},position_ticket_mismatch{false};
 bool orphan_order{false},orphan_position{false},margin_mode_mismatch{false};
 bool unresolved_unknown{false},orphan{false};
};
[[nodiscard]] constexpr ReconcileBreak reconcile_break(const ReconcileView&v,std::int64_t tolerance=0)noexcept{
 if(tolerance<0)return ReconcileBreak::invalid_tolerance;
 if(v.order_state_mismatch)return ReconcileBreak::order_state;
 if(v.filled_quantity_mismatch)return ReconcileBreak::filled_quantity;
 if(v.position_quantity_mismatch)return ReconcileBreak::position_quantity;
 if(v.position_ticket_mismatch)return ReconcileBreak::position_ticket;
 if(v.orphan_order||v.orphan)return ReconcileBreak::orphan_order;
 if(v.orphan_position)return ReconcileBreak::orphan_position;
 auto outside=[tolerance](std::int64_t a,std::int64_t b){return a>b?a-b>tolerance:b-a>tolerance;};
 if(outside(v.engine_balance,v.broker_balance))return ReconcileBreak::balance;
 if(outside(v.engine_equity,v.broker_equity))return ReconcileBreak::equity;
 if(outside(v.engine_margin,v.broker_margin))return ReconcileBreak::margin;
 if(v.margin_mode_mismatch)return ReconcileBreak::margin_mode;
 if(v.unresolved_unknown)return ReconcileBreak::unresolved_unknown;
 if(v.engine_orders!=v.broker_orders)return ReconcileBreak::orphan_order;
 if(v.engine_positions!=v.broker_positions)return ReconcileBreak::orphan_position;
 return ReconcileBreak::none;
}
[[nodiscard]] constexpr bool reconciled(const ReconcileView&v,std::int64_t tolerance=0)noexcept{return reconcile_break(v,tolerance)==ReconcileBreak::none;}
}
