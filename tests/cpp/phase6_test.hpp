#pragma once
#include <iostream>
struct Phase6Test{int failures{0};void check(bool v,const char*n){if(!v){std::cerr<<n<<"\n";++failures;}}int result()const{return failures?1:0;}};
