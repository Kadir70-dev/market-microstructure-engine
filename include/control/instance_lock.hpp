#pragma once
#include <filesystem>
#include <string>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
namespace control {
class InstanceLock final {public: InstanceLock()=default;InstanceLock(const InstanceLock&)=delete;InstanceLock&operator=(const InstanceLock&)=delete;~InstanceLock(){release();}
 [[nodiscard]] bool acquire(const std::filesystem::path&p,const char*name="mme_engine")noexcept{if(held())return false;
#ifdef _WIN32
  mutex_=CreateMutexA(nullptr,FALSE,name);if(!mutex_||GetLastError()==ERROR_ALREADY_EXISTS){release();return false;}file_=CreateFileW(p.wstring().c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(file_==INVALID_HANDLE_VALUE){release();return false;}return true;
#else
  (void)name;fd_=::open(p.c_str(),O_CREAT|O_RDWR,0600);if(fd_<0)return false;if(::flock(fd_,LOCK_EX|LOCK_NB)!=0){::close(fd_);fd_=-1;return false;}return true;
#endif
 }
 [[nodiscard]] bool held()const noexcept{
#ifdef _WIN32
 return mutex_!=nullptr&&file_!=INVALID_HANDLE_VALUE;
#else
 return fd_>=0;
#endif
 }
 void release()noexcept{
#ifdef _WIN32
 if(file_!=INVALID_HANDLE_VALUE){CloseHandle(file_);file_=INVALID_HANDLE_VALUE;}if(mutex_){CloseHandle(mutex_);mutex_=nullptr;}
#else
 if(fd_>=0){::flock(fd_,LOCK_UN);::close(fd_);fd_=-1;}
#endif
 }
private:
#ifdef _WIN32
 HANDLE mutex_{nullptr};HANDLE file_{INVALID_HANDLE_VALUE};
#else
 int fd_{-1};
#endif
};}
