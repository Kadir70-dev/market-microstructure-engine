#include <chrono>
#include <filesystem>
#include "phase6_test.hpp"
#include "control/instance_lock.hpp"
int main(){Phase6Test t;auto p=std::filesystem::temp_directory_path()/("mme_lock_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".pid");control::InstanceLock a,b;t.check(a.acquire(p,"phase6"),"first");t.check(!b.acquire(p,"phase6"),"exclusive");a.release();t.check(b.acquire(p,"phase6"),"restart");b.release();std::error_code ec;std::filesystem::remove(p,ec);return t.result();}
