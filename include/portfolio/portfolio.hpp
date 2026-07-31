#pragma once
#include <array>
#include "exec/exec_types.hpp"
namespace portfolio {
enum class MarginMode:std::uint8_t{netting,hedging};
struct Aggregate{std::int64_t net{0},gross{0},realized{0},unrealized{0};};
class Portfolio final{public:explicit Portfolio(MarginMode m)noexcept:mode_(m){}
 [[nodiscard]] MarginMode mode()const noexcept{return mode_;}[[nodiscard]]std::size_t size()const noexcept{return count_;}
 [[nodiscard]] bool upsert(std::uint64_t ticket,std::uint32_t symbol,exec::Side side,std::int64_t volume,std::int64_t realized=0,std::int64_t unrealized=0)noexcept{
  if(symbol>=agg_.size()||volume<0)return false;
  if(mode_==MarginMode::netting){for(std::size_t i=0;i<count_;++i)if(pos_[i].symbol==symbol&&pos_[i].ticket!=ticket)return false;}
  auto*p=find(ticket);if(!p){if(count_>=pos_.size())return false;p=&pos_[count_++];}*p={ticket,symbol,side,volume,realized,unrealized};rebuild();return true;}
 [[nodiscard]] Aggregate symbol(std::uint32_t s)const noexcept{return s<agg_.size()?agg_[s]:Aggregate{};}
 [[nodiscard]] std::int64_t gross()const noexcept{std::int64_t v=0;for(auto&a:agg_)v+=a.gross;return v;}
 [[nodiscard]] std::int64_t net()const noexcept{std::int64_t v=0;for(auto&a:agg_)v+=a.net;return v;}
private:struct P{std::uint64_t ticket{0};std::uint32_t symbol{0};exec::Side side{exec::Side::buy};std::int64_t volume{0},realized{0},unrealized{0};};P*find(std::uint64_t t)noexcept{for(std::size_t i=0;i<count_;++i)if(pos_[i].ticket==t)return&pos_[i];return nullptr;}void rebuild()noexcept{agg_={};for(std::size_t i=0;i<count_;++i){auto&p=pos_[i];auto&a=agg_[p.symbol];auto s=p.side==exec::Side::buy?p.volume:-p.volume;a.net+=s;a.gross+=p.volume;a.realized+=p.realized;a.unrealized+=p.unrealized;}}MarginMode mode_;std::array<P,64>pos_{};std::size_t count_{0};std::array<Aggregate,8>agg_{};};
}
