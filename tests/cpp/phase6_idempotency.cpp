#include "phase6_test.hpp"
#include "oms/oms.hpp"
int main(){Phase6Test t;risk::RiskEngine e(risk::Limits{});risk::Request q{};q.volume=1;q.risk_minor=1;q.free_margin=100;q.warm_mask=1;q.session_open=true;auto d=e.check(q);oms::Oms o;exec::BrokerOrderRef id{1,2};t.check(o.create(id,0,10,d.token)!=nullptr,"create");t.check(o.create(id,0,10,d.token)==nullptr,"duplicate id");t.check(o.size()==1&&o.reserved_exposure()==10,"unchanged");return t.result();}
