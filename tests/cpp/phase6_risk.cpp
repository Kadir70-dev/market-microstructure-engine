#include <limits>
#include "phase6_test.hpp"
#include "risk/risk_engine.hpp"
namespace { risk::Request good(){risk::Request r{};r.symbol_id=0;r.strategy_id=1;r.volume=1;r.risk_minor=1;r.free_margin=1000000;r.warm_mask=1;r.session_open=true;return r;} }
int main(){Phase6Test t;risk::Limits l{};risk::RiskEngine e(l);auto check=[&](risk::Request r,risk::Reject x,const char*n){t.check(e.check(r).reject==x,n);};t.check(e.check(good()).approved,"approve");
 auto r=good();r.volume=0;check(r,risk::Reject::fat_finger,"quantity zero");r=good();r.volume=l.max_order_volume+1;check(r,risk::Reject::fat_finger,"quantity maximum");
 r=good();r.risk_minor=l.max_risk_per_trade+1;check(r,risk::Reject::risk_per_trade,"notional risk limit");r=good();r.projected_gross=l.max_gross_exposure+1;check(r,risk::Reject::gross,"gross");r=good();r.projected_net=l.max_net_exposure+1;check(r,risk::Reject::net,"net");r=good();r.projected_position=l.max_position_per_symbol+1;check(r,risk::Reject::position,"position");
 r=good();r.requests_second=l.max_order_requests_per_second;check(r,risk::Reject::rate_second,"order rate");r=good();r.order_margin=r.free_margin+1;check(r,risk::Reject::margin,"margin");r=good();r.hedging=true;r.position_owner_strategy=2;r.current_net=1;r.side=exec::Side::sell;check(r,risk::Reject::self_trade,"self trade");
 r=good();r.notional=std::numeric_limits<std::int64_t>::max();r.order_margin=1;check(r,risk::Reject::margin,"multiply overflow");r=good();r.projected_net=std::numeric_limits<std::int64_t>::min();check(r,risk::Reject::invalid_arithmetic,"abs underflow");
 risk::Limits bad=l;bad.max_gross_exposure=0;risk::RiskEngine invalid(bad);t.check(invalid.check(good()).reject==risk::Reject::invalid_configuration,"invalid configuration");risk::RiskEngine halted(l);halted.halt(risk::HaltReason::manual_kill);t.check(halted.check(good()).reject==risk::Reject::halted,"halt kill");
 risk::HaltSignals s{};s.pnl=-l.max_daily_loss;t.check(e.evaluate(s)==risk::HaltReason::daily_loss,"daily loss");s={};s.peak_equity=100;s.equity=80;t.check(e.evaluate(s)==risk::HaltReason::drawdown,"drawdown");return t.result();}
