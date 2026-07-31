#pragma once
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#include "phase6_test.hpp"
#include "oms/recovery.hpp"
inline int phase6_crash_test(int argc,char**argv,unsigned boundary){
 if(argc==3&&std::string(argv[1])=="--child"){
  auto j=std::make_unique<exec::Journal>();
  for(unsigned i=1;i<=boundary;++i){const auto volume=static_cast<std::int64_t>(i);(void)j->emit(exec::JournalRecordType::command,i,77,i,0,0,0,volume,10,1);(void)j->emit(exec::JournalRecordType::order_state,i,77,i,0,0,3,4,0,volume);(void)j->emit(exec::JournalRecordType::acknowledgement,i,77,i,0,0,i);}
  std::ofstream f(argv[2],std::ios::binary|std::ios::trunc);for(std::size_t i=0;i<j->size();++i)f.write(reinterpret_cast<const char*>(&j->at(i)),sizeof(exec::JournalRecord));f.flush();f.close();std::_Exit(86);
 }
 Phase6Test t;auto path=std::filesystem::temp_directory_path()/("mme_crash_"+std::to_string(boundary)+"_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".bin");
#ifdef _WIN32
 auto child=_spawnl(_P_WAIT,argv[0],argv[0],"--child",path.string().c_str(),static_cast<char*>(nullptr));t.check(child==86,"child terminated at boundary");
#else
 auto pid=fork();if(pid==0)execl(argv[0],argv[0],"--child",path.string().c_str(),static_cast<char*>(nullptr));int status=0;if(pid>0)(void)waitpid(pid,&status,0);t.check(pid>0&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"child terminated at boundary");
#endif
 auto j=std::make_unique<exec::Journal>();std::ifstream f(path,std::ios::binary);exec::JournalRecord r{};while(f.read(reinterpret_cast<char*>(&r),sizeof(r)))t.check(j->append(r),"journal append");t.check(f.eof()&&f.gcount()==0,"complete durable records");
 auto a=std::make_unique<oms::RecoveryState>(portfolio::MarginMode::hedging);auto b=std::make_unique<oms::RecoveryState>(portfolio::MarginMode::hedging);t.check(oms::recover(*j,*a),"restart recovery");t.check(oms::recover(*j,*b),"deterministic repeat");const auto expected=static_cast<std::int64_t>(boundary)*(boundary+1)/2;t.check(a->orders.size()==boundary&&b->orders.size()==boundary,"no lost acknowledged state");t.check(a->counters.acks==boundary&&b->counters.acks==boundary,"no duplicated state");t.check(a->orders.reserved_exposure()==expected&&b->orders.reserved_exposure()==expected,"UNKNOWN exposure reserved");for(std::size_t i=0;i<a->orders.size();++i)t.check(a->orders.at(i).state==exec::OrderState::unknown,"nonterminal UNKNOWN");std::error_code ec;std::filesystem::remove(path,ec);return t.result();}
