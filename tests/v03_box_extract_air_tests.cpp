// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_box_extract_air_tests.cpp
//
// Description :
//     전체 Box 추출의 소유자·결과·초기화·한 번 소비 및 다른 opcode 메타데이터를 검사한다.
//     Check whole-Box extraction owner/result types, initialization, single consumption and foreign opcode metadata.
//
// ========================
#include "aevix/IR/AIR/Module.hpp"
#include <functional>
#include <iostream>
namespace {
using namespace aevix;
unsigned checks=0,failures=0;
void expect(bool value,const char* label){++checks;if(!value){++failures;std::cerr<<"FAIL: "<<label<<'\n';}}
struct Fixture {
 TypeContext types;AIRModule module;
 TypeId payload=types.builtin("i32");TypeId owner=types.boxed(100,"Box<i32>",payload);
 Fixture(){
  AIRFunction f;f.symbol=1;f.name="main";f.result=payload;f.parameters={0};
  f.locals.push_back({"input",101,owner,false,true,0,nullptr});
  f.locals.push_back({"$box.extract",InvalidSymbolId,owner,false,false,0,nullptr});
  f.scopes.push_back({InvalidAIRId,{0,1}});f.blocks.resize(1);f.valueTypes={owner,payload};
  AIRInstruction begin;begin.opcode=AIROpcode::ScopeBegin;begin.scope=0;f.blocks[0].instructions.push_back(begin);
  AIRInstruction move;move.opcode=AIROpcode::Move;move.type=owner;move.result=0;move.place.local=0;move.place.type=owner;f.blocks[0].instructions.push_back(move);
  AIRInstruction write;write.opcode=AIROpcode::Write;write.type=owner;write.place.local=1;write.place.type=owner;write.operands={0};f.blocks[0].instructions.push_back(write);
  AIRInstruction take;take.opcode=AIROpcode::BoxExtract;take.type=payload;take.result=1;take.place.local=1;take.place.type=owner;take.boxOwner=owner;take.boxElement=payload;take.operation="into_inner";f.blocks[0].instructions.push_back(take);
  f.blocks[0].terminator.kind=AIRTerminatorKind::Return;f.blocks[0].terminator.value=1;
  module.functions.push_back(std::move(f));module.entry=1;
 }
 AIRFunction& function(){return module.functions[0];}
 AIRInstruction& take(){return function().blocks[0].instructions[3];}
 AIRInstruction& init(){return function().blocks[0].instructions[2];}
 bool valid(){Diagnostics d;return verifyAIR(module,types,d);}
};
void reject(const char* name,const std::function<void(Fixture&)>& mutate){Fixture f;mutate(f);expect(!f.valid(),name);}
}
int main(){
 Fixture good;expect(good.valid(),"whole initialized owner accepted");expect(dumpAIR(good.module,good.types).find("box_extract into_inner")!=std::string::npos,"closed operation dump");
 reject("unknown operation",[](auto& f){f.take().operation="copy";});
 reject("missing owner metadata",[](auto& f){f.take().boxOwner=InvalidTypeId;});
 reject("missing payload metadata",[](auto& f){f.take().boxElement=InvalidTypeId;});
 reject("wrong owner metadata",[](auto& f){f.take().boxOwner=f.types.boxed(102,"Box<u64>",f.types.builtin("u64"));});
 reject("wrong payload metadata",[](auto& f){f.take().boxElement=f.types.builtin("u64");});
 reject("wrong result type",[](auto& f){f.take().type=f.types.builtin("bool");f.function().valueTypes[1]=f.take().type;});
 reject("reference owner",[](auto& f){f.take().boxOwner=f.types.reference(f.owner,false);});
 reject("missing result",[](auto& f){f.take().result=InvalidAIRId;});
 reject("unexpected operand",[](auto& f){f.take().operands={0};});
 reject("local out of range",[](auto& f){f.take().place.local=20;});
 reject("missing local",[](auto& f){f.take().place.local=InvalidAIRId;});
 reject("global storage",[](auto& f){f.take().place.global=123;});
 reject("reference storage",[](auto& f){f.take().place.pointer=0;});
 reject("projected storage",[](auto& f){f.take().place.projections.push_back({AIRProjectionKind::Dereference});});
 reject("wrong place type",[](auto& f){f.take().place.type=f.payload;});
 reject("unsealed storage name",[](auto& f){f.function().locals[1].name="user";});
 reject("parameter storage",[](auto& f){f.function().locals[1].isParameter=true;});
 reject("mutable storage",[](auto& f){f.function().locals[1].isMutable=true;});
 reject("symbol-bearing storage",[](auto& f){f.function().locals[1].symbol=123;});
 reject("wrong storage type",[](auto& f){f.function().locals[1].type=f.payload;});
 reject("scope mismatch",[](auto& f){f.take().scope=1;});
 reject("missing cleanup membership",[](auto& f){f.function().scopes[0].locals.pop_back();});
 reject("duplicate cleanup membership",[](auto& f){f.function().scopes[0].locals.push_back(1);});
 reject("no initialization",[](auto& f){f.init().place.local=0;});
 reject("initialization follows extraction",[](auto& f){std::swap(f.function().blocks[0].instructions[2],f.function().blocks[0].instructions[3]);});
 reject("drop between initialization and extraction",[](auto& f){AIRInstruction drop;drop.opcode=AIROpcode::Drop;drop.type=f.owner;drop.place.local=1;drop.place.type=f.owner;f.function().blocks[0].instructions.insert(f.function().blocks[0].instructions.begin()+3,drop);});
 reject("nonadjacent initialization",[](auto& f){AIRInstruction extra;extra.opcode=AIROpcode::ScopeBegin;extra.scope=0;f.function().blocks[0].instructions.insert(f.function().blocks[0].instructions.begin()+3,extra);});
 reject("multiple initialization",[](auto& f){f.function().blocks[0].instructions.push_back(f.init());});
 reject("borrowed duplicate producer",[](auto& f){f.function().blocks[0].instructions[1].opcode=AIROpcode::Read;});
 reject("constant fabricated owner",[](auto& f){auto& i=f.function().blocks[0].instructions[1];i.opcode=AIROpcode::Constant;i.place={};i.operation="null";});
 reject("extra local read",[](auto& f){auto i=f.function().blocks[0].instructions[1];i.place.local=1;i.result=f.function().valueTypes.size();f.function().valueTypes.push_back(f.owner);f.function().blocks[0].instructions.push_back(i);});
 reject("double extraction",[](auto& f){auto i=f.take();i.result=f.function().valueTypes.size();f.function().valueTypes.push_back(f.payload);f.function().blocks[0].instructions.push_back(i);});
 reject("arbitrary callee",[](auto& f){f.take().callee=99;});
 reject("dynamic targets",[](auto& f){f.take().dynamicTargets={99};});
 reject("dynamic signature",[](auto& f){f.take().dynamicSignature=f.payload;});
 reject("atomic metadata",[](auto& f){f.take().atomicElement=f.payload;});
 reject("sync metadata",[](auto& f){f.take().syncElement=f.payload;});
 reject("task element",[](auto& f){f.take().taskElement=f.payload;});
 reject("task callable",[](auto& f){f.take().taskCallable=f.payload;});
 reject("task invoke",[](auto& f){f.take().taskInvoke=42;});
 reject("capture destinations",[](auto& f){f.take().closureLocals={0};});
 reject("metadata on Move",[](auto& f){auto& i=f.function().blocks[0].instructions[1];i.boxOwner=f.owner;i.boxElement=f.payload;});
 reject("metadata on Write",[](auto& f){f.init().boxOwner=f.owner;});
 reject("unsized dynamic payload",[](auto& f){const auto d=f.types.dynamic(105,"Read");f.take().boxOwner=f.types.boxed(106,"Box<dyn Read>",d);f.take().boxElement=d;f.take().type=d;f.function().valueTypes[1]=d;});
 for(const auto* name:{"void","never"})reject(name,[&](auto& f){const auto type=f.types.builtin(name);f.take().boxElement=type;f.take().boxOwner=f.types.boxed(107,"Invalid",type);f.take().type=type;f.function().valueTypes[1]=type;});
 std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
