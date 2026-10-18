// ========================
//
// Created By : miruku_sora2026
//
// File name : proof_range_tests.cpp
//
// Description :
//     직접 생성한 AIR와 작은 정수 전수 오라클로 구간 증명의 건전성을 검사한다.
//     Check interval-proof soundness with directly constructed AIR and exhaustive small-integer oracles.
//
// ========================
#include "aevix/Optimizer/ProofRangeAnalysis.hpp"
#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <string>
using namespace aevix;
namespace {
std::size_t checks=0,failures=0;
void expect(bool value,const char* label){++checks;if(!value){if(failures<20)std::cerr<<"FAIL "<<label<<'\n';++failures;}}
struct Builder {
    TypeContext types;AIRFunction function;std::size_t block=0;
    Builder(std::size_t blocks=1){function.blocks.resize(blocks);function.scopes.resize(1);function.result=types.builtin("i32");}
    AIRLocalId local(TypeId type,bool parameter=false){const auto id=function.locals.size();function.locals.push_back({"x"+std::to_string(id),InvalidSymbolId,type,true,parameter,0,nullptr});function.scopes[0].locals.push_back(id);if(parameter)function.parameters.push_back(id);return id;}
    AIRValueId emit(AIRInstruction item,bool result=true){if(result){item.result=function.valueTypes.size();function.valueTypes.push_back(item.type);}function.blocks[block].instructions.push_back(item);return item.result;}
    AIRValueId constant(TypeId type,std::string text){AIRInstruction i;i.opcode=AIROpcode::Constant;i.type=type;i.operation=std::move(text);return emit(i);}
    AIRValueId number(TypeId type,long long value){return constant(type,std::to_string(value));}
    AIRValueId read(AIRLocalId id){AIRInstruction i;i.opcode=AIROpcode::Read;i.type=function.locals[id].type;i.place.local=id;i.place.type=i.type;return emit(i);}
    void write(AIRLocalId id,AIRValueId value){AIRInstruction i;i.opcode=AIROpcode::Write;i.type=function.locals[id].type;i.place.local=id;i.place.type=i.type;i.operands={value};emit(i,false);}
    AIRValueId binary(std::string operation,TypeId type,AIRValueId a,AIRValueId b){AIRInstruction i;i.opcode=AIROpcode::Binary;i.operation=std::move(operation);i.type=type;i.operands={a,b};return emit(i);}
    void jump(std::size_t target){function.blocks[block].terminator.kind=AIRTerminatorKind::Jump;function.blocks[block].terminator.target=target;}
    void branch(AIRValueId condition,std::size_t yes,std::size_t no){auto& end=function.blocks[block].terminator;end.kind=AIRTerminatorKind::Branch;end.value=condition;end.target=yes;end.alternative=no;}
    void returned(){function.blocks[block].terminator.kind=AIRTerminatorKind::Return;}
    void index(AIRLocalId array,AIRValueId value,std::size_t extent){AIRInstruction i;i.opcode=AIROpcode::Read;i.type=types.get(function.locals[array].type).element;i.place.local=array;i.place.type=i.type;i.place.projections.push_back({AIRProjectionKind::Index,0,value,i.type,extent});emit(i);}
    ProofRangeResult analyze(const ProofRangeOptions& options={}){return analyzeProofRanges(function,types,options);}
};
void bruteArithmetic(){
    for(bool signedType:{false,true})for(const auto* op:{"+","-","*"}){
        Builder b;const auto t=b.types.builtin(signedType?"i8":"u8");b.number(t,0);b.number(t,0);b.binary(op,t,0,1);b.returned();
        auto& code=b.function.blocks[0].instructions;
        const int low=signedType?-128:0, high=signedType?127:255;
        for(int a=low;a<=high;++a)for(int c=low;c<=high;++c){
            code[0].operation=std::to_string(a);code[1].operation=std::to_string(c);
            const int actual=*op=='+'?a+c:*op=='-'?a-c:a*c;
            const bool safe=actual>=low&&actual<=high;
            const auto proof=b.analyze();expect(proof.arithmeticNoOverflow.contains(&code[2])==safe,"all 8-bit arithmetic pairs match independent int oracle");
        }
    }
}
void bruteBounds(){
    for(bool signedType:{false,true})for(std::size_t extent:{0,1,7,128,255,256}){
        Builder b;const auto t=b.types.builtin(signedType?"i8":"u8");const auto array=b.local(b.types.array(b.types.builtin("i32"),extent));
        b.number(t,0);b.index(array,0,extent);b.returned();auto& code=b.function.blocks[0].instructions;
        for(int value=signedType?-128:0;value<=(signedType?127:255);++value){
            code[0].operation=std::to_string(value);const auto proof=b.analyze();
            expect(proof.fixedArrayIndices.contains(&code[1])==(value>=0&&std::size_t(value)<extent),"all 8-bit index values match independent bounds oracle");
        }
    }
}
void loops(){
    Builder b(5);const auto u=b.types.builtin("usize"),i=b.types.builtin("i32"),flag=b.types.builtin("bool");
    const auto counter=b.local(u),array=b.local(b.types.array(i,4096));b.write(counter,b.number(u,0));b.jump(1);
    b.block=1;const auto read=b.read(counter),limit=b.number(u,4096),test=b.binary("<",flag,read,limit);b.branch(test,2,4);
    b.block=2;b.index(array,b.read(counter),4096);b.jump(3);
    b.block=3;const auto x=b.read(counter),one=b.number(u,1),next=b.binary("+",u,x,one);b.write(counter,next);b.jump(1);
    b.block=4;b.returned();const auto proof=b.analyze();
    expect(proof.stats.skipped==0,"loop dataflow converges");
    expect(proof.fixedArrayIndices.contains(&b.function.blocks[2].instructions[1]),"CFG true-edge proves 4096 loop index");
    expect(proof.arithmeticNoOverflow.contains(&b.function.blocks[3].instructions[2]),"CFG true-edge proves loop increment");
    expect(proof.stats.cfgIterations<30,"per-bound widening terminates promptly");
    ProofRangeOptions tiny;tiny.maxCFGIterations=2;const auto aborted=b.analyze(tiny);
    expect(aborted.stats.budgetFallback&&aborted.arithmeticNoOverflow.empty()&&aborted.fixedArrayIndices.empty(),"budget exhaustion clears every proof");
    // 상한 없는 반복 증가에 첫 반복에서만 성립하는 낙관적인 증명을 유지하면 안 된다.
    // An unconstrained nonterminating increment must not keep its optimistic first iteration proof.
    Builder infinite(2);const auto t=infinite.types.builtin("u8"),local=infinite.local(t);infinite.write(local,infinite.number(t,0));infinite.jump(1);infinite.block=1;
    const auto current=infinite.read(local),unit=infinite.number(t,1),added=infinite.binary("+",t,current,unit);infinite.write(local,added);infinite.jump(1);
    const auto unbounded=infinite.analyze();expect(unbounded.arithmeticNoOverflow.empty(),"final pass rejects eventual overflow in cyclic state");
}
void joinsEffectsVersions(){
    for(bool call:{false,true}){
        Builder b(4);const auto t=b.types.builtin("i8"),flag=b.types.builtin("bool");const auto local=b.local(t),condition=b.local(flag,true);
        b.branch(b.read(condition),1,2);b.block=1;b.write(local,b.number(t,2));b.jump(3);b.block=2;b.write(local,b.number(t,5));b.jump(3);b.block=3;
        if(call){AIRInstruction effect;effect.opcode=AIROpcode::Call;effect.type=b.types.builtin("void");b.emit(effect,false);}
        const auto x=b.read(local),y=b.number(t,120);b.binary("+",t,x,y);b.returned();const auto proof=b.analyze();
        expect(proof.arithmeticNoOverflow.empty()==call,"acyclic join retains union but unknown call kills local facts");
    }
    Builder b(3);const auto t=b.types.builtin("i8"),flag=b.types.builtin("bool");const auto local=b.local(t,true);
    const auto stale=b.read(local);b.write(local,b.number(t,127));const auto limit=b.number(t,10),test=b.binary("<",flag,stale,limit);b.branch(test,1,2);
    b.block=1;const auto x=b.read(local),one=b.number(t,1);b.binary("+",t,x,one);b.returned();b.block=2;b.returned();
    expect(b.analyze().arithmeticNoOverflow.empty(),"old comparison read cannot refine a newer local write");
    Builder alias;const auto a=alias.types.builtin("i8"),id=alias.local(a);alias.write(id,alias.number(a,2));AIRInstruction loan;loan.opcode=AIROpcode::BorrowMut;loan.type=alias.types.reference(a,true);loan.place.local=id;loan.place.type=a;alias.emit(loan);
    const auto v=alias.read(id),n=alias.number(a,120);alias.binary("+",a,v,n);alias.returned();expect(alias.analyze().arithmeticNoOverflow.empty(),"address-taken scalar facts are excluded");
}
void intervalOracle(){
    unsigned random=0xA371u;
    const auto next=[&](){random=random*1664525u+1013904223u;return random;};
    for(bool sign:{false,true})for(unsigned trial=0;trial<64;++trial)for(const auto* op:{"+","-","*"}){
        const int domainLow=sign?-128:0,domainHigh=sign?127:255;
        int loA=int(next()%256)+domainLow,hiA=int(next()%256)+domainLow;
        int loB=int(next()%256)+domainLow,hiB=int(next()%256)+domainLow;
        if(loA>hiA)std::swap(loA,hiA);if(loB>hiB)std::swap(loB,hiB);
        Builder b(6);const auto t=b.types.builtin(sign?"i8":"u8"),flag=b.types.builtin("bool");
        const auto x=b.local(t,true),y=b.local(t,true);
        for(unsigned guard=0;guard<4;++guard){b.block=guard;const auto value=b.read(guard<2?x:y),limit=b.number(t,guard==0?loA:guard==1?hiA:guard==2?loB:hiB);b.branch(b.binary(guard%2?"<=":">=",flag,value,limit),guard+1,5);}
        b.block=4;const auto a=b.read(x),c=b.read(y);b.binary(op,t,a,c);b.returned();b.block=5;b.returned();
        bool safe=true;for(int i=loA;i<=hiA;++i)for(int j=loB;j<=hiB;++j){const int value=*op=='+'?i+j:*op=='-'?i-j:i*j;if(value<domainLow||value>domainHigh)safe=false;}
        const auto proof=b.analyze();expect(proof.arithmeticNoOverflow.contains(&b.function.blocks[4].instructions[2])==safe,"guarded intervals match exhaustive Cartesian oracle");
    }
    Builder b;const auto u=b.types.builtin("u64"),id=b.local(u,true),x=b.read(id),modulus=b.constant(u,"2147483647"),rem=b.binary("%",u,x,modulus),factor=b.number(u,48271),product=b.binary("*",u,rem,factor),one=b.number(u,1);b.binary("+",u,product,one);b.returned();
    expect(b.analyze().arithmeticNoOverflow.size()==2,"unsigned constant remainder bounds LCG multiplication and addition");
    for(bool strict:{false,true}){Builder edge(3);const auto t=edge.types.builtin("u64"),flag=edge.types.builtin("bool"),local=edge.local(t,true);const auto value=edge.read(local),max=edge.constant(t,"18446744073709551615");edge.branch(edge.binary(strict?"<":"<=",flag,value,max),1,2);edge.block=1;const auto read=edge.read(local),one=edge.number(t,1);edge.binary("+",t,read,one);edge.returned();edge.block=2;edge.returned();expect(!edge.analyze().arithmeticNoOverflow.empty()==strict,"near MAX strict bound distinguishes safe and overflowing increments");}
}
void cleanupAndSnapshots(){
    for(unsigned effect=0;effect<3;++effect){
        Builder b(4);const auto t=b.types.builtin("u8"),flag=b.types.builtin("bool");const auto x=b.local(t,true);
        // 불투명 정리 효과를 모델링하는 합성 AIR이며 실제 소스 검증은 source_probe가 별도로 한다.
        // This synthetic AIR models opaque cleanup; source_probe separately verifies real frontend AIR.
        const auto resourceType=b.types.nominal(99,"OpaqueResource"),resource=b.local(resourceType);
        b.function.locals[resource].scope=1;
        std::erase(b.function.scopes[0].locals,resource);
        b.function.scopes.push_back({0,{resource}});
        const auto current=b.read(x),ten=b.number(t,10);b.branch(b.binary("<",flag,current,ten),1,3);
        b.block=1;const auto snapshot=b.read(x);
        if(effect==0){AIRInstruction call;call.opcode=AIROpcode::Call;call.type=b.types.builtin("void");b.emit(call,false);}
        if(effect==1){AIRInstruction drop;drop.opcode=AIROpcode::Drop;drop.type=resourceType;drop.place.type=resourceType;drop.place.local=resource;b.emit(drop,false);}
        const auto eight=b.number(t,8);b.branch(b.binary("<",flag,snapshot,eight),2,3);
        if(effect==2)b.function.blocks[1].terminator.exitedScopes={1};
        b.block=2;const auto one=b.number(t,1);b.binary("+",t,snapshot,one);const auto changed=b.read(x);b.binary("+",t,changed,one);b.returned();
        b.block=3;b.returned();const auto result=b.analyze();const auto& code=b.function.blocks[2].instructions;
        expect(result.arithmeticNoOverflow.contains(&code[1]),"immutable SSA snapshot survives unknown call/drop/edge cleanup");
        expect(!result.arithmeticNoOverflow.contains(&code[3]),"stale compared read never refines current local after effects");
    }
    Builder b(6);const auto t=b.types.builtin("u8"),flag=b.types.builtin("bool"),counter=b.local(t),choice=b.local(flag,true),array=b.local(b.types.array(b.types.builtin("i32"),255));
    b.write(counter,b.number(t,0));b.jump(1);b.block=1;const auto value=b.read(counter),max=b.number(t,255);b.branch(b.binary("<",flag,value,max),2,5);
    b.block=2;b.index(array,b.read(counter),255);b.branch(b.read(choice),3,4);
    b.block=3;const auto x=b.read(counter),one=b.number(t,1),small=b.binary("+",t,x,one);b.write(counter,small);b.jump(1);
    b.block=4;const auto y=b.read(counter),two=b.number(t,2),large=b.binary("+",t,y,two);b.write(counter,large);b.jump(1);b.block=5;b.returned();
    const auto result=b.analyze();
    expect(result.fixedArrayIndices.contains(&b.function.blocks[2].instructions[1]),"differing back edges preserve true-edge array bound");
    expect(result.arithmeticNoOverflow.contains(&b.function.blocks[3].instructions[2]),"safe one-step back edge proved");
    expect(!result.arithmeticNoOverflow.contains(&b.function.blocks[4].instructions[2]),"two-step back edge near MAX keeps overflow check");
    for(unsigned budget=0;budget<4;++budget){ProofRangeOptions options;if(budget==0)options.maxCFGIterations=0;if(budget==1)options.maxInstructionVisits=0;if(budget==2)options.maxStateCells=0;if(budget==3)options.maxBlocks=0;const auto empty=b.analyze(options);expect(empty.stats.budgetFallback&&empty.stats.skipped==1&&empty.arithmeticNoOverflow.empty()&&empty.fixedArrayIndices.empty(),"every zero budget returns empty fallback");}
}
void extremesAndShapes(){
    struct Row{const char* type;const char* a;const char* b;const char* op;bool safe;};
    for(const Row row: {Row{"u64","18446744073709551615","1","+",false},Row{"u64","18446744073709551615","18446744073709551615","*",false},Row{"u64","18446744073709551615","0","+",true},Row{"i64","-9223372036854775808","-1","*",false},Row{"i64","-9223372036854775808","1","+",true},Row{"i128","1","1","+",false},Row{"u128","1","1","+",false},Row{"i16","0x7f_i16","0b1i16","+",true}}){
        Builder b;const auto t=b.types.builtin(row.type);b.constant(t,row.a);b.constant(t,row.b);b.binary(row.op,t,0,1);b.returned();expect(!b.analyze().arithmeticNoOverflow.empty()==row.safe,"wide endpoint or unsupported width behavior");
    }
    for(unsigned shape=0;shape<5;++shape){Builder b;const auto i=b.types.builtin("i32"),index=b.number(i,2);const auto local=b.local(shape==1?b.types.vector(i):b.types.array(i,8));b.index(local,index,shape==2?7:8);auto& item=b.function.blocks[0].instructions[1];
        if(shape==3)item.place.pointer=index;if(shape==4)item.place.projections.insert(item.place.projections.begin(),{AIRProjectionKind::Field});b.returned();const auto result=b.analyze();expect(result.fixedArrayIndices.empty()==(shape!=0),"only validated root fixed-array projection admitted");}
    Builder b;const auto t=b.types.builtin("i8");b.number(t,2);b.number(t,3);b.binary("+",t,0,1);b.number(t,4);b.binary("+",t,2,3);b.returned();ProofRangeOptions limited;limited.maxInstructionVisits=8;const auto result=b.analyze(limited);
    expect(result.stats.budgetFallback&&result.arithmeticNoOverflow.empty()&&result.stats.proven==0,"second-pass budget failure removes earlier collected proofs");
    auto& code=b.function.blocks[0].instructions;code[1].result=code[0].result;expect(b.analyze().stats.skipped==1,"duplicate SSA definition rejected");
}
}
int main(){bruteArithmetic();bruteBounds();loops();joinsEffectsVersions();intervalOracle();cleanupAndSnapshots();extremesAndShapes();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
