#include <limits>
#include "phase6_test.hpp"
#include "risk/margin_projection.hpp"
int main(){Phase6Test t;t.check(risk::projected_free_margin(1000,100,1000,200)==880,"projection");t.check(risk::projected_free_margin(10,11,0,0)<0,"margin reject");t.check(risk::projected_free_margin(100,0,std::numeric_limits<std::int64_t>::max(),200)<0,"overflow");t.check(risk::projected_free_margin(-1,0,0,0)<0,"negative");return t.result();}
