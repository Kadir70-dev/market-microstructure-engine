#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include "risk/halt_state.hpp"
namespace risk {
class KillSwitch final{public:explicit KillSwitch(std::filesystem::path p):path_(std::move(p)){}[[nodiscard]]bool halted_on_startup()const noexcept{std::error_code e;return std::filesystem::exists(path_,e);}
 [[nodiscard]]bool trip(HaltReason r,std::uint64_t ts,std::int64_t metric)noexcept{std::error_code e;std::filesystem::create_directories(path_.parent_path(),e);if(e)return false;auto tmp=path_;tmp+=".tmp";char data[192]{};auto n=std::snprintf(data,sizeof(data),"{\"reason\":%u,\"timestamp_ns\":%llu,\"metric\":%lld}\n",static_cast<unsigned>(r),static_cast<unsigned long long>(ts),static_cast<long long>(metric));if(n<=0||static_cast<std::size_t>(n)>=sizeof(data))return false;
#ifdef _WIN32
  HANDLE h=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE)return false;DWORD wrote=0;bool ok=WriteFile(h,data,static_cast<DWORD>(n),&wrote,nullptr)&&wrote==static_cast<DWORD>(n)&&FlushFileBuffers(h);if(!CloseHandle(h))ok=false;if(!ok){DeleteFileW(tmp.c_str());return false;}if(!MoveFileExW(tmp.c_str(),path_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(tmp.c_str());return false;}HANDLE d=CreateFileW(path_.parent_path().c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);if(d==INVALID_HANDLE_VALUE)return false;if(!FlushFileBuffers(d)){const auto err=GetLastError();ok=err==ERROR_INVALID_FUNCTION||err==ERROR_ACCESS_DENIED;}CloseHandle(d);return ok;
#else
  int f=::open(tmp.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);if(f<0)return false;std::size_t off=0;while(off<static_cast<std::size_t>(n)){auto w=::write(f,data+off,static_cast<std::size_t>(n)-off);if(w<=0){::close(f);::unlink(tmp.c_str());return false;}off+=static_cast<std::size_t>(w);}bool ok=::fsync(f)==0&&::close(f)==0;if(!ok||::rename(tmp.c_str(),path_.c_str())!=0){::unlink(tmp.c_str());return false;}int d=::open(path_.parent_path().c_str(),O_RDONLY|O_DIRECTORY);if(d<0)return false;ok=::fsync(d)==0;::close(d);return ok;
#endif
 }
private:std::filesystem::path path_;};}
