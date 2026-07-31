#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include "oms/oms.hpp"
#include "risk/risk_engine.hpp"
int main(){risk::Limits l{};risk::RiskEngine r(l);risk::Request q{};q.volume=1;q.risk_minor=1;q.free_margin=1000000;q.warm_mask=1;constexpr std::size_t n=100000;static std::array<std::uint64_t,n> a{},b{};oms::Oms o;auto approval=r.check(q);exec::BrokerOrderRef ref{1,1};auto*order=o.create(ref,0,1,approval.token);if(!order||!o.transition(ref,exec::OrderState::sent)||!o.transition(ref,exec::OrderState::acknowledged)||!o.transition(ref,exec::OrderState::partially_filled))return 2;for(std::size_t i=0;i<n;++i){auto x=std::chrono::steady_clock::now();auto z=r.check(q);auto y=std::chrono::steady_clock::now();if(!z.approved)return 4;a[i]=std::chrono::duration_cast<std::chrono::nanoseconds>(y-x).count();x=std::chrono::steady_clock::now();const auto moved=o.transition(ref,exec::OrderState::partially_filled);y=std::chrono::steady_clock::now();if(!moved)return 3;b[i]=std::chrono::duration_cast<std::chrono::nanoseconds>(y-x).count();}std::sort(a.begin(),a.end());std::sort(b.begin(),b.end());auto p99=n*99/100;std::cout<<"risk_p99_ns="<<a[p99]<<" oms_p99_ns="<<b[p99]<<"\n";return a[p99]<1000&&b[p99]<500?0:1;}
