#include <chrono>
#include <filesystem>
#include "phase6_test.hpp"
#include "risk/kill_switch.hpp"
int main(){Phase6Test t;auto p=std::filesystem::temp_directory_path()/("mme_kill_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));risk::KillSwitch a(p/"state/kill.json");t.check(!a.halted_on_startup(),"clear startup");t.check(a.trip(risk::HaltReason::manual_kill,7,-9),"durable trip");risk::KillSwitch b(p/"state/kill.json");t.check(b.halted_on_startup(),"restart halted");std::error_code ec;std::filesystem::remove_all(p,ec);return t.result();}
