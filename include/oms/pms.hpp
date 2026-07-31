#pragma once
#include <array>
#include "exec/exec_types.hpp"
#include "oms/position_state.hpp"
namespace oms {
class Pms final {public:
 [[nodiscard]] exec::Position* restore(const exec::Position&v)noexcept{if(find(v.position_ticket)||count_>=p_.size()||v.position_ticket==0||v.symbol_id>=8||v.volume<0)return nullptr;p_[count_]=v;return&p_[count_++];}
 [[nodiscard]] exec::Position* open(std::uint64_t ticket,std::uint32_t symbol,exec::Side side)noexcept{if(find(ticket)||count_>=64)return nullptr;auto&p=p_[count_++];p={};p.position_ticket=ticket;p.symbol_id=symbol;p.side=side;p.state=exec::PositionState::opening;return&p;}
 [[nodiscard]] exec::Position* find(std::uint64_t ticket)noexcept{for(std::size_t i=0;i<count_;++i)if(p_[i].position_ticket==ticket)return&p_[i];return nullptr;}
 [[nodiscard]] const exec::Position* find(std::uint64_t ticket)const noexcept{for(std::size_t i=0;i<count_;++i)if(p_[i].position_ticket==ticket)return&p_[i];return nullptr;}
 [[nodiscard]] bool transition(std::uint64_t t,exec::PositionState n)noexcept{auto*p=find(t);if(!p||!legal(p->state,n))return false;p->state=n;return true;}
 [[nodiscard]] bool modify(std::uint64_t t,std::uint64_t seq)noexcept{auto*p=find(t);if(!p||p->state!=exec::PositionState::open||seq<=modify_seq_[p-p_.data()])return false;modify_seq_[p-p_.data()]=seq;return transition(t,exec::PositionState::modify_pending);}
 [[nodiscard]] std::size_t size()const noexcept{return count_;} [[nodiscard]] const exec::Position& at(std::size_t i)const noexcept{return p_[i];}
private:std::array<exec::Position,64>p_{};std::array<std::uint64_t,64>modify_seq_{};std::size_t count_{0};};
}
