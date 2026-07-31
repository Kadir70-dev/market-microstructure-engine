#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>
namespace control {
enum class Intent:std::uint8_t{none,pause,flatten,kill};
struct SupervisorInputs final{bool instance_lock_healthy{false};bool kill_file_present{true};bool disk_headroom_ok{false};Intent intent{Intent::kill};};
class Supervisor final{public:void poll(const SupervisorInputs&i)noexcept{lock_failed_.store(!i.instance_lock_healthy,std::memory_order_relaxed);kill_.store(i.kill_file_present||i.intent==Intent::kill,std::memory_order_relaxed);disk_failed_.store(!i.disk_headroom_ok,std::memory_order_relaxed);pause_.store(i.intent==Intent::pause||i.intent==Intent::flatten||i.intent==Intent::kill,std::memory_order_relaxed);flatten_.store(i.intent==Intent::flatten||i.intent==Intent::kill,std::memory_order_relaxed);}[[nodiscard]]bool halted()const noexcept{return lock_failed_.load(std::memory_order_relaxed)||kill_.load(std::memory_order_relaxed)||disk_failed_.load(std::memory_order_relaxed);}[[nodiscard]]bool pause()const noexcept{return pause_.load(std::memory_order_relaxed);}[[nodiscard]]bool flatten()const noexcept{return flatten_.load(std::memory_order_relaxed);}[[nodiscard]]bool lock_failed()const noexcept{return lock_failed_.load(std::memory_order_relaxed);}[[nodiscard]]bool kill()const noexcept{return kill_.load(std::memory_order_relaxed);}[[nodiscard]]bool disk_failed()const noexcept{return disk_failed_.load(std::memory_order_relaxed);}static constexpr std::uint64_t period_ns=100000000ULL;
private:std::atomic<bool>lock_failed_{true},kill_{true},disk_failed_{true},pause_{true},flatten_{false};};
using SupervisorReader=SupervisorInputs(*)(void*) noexcept;
class SupervisorThread final{public:SupervisorThread(Supervisor&s,SupervisorReader r,void*c)noexcept:s_(s),reader_(r),context_(c){}~SupervisorThread(){stop();}SupervisorThread(const SupervisorThread&)=delete;SupervisorThread&operator=(const SupervisorThread&)=delete;
 [[nodiscard]]bool start(){bool expected=false;if(!running_.compare_exchange_strong(expected,true,std::memory_order_relaxed))return false;worker_=std::thread([this]{run();});return true;}
 void stop()noexcept{running_.store(false,std::memory_order_relaxed);if(worker_.joinable())worker_.join();}[[nodiscard]]bool running()const noexcept{return running_.load(std::memory_order_relaxed);}
private:void run()noexcept{using clock=std::chrono::steady_clock;auto next=clock::now();while(running_.load(std::memory_order_relaxed)){s_.poll(reader_(context_));next+=std::chrono::nanoseconds(Supervisor::period_ns);std::this_thread::sleep_until(next);}}Supervisor&s_;SupervisorReader reader_;void*context_;std::atomic<bool>running_{false};std::thread worker_{};};}
