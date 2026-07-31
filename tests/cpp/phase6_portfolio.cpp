#include "phase6_test.hpp"
#include "portfolio/portfolio.hpp"
int main(){Phase6Test t;portfolio::Portfolio h(portfolio::MarginMode::hedging);t.check(h.upsert(1,0,exec::Side::buy,10),"hedge buy");t.check(h.upsert(2,0,exec::Side::sell,4),"hedge sell");t.check(h.symbol(0).net==6&&h.symbol(0).gross==14,"aggregate");portfolio::Portfolio n(portfolio::MarginMode::netting);t.check(n.upsert(1,0,exec::Side::buy,10),"net first");t.check(!n.upsert(2,0,exec::Side::sell,1),"net second ticket rejected");return t.result();}
