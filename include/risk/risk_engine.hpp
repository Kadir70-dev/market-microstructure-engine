#pragma once
#include <cstdint>
#include <limits>
#include "exec/exec_types.hpp"
#include "risk/halt_state.hpp"
#include "risk/limits.hpp"
#include "risk/margin_projection.hpp"
#include "risk/self_trade_prevention.hpp"
namespace risk {
enum class Reject : std::uint8_t { none, halted, invalid_configuration, invalid_arithmetic,
 risk_per_trade, position, gross, net,
 leverage, margin, open_orders, in_flight, rate_second, rate_minute, fills_minute,
 trades_day, cadence, spread, slippage, fat_finger, symbol, session, reduce_only,
 cold_features, self_trade };
struct Decision;
class Approval final { friend class RiskEngine; friend struct Decision; Approval() noexcept = default; public: Approval(const Approval&)=default; };
struct Request final { std::uint32_t symbol_id{0}, strategy_id{0}; exec::Side side{exec::Side::buy};
 std::int64_t volume{0}, risk_minor{0}, projected_position{0}, projected_gross{0}, projected_net{0};
 std::int64_t notional{0}, order_margin{0}, free_margin{0}, leverage_bp{0}, spread{0}, slippage{0};
 std::uint32_t open_orders{0}, in_flight{0}, requests_second{0}, orders_minute{0}, fills_minute{0}, trades_day{0};
 std::uint64_t now_ns{0}, last_order_ns{0}, warm_mask{0}; bool session_open{true}, reduce_only{false};
 std::int64_t current_net{0}; std::uint32_t position_owner_strategy{0}; bool hedging{false}; };
struct Decision final { bool approved{false}; Reject reject{Reject::none}; Approval token{}; };
class RiskEngine final { public:
 explicit constexpr RiskEngine(Limits limits) noexcept: limits_(limits), valid_(self_test(limits)) {}
 [[nodiscard]] constexpr bool valid() const noexcept { return valid_; }
 void halt(HaltReason r) noexcept { halted_=true; halt_reason_=r; }
 [[nodiscard]] bool halted() const noexcept { return halted_; }
 [[nodiscard]] HaltReason halt_reason() const noexcept { return halt_reason_; }
 [[nodiscard]] Decision check(const Request& r) const noexcept {
  const auto no=[&](Reject x){ Decision d{}; d.reject=x; return d; };
  if(!valid_) return no(Reject::invalid_configuration);
  if(halted_) return no(Reject::halted);
  if(r.projected_position==std::numeric_limits<std::int64_t>::min()||
     r.projected_net==std::numeric_limits<std::int64_t>::min()||
     r.notional<0||r.order_margin<0||r.free_margin<0||r.risk_minor<0||
     r.projected_gross<0||r.leverage_bp<0||r.spread<0||r.slippage<0||
     (r.last_order_ns&&r.now_ns<r.last_order_ns)) return no(Reject::invalid_arithmetic);
  if(r.risk_minor>limits_.max_risk_per_trade) return no(Reject::risk_per_trade);
  if(abs64(r.projected_position)>limits_.max_position_per_symbol) return no(Reject::position);
  if(r.projected_gross>limits_.max_gross_exposure) return no(Reject::gross);
  if(abs64(r.projected_net)>limits_.max_net_exposure) return no(Reject::net);
  if(r.leverage_bp>limits_.max_leverage_bp) return no(Reject::leverage);
  if(projected_free_margin(r.free_margin,r.order_margin,r.notional,limits_.adverse_shock_bp)<0) return no(Reject::margin);
  if(r.open_orders>=limits_.max_open_orders) return no(Reject::open_orders);
  if(r.in_flight>=limits_.max_in_flight_per_symbol) return no(Reject::in_flight);
  if(r.requests_second>=limits_.max_order_requests_per_second) return no(Reject::rate_second);
  if(r.orders_minute>=limits_.max_orders_per_minute) return no(Reject::rate_minute);
  if(r.fills_minute>=limits_.max_fills_per_minute) return no(Reject::fills_minute);
  if(r.trades_day>=limits_.max_trades_per_day) return no(Reject::trades_day);
  if(r.last_order_ns&&r.now_ns-r.last_order_ns<limits_.min_time_between_orders_ns) return no(Reject::cadence);
  if(r.spread>limits_.max_spread_ticks) return no(Reject::spread);
  if(r.slippage>limits_.max_slippage_ticks) return no(Reject::slippage);
  if(r.volume<=0||r.volume>limits_.max_order_volume) return no(Reject::fat_finger);
  if(r.symbol_id>=limits_.symbol_allowed.size()||!limits_.symbol_allowed[r.symbol_id]) return no(Reject::symbol);
  if(!r.session_open) return no(Reject::session);
  if(r.reduce_only && ((r.current_net>=0&&r.side==exec::Side::buy)||(r.current_net<=0&&r.side==exec::Side::sell))) return no(Reject::reduce_only);
  if((r.warm_mask&limits_.required_warm_mask)!=limits_.required_warm_mask) return no(Reject::cold_features);
  if(!self_trade_ok(r.hedging,limits_.allow_cross_strategy_opposition,r.position_owner_strategy,r.strategy_id,r.current_net,r.side)) return no(Reject::self_trade);
  Decision d{}; d.approved=true; return d;
 }
 [[nodiscard]] HaltReason evaluate(const HaltSignals&s) const noexcept {
  if(s.pnl<=-limits_.max_daily_loss)return HaltReason::daily_loss;
  if(s.peak_equity>0&&s.equity*10000<=s.peak_equity*(10000-limits_.max_drawdown_bp))return HaltReason::drawdown;
  if(s.consecutive_losses>=limits_.max_consecutive_losses)return HaltReason::consecutive_losses;
  if(s.feed_age_ms>=limits_.max_staleness_ms)return HaltReason::feed_stale;
  if(s.gap)return HaltReason::sequence_gap;
  if(s.ring_percent>=100)return HaltReason::backpressure;
  if(s.latency_breach)return HaltReason::latency;
  if(s.reject_rate_bp>limits_.max_reject_rate_bp)return HaltReason::reject_rate;
  if(!s.connected)return HaltReason::disconnected;
  if(s.reconciliation_break)return HaltReason::reconciliation;
  if(!s.lock_held)return HaltReason::instance_lock;
  if(s.stale_epoch)return HaltReason::stale_epoch;
  if(s.disk_free_bytes<limits_.min_free_bytes)return HaltReason::disk;
  if(s.manual_kill)return HaltReason::manual_kill;
  if(s.control_kill)return HaltReason::control_kill;
  return HaltReason::none;
 }
private: static constexpr std::int64_t abs64(std::int64_t x) noexcept{return x<0?-x:x;} Limits limits_{}; bool valid_{false},halted_{false}; HaltReason halt_reason_{HaltReason::none}; };
}
