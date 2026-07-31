#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include "risk/limits.hpp"
namespace risk {
class Sha256 final {
public:
 Sha256() noexcept { reset(); }
 void reset() noexcept { h_={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}; used_=0; bytes_=0; done_=false; }
 bool update(const void* data,std::size_t n) noexcept { if(done_||(n&&data==nullptr))return false;auto*p=static_cast<const std::uint8_t*>(data);bytes_+=n;while(n){auto take=(n<64-used_)?n:64-used_;for(std::size_t i=0;i<take;++i)buf_[used_+i]=p[i];used_+=take;p+=take;n-=take;if(used_==64){block(buf_.data());used_=0;}}return true; }
 std::array<std::uint8_t,32> finish() noexcept { if(done_)return digest_;auto bits=static_cast<std::uint64_t>(bytes_)*8;buf_[used_++]=0x80;if(used_>56){while(used_<64)buf_[used_++]=0;block(buf_.data());used_=0;}while(used_<56)buf_[used_++]=0;for(int i=7;i>=0;--i)buf_[used_++]=static_cast<std::uint8_t>(bits>>(i*8));block(buf_.data());for(std::size_t i=0;i<8;++i)for(std::size_t j=0;j<4;++j)digest_[i*4+j]=static_cast<std::uint8_t>(h_[i]>>(24-j*8));done_=true;return digest_; }
private:
 static constexpr std::array<std::uint32_t,64> k_={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
 static std::uint32_t ro(std::uint32_t x,unsigned n)noexcept{return(x>>n)|(x<<(32-n));}
 void block(const std::uint8_t*p)noexcept{std::uint32_t w[64]{};for(std::size_t i=0;i<16;++i)w[i]=(static_cast<std::uint32_t>(p[i*4])<<24)|(static_cast<std::uint32_t>(p[i*4+1])<<16)|(static_cast<std::uint32_t>(p[i*4+2])<<8)|p[i*4+3];for(std::size_t i=16;i<64;++i)w[i]=w[i-16]+(ro(w[i-15],7)^ro(w[i-15],18)^(w[i-15]>>3))+w[i-7]+(ro(w[i-2],17)^ro(w[i-2],19)^(w[i-2]>>10));auto a=h_[0],b=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],z=h_[7];for(std::size_t i=0;i<64;++i){auto t1=z+(ro(e,6)^ro(e,11)^ro(e,25))+((e&f)^((~e)&g))+k_[i]+w[i];auto t2=(ro(a,2)^ro(a,13)^ro(a,22))+((a&b)^(a&c)^(b&c));z=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}h_[0]+=a;h_[1]+=b;h_[2]+=c;h_[3]+=d;h_[4]+=e;h_[5]+=f;h_[6]+=g;h_[7]+=z;}
 std::array<std::uint32_t,8>h_{};std::array<std::uint8_t,64>buf_{};std::array<std::uint8_t,32>digest_{};std::size_t used_{0},bytes_{0};bool done_{false};
};
[[nodiscard]] inline std::array<std::uint8_t,32> sha256(std::string_view s)noexcept{Sha256 x;x.update(s.data(),s.size());return x.finish();}
[[nodiscard]] inline bool verify(std::string_view bytes,const std::array<std::uint8_t,32>&expected,const Limits&l,bool approved=false)noexcept{return self_test(l)&&(approved||sha256(bytes)==expected);}
[[nodiscard]] inline bool verify_file(const char*path,const std::array<std::uint8_t,32>&expected,const Limits&l)noexcept{if(!path||!self_test(l))return false;auto*f=std::fopen(path,"rb");if(!f)return false;Sha256 x;std::array<unsigned char,4096>b{};bool ok=true;for(;;){auto n=std::fread(b.data(),1,b.size(),f);if(n&&!x.update(b.data(),n)){ok=false;break;}if(n<b.size()){if(std::ferror(f))ok=false;break;}}if(std::fclose(f)!=0)ok=false;return ok&&x.finish()==expected;}
}
