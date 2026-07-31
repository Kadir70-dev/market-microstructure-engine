#include "phase6_test.hpp"
#include "risk/limits_verifier.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#ifndef MME_LIMITS_PATH
#error MME_LIMITS_PATH required
#endif
int main(){Phase6Test t;risk::Limits l{};const std::array<std::uint8_t,32> expected={0xac,0xe9,0x0a,0x9c,0xe8,0x9f,0x77,0xc6,0x38,0xcd,0xd7,0x88,0x5b,0x28,0xdc,0x2b,0xa9,0x3d,0x6a,0x87,0x30,0xf0,0xc2,0xb4,0xe2,0xd3,0x52,0x4f,0x3c,0xe9,0xb9,0xdc};t.check(risk::verify_file(MME_LIMITS_PATH,expected,l),"real full digest");auto w=expected;w[0]^=1;t.check(!risk::verify_file(MME_LIMITS_PATH,w,l),"wrong digest");t.check(!risk::verify_file("missing-limits.json",expected,l),"missing");auto dir=std::filesystem::temp_directory_path()/("mme_limits_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(dir);auto truncated=dir/"truncated.json";{std::ofstream f(truncated);f<<"{\n";}t.check(!risk::verify_file(truncated.string().c_str(),expected,l),"truncated");auto modified=dir/"modified.json";std::filesystem::copy_file(MME_LIMITS_PATH,modified);{std::ofstream f(modified,std::ios::app);f<<" ";}t.check(!risk::verify_file(modified.string().c_str(),expected,l),"modified");t.check(!risk::verify_file(dir.string().c_str(),expected,l),"unreadable non-file");std::error_code ec;std::filesystem::remove_all(dir,ec);return t.result();}
