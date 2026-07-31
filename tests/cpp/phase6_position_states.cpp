#include "phase6_test.hpp"
#include "oms/position_state.hpp"
int main(){Phase6Test t;for(int a=0;a<6;++a)for(int b=0;b<6;++b){bool e=(a==0&&(b==1||b==5))||(a==1&&(b==2||b==3||b==5))||(a==2&&(b==1||b==5))||(a==3&&(b==4||b==5))||(a==5&&(b==1||b==4));t.check(oms::legal(static_cast<exec::PositionState>(a),static_cast<exec::PositionState>(b))==e,"position transition matrix");}return t.result();}
