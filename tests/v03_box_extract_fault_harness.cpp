// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_box_extract_fault_harness.cpp
//
// Description :
//     생성된 Box 할당·해제를 추적하고 각 할당 실패에서 입력과 정확한 정리를 검사한다.
//     Track generated Box allocation/free calls and check retained input and exact cleanup at every allocation failure.
//
// ========================
#include <cstdint>
#include <cstdlib>
#include <iostream>
#ifdef AEVIX_BOX_JIT_TEST
#include "aevix/JIT/JITEngine.hpp"
#include <fstream>
#include <iterator>
#endif
namespace {
unsigned attempts=0,releases=0,live=0,failAt=0;
void* pointers[16]{};
void reset(unsigned stage){attempts=releases=live=0;failAt=stage;for(auto& p:pointers)p=nullptr;}
bool complete(){const auto calls=failAt==1?1u:2u;const auto frees=failAt==1?0u:failAt==2?1u:2u;return attempts==calls&&releases==frees&&live==0;}
}
extern "C" void* box_fault_allocate(std::uint64_t size){
 ++attempts;if(failAt&&attempts==failAt)return nullptr;
 auto* result=std::malloc(static_cast<std::size_t>(size));if(!result)std::abort();
 for(auto& slot:pointers)if(!slot){slot=result;++live;return result;}
 std::abort();
}
extern "C" void box_fault_release(void* pointer){
 if(!pointer)return;for(auto& slot:pointers)if(slot==pointer){slot=nullptr;++releases;--live;std::free(pointer);return;}std::abort();
}
#ifdef AEVIX_BOX_JIT_TEST
int main(int argc,char** argv){
 if(argc!=3)return 30;if(!aevix::JITEngine::available())return 77;
 std::ifstream file(argv[1]);std::string ir{std::istreambuf_iterator<char>(file),{}};
 if(ir.empty())return 31;
 for(unsigned stage=0;stage<3;++stage){
  reset(stage);std::string error;aevix::JITOptions options;options.optimization=static_cast<unsigned>(std::atoi(argv[2]));
  auto engine=aevix::JITEngine::create(options,error);
  if(!engine||!engine->defineSymbol("box_fault_allocate",reinterpret_cast<std::uintptr_t>(&box_fault_allocate),error)
   ||!engine->defineSymbol("box_fault_release",reinterpret_cast<std::uintptr_t>(&box_fault_release),error)||!engine->addModule(ir,error)){std::cerr<<error<<'\n';return 32;}
  const auto result=engine->invokeI32();engine.reset();
  if(!result.succeeded||result.value!=0||!complete()){std::cerr<<"stage "<<stage<<" result "<<result.value<<" calls "<<attempts<<" frees "<<releases<<" live "<<live<<'\n';return 40+stage;}
 }
 std::cout<<"3 exact allocator/failure stages passed\n";return 0;
}
#else
extern "C" int box_fault_main();
int main(int argc,char** argv){
 if(argc!=2)return 30;reset(static_cast<unsigned>(std::atoi(argv[1])));if(failAt>2)return 31;
 const auto result=box_fault_main();if(result||!complete()){std::cerr<<"result "<<result<<" calls "<<attempts<<" frees "<<releases<<" live "<<live<<'\n';return 40+result;}
 return 0;
}
#endif
