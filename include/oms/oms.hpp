#pragma once
#include <array>
#include "exec/exec_types.hpp"
#include "oms/order_state.hpp"
#include "risk/risk_engine.hpp"
namespace oms {
inline constexpr std::size_t capacity=256;
class Oms final { public:
 // Invariant 5: an order reserves its FULL requested volume, not the unfilled
 // remainder — an UNKNOWN order may have executed any part of itself. Reserve
 // and release must therefore be symmetric on requested_volume, or a partial
 // fill silently strands reservation that Part 8.5 says is released only on a
 // terminal state.
 [[nodiscard]] exec::Order* restore(const exec::Order&o)noexcept{if(find(o.ref)||count_>=capacity||o.requested_volume<=0||o.filled_volume<0||o.filled_volume>o.requested_volume)return nullptr;orders_[count_]=o;if(exec::reserves_exposure(o.state))reserved_+=o.requested_volume;return &orders_[count_++];}
 [[nodiscard]] exec::Order* create(exec::BrokerOrderRef ref,std::uint32_t symbol,std::int64_t volume,const risk::Approval&) noexcept {
  if(find(ref)||count_>=capacity)return nullptr;
  auto&o=orders_[count_++]; o={}; o.ref=ref;o.symbol_id=symbol;o.requested_volume=volume;o.state=exec::OrderState::pending_send;reserved_+=volume;return &o; }
 [[nodiscard]] bool transition(exec::BrokerOrderRef ref,exec::OrderState next) noexcept {auto*o=find(ref);if(!o||!legal(o->state,next))return false;auto was=exec::reserves_exposure(o->state);o->state=next;auto now=exec::reserves_exposure(next);if(was&&!now)reserved_-=o->requested_volume;return true;}
 [[nodiscard]] bool fill(exec::BrokerOrderRef ref,std::int64_t volume) noexcept {auto*o=find(ref);if(!o||volume<=0||volume>o->remaining())return false;o->filled_volume+=volume;return transition(ref,o->remaining()?exec::OrderState::partially_filled:exec::OrderState::filled);}
 [[nodiscard]] bool recover_fill(exec::BrokerOrderRef ref,std::int64_t volume)noexcept{auto*o=find(ref);if(!o||volume<=0||volume>o->remaining())return false;o->filled_volume+=volume;return true;}
 [[nodiscard]] bool mark_unknown_on_restart(exec::BrokerOrderRef ref)noexcept{auto*o=find(ref);if(!o||exec::is_terminal(o->state))return false;o->state=exec::OrderState::unknown;return true;}
 [[nodiscard]] bool resolve_unknown(exec::BrokerOrderRef ref,exec::OrderState terminal) noexcept {auto*o=find(ref);return o&&o->state==exec::OrderState::unknown&&exec::is_terminal(terminal)&&transition(ref,terminal);}
 [[nodiscard]] exec::Order* find(exec::BrokerOrderRef ref) noexcept {for(std::size_t i=0;i<count_;++i)if(orders_[i].ref==ref)return &orders_[i];return nullptr;}
 [[nodiscard]] const exec::Order* find(exec::BrokerOrderRef ref)const noexcept {for(std::size_t i=0;i<count_;++i)if(orders_[i].ref==ref)return &orders_[i];return nullptr;}
 [[nodiscard]] const exec::Order& at(std::size_t i)const noexcept{return orders_[i];} [[nodiscard]] std::size_t size()const noexcept{return count_;}
 [[nodiscard]] std::int64_t reserved_exposure()const noexcept{return reserved_;}
private:std::array<exec::Order,capacity>orders_{};std::size_t count_{0};std::int64_t reserved_{0};};
}
