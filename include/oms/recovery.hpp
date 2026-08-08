#pragma once
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include "exec/account.hpp"
#include "exec/journal.hpp"
#include "oms/oms.hpp"
#include "oms/pms.hpp"
#include "portfolio/portfolio.hpp"
#include "risk/halt_state.hpp"
namespace oms {
struct RiskCounters final { std::uint64_t commands{0},acks{0},rejections{0},fills{0},cancels{0},replaces{0}; };
struct RecoveryState final {
 // oms_capacity defaults to Oms's own default (== exec::max_journal_records,
 // 16384) so every pre-existing call site (RecoveryState(mode) with one
 // argument) is unaffected -- additive only. A caller recovering at larger
 // scale (see include/oms/recovery_streaming.hpp) passes an explicit,
 // larger capacity; Oms itself has supported arbitrary capacities via one
 // preallocated heap block since Phase B, proven to 1M+ orders by
 // oms_bench_main.cpp -- this constructor is the only piece that was
 // missing to let a caller actually ask for that here.
 explicit RecoveryState(portfolio::MarginMode m, std::size_t oms_capacity = default_capacity) noexcept
   : orders(oms_capacity), portfolio(m) {}
 Oms orders; Pms positions{}; portfolio::Portfolio portfolio; exec::AccountState account{};
 RiskCounters counters{}; risk::HaltReason halt{risk::HaltReason::none}; bool kill{false};
};
namespace detail {
[[nodiscard]] inline bool same_payload(const exec::JournalRecord&a,const exec::JournalRecord&b)noexcept{
 return a.ts_ns==b.ts_ns&&a.type==b.type&&a.reserved==b.reserved&&a.symbol_id==b.symbol_id&&
  a.run_id==b.run_id&&a.logical_order_id==b.logical_order_id&&a.position_ticket==b.position_ticket&&
  a.a==b.a&&a.b==b.b&&a.c==b.c&&a.d==b.d;
}
[[nodiscard]] inline bool valid_order_state(std::int64_t x)noexcept{return x>=0&&x<=static_cast<std::int64_t>(exec::OrderState::expired);}
}
[[nodiscard]] inline bool recover(const exec::Journal&j,RecoveryState&out,bool kill_present=false,
 risk::HaltReason persisted_halt=risk::HaltReason::none)noexcept{
 RecoveryState next(out.portfolio.mode());
 // Pre-existing, unrelated to this phase's storage change: exec::AccountState
 // is trivially copyable, so RecoveryState's compiler-generated assignment
 // (used below via out=std::move(next), same as the plain `out=next` this
 // replaced) can legally implement account's transfer as a raw byte copy of
 // sizeof(AccountState) rather than field-by-field -- which copies whatever
 // was in next.account's *trailing padding* verbatim. That padding was never
 // written by anything in this function (only named fields are), so it is
 // only as deterministic as next's own construction happened to leave it.
 // This was already latent; it surfaced reliably once Oms moved to a heap
 // allocation and shifted the memory-reuse pattern that had been masking it.
 // Zeroing it explicitly, once, right after construction and before any
 // field is set, makes it deterministic regardless of construction history.
 std::memset(&next.account,0,sizeof(next.account));
 if(j.overflowed())return false;
 std::uint64_t last_ts=0; bool have_account=false,have_pnl=false;
 for(std::size_t i=0;i<j.size();++i){
  const auto&r=j.at(i);
  if(r.seq!=i||r.reserved!=0||r.ts_ns<last_ts||r.symbol_id>=exec::max_symbols||r.type<1||r.type>10)return false;
  for(std::size_t k=0;k<i;++k)if(detail::same_payload(r,j.at(k)))return false;
  last_ts=r.ts_ns;const auto t=static_cast<exec::JournalRecordType>(r.type);
  const exec::BrokerOrderRef ref{r.run_id,r.logical_order_id};auto*o=next.orders.find(ref);
  if(t==exec::JournalRecordType::command){
   if(!r.run_id||!r.logical_order_id||r.position_ticket||r.a<0||r.a>1||r.b<=0||r.c<0||r.d<0||r.d>1)return false;
   exec::Order v{};v.ref=ref;v.symbol_id=r.symbol_id;v.side=static_cast<exec::Side>(r.a);v.requested_volume=r.b;
   v.limit_price_ticks=r.c;v.type=static_cast<exec::OrderType>(r.d);v.state=exec::OrderState::sent;
   if(!next.orders.restore(v))return false;++next.counters.commands;continue;
  }
  if(t==exec::JournalRecordType::order_state){
   if(!detail::valid_order_state(r.a)||!detail::valid_order_state(r.b)||r.position_ticket||r.c<0||r.d<0)return false;
   const auto from=static_cast<exec::OrderState>(r.a),to=static_cast<exec::OrderState>(r.b);
   // PaperBroker persists pending_send->sent immediately before its command payload.
   if(!o&&from==exec::OrderState::pending_send&&to==exec::OrderState::sent)continue;
   if(!o||o->state!=from||o->filled_volume!=r.c||o->requested_volume!=r.d||!next.orders.transition(ref,to))return false;
  } else if(t==exec::JournalRecordType::acknowledgement){
   if(!o||o->state!=exec::OrderState::acknowledged||r.a<0||r.position_ticket)return false;++next.counters.acks;
  } else if(t==exec::JournalRecordType::rejection){
   if(r.a<=0)return false;if(r.logical_order_id&&(!o||o->state!=exec::OrderState::sent))return false;++next.counters.rejections;
  } else if(t==exec::JournalRecordType::fill){
   if(!o||!r.position_ticket||r.a<=0||r.b<=0||r.c<0||(r.d!=0&&r.d!=1)||!next.orders.recover_fill(ref,r.b))return false;++next.counters.fills;
  } else if(t==exec::JournalRecordType::cancel){
   if(!o||o->state!=exec::OrderState::cancel_pending||r.a<0||r.position_ticket)return false;++next.counters.cancels;
  } else if(t==exec::JournalRecordType::replace){
   if(!o||o->state!=exec::OrderState::acknowledged||r.a<=0||r.b<=0||r.c<=0||r.d<=0||r.d<o->filled_volume||r.a!=o->limit_price_ticks||r.c!=o->requested_volume)return false;
   o->limit_price_ticks=r.b;o->requested_volume=r.d;++next.counters.replaces;
  } else if(t==exec::JournalRecordType::position_update){
   if(r.logical_order_id||!r.position_ticket||r.a<0||r.a>1||r.b<0||r.c<0)return false;
   auto*old=next.positions.find(r.position_ticket);
   if(old&&(old->symbol_id!=r.symbol_id||old->side!=static_cast<exec::Side>(r.a)||old->state==exec::PositionState::closed))return false;
   if(!old&&r.b==0)return false;
   exec::Position p{};p.position_ticket=r.position_ticket;p.symbol_id=r.symbol_id;p.side=static_cast<exec::Side>(r.a);
   p.state=r.b?exec::PositionState::open:exec::PositionState::closed;p.volume=r.b;p.avg_price_ticks=r.c;p.realized_pnl_minor=r.d;
   if(old)*old=p;else if(!next.positions.restore(p))return false;
   if(!next.portfolio.upsert(p.position_ticket,p.symbol_id,p.side,p.volume,p.realized_pnl_minor))return false;
  } else if(t==exec::JournalRecordType::account_update){
   if(r.logical_order_id||r.position_ticket||r.c<0||r.d<0||r.b-r.c!=r.d)return false;
   next.account.balance_minor=r.a;next.account.equity_minor=r.b;next.account.margin_used_minor=r.c;next.account.free_margin_minor=r.d;have_account=true;have_pnl=false;
  } else if(t==exec::JournalRecordType::pnl_update){
   if(r.logical_order_id||r.position_ticket||r.d!=0||!have_account||have_pnl||next.account.equity_minor!=next.account.balance_minor+r.b)return false;
   next.account.realized_pnl_minor=r.a;next.account.unrealized_pnl_minor=r.b;next.account.commission_paid_minor=r.c;have_pnl=true;
  }
 }
 if(have_account!=have_pnl)return false;
 for(std::size_t i=0;i<next.orders.size();++i){const auto&o=next.orders.at(i);if(!exec::is_terminal(o.state)&&o.state!=exec::OrderState::unknown)
   if(!next.orders.mark_unknown_on_restart(o.ref))return false;}
 next.account.open_orders=static_cast<std::uint32_t>(next.orders.size());next.account.open_positions=0;
 for(std::size_t i=0;i<next.positions.size();++i)if(next.positions.at(i).state!=exec::PositionState::closed)++next.account.open_positions;
 next.kill=kill_present;next.halt=kill_present?(persisted_halt==risk::HaltReason::none?risk::HaltReason::manual_kill:persisted_halt):persisted_halt;
 out=std::move(next);return true;
}
[[nodiscard]] inline bool recover(const exec::Journal&j,Oms&out)noexcept{RecoveryState s(portfolio::MarginMode::hedging);if(!recover(j,s))return false;out=std::move(s.orders);return true;}
}
