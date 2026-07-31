#include "phase6_test.hpp"
#include "risk/self_trade_prevention.hpp"
int main(){Phase6Test t;t.check(risk::self_trade_ok(false,false,1,2,1,exec::Side::sell),"netting");t.check(!risk::self_trade_ok(true,false,1,2,1,exec::Side::sell),"hedging opposition");t.check(risk::self_trade_ok(true,true,1,2,1,exec::Side::sell),"explicit allow");t.check(risk::self_trade_ok(true,false,1,1,1,exec::Side::sell),"same owner");return t.result();}
