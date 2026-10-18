// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_char_scalar_air_tests.cpp
//
// Description :
//     유니코드 스칼라 변환의 닫힌 타입·오류 형태와 위조 AIR 거부를 검사한다.
//     Check closed Unicode scalar types, exact error shapes, and malformed AIR rejection.
//
// ========================
#include "aevix/IR/AIR/Module.hpp"
#include <iostream>

namespace
{
    using namespace aevix;
    unsigned checks = 0, failures = 0;
    void expect(bool condition, const char* label)
    { ++checks; if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; } }
    struct Fixture final
    {
        TypeContext types;
        AIRModule module;
        TypeId scalar, word, error, result;
        explicit Fixture(bool from)
        {
            scalar = types.builtin("char"); word = types.builtin("u32");
            error = types.nominal(10,"CharError"); result = types.nominal(20,"Result$char");
            AIRRecord errors; errors.type = error; errors.isEnum = true;
            errors.variants.push_back({11,error,0,{12},{word}});
            AIRRecord carrier; carrier.type = result; carrier.isEnum = true;
            carrier.variants.push_back({21,result,0,{22},{scalar}});
            carrier.variants.push_back({23,result,1,{24},{error}});
            module.records = {errors,carrier};
            AIRFunction function; function.symbol = 1; function.name = "main"; function.result = types.builtin("i32"); function.blocks.resize(1);
            const auto input = from ? word : scalar, output = from ? result : word;
            function.valueTypes = {input,output,function.result};
            AIRInstruction source; source.opcode = AIROpcode::Constant; source.result = 0; source.type = input; source.operation = "65";
            AIRInstruction convert; convert.opcode = AIROpcode::CharScalar; convert.result = 1; convert.type = output;
            convert.operation = from ? "Char.from_u32" : "Char.to_u32"; convert.operands = {0};
            AIRInstruction exit; exit.opcode = AIROpcode::Constant; exit.result = 2; exit.type = function.result; exit.operation = "0";
            function.blocks[0].instructions = {source,convert,exit};
            function.blocks[0].terminator.kind = AIRTerminatorKind::Return; function.blocks[0].terminator.value = 2;
            module.functions.push_back(std::move(function)); module.entry = 1;
        }
        AIRInstruction& operation() { return module.functions[0].blocks[0].instructions[1]; }
        void input(TypeId type) { module.functions[0].valueTypes[0] = type; module.functions[0].blocks[0].instructions[0].type = type; }
        void output(TypeId type) { module.functions[0].valueTypes[1] = type; operation().type = type; }
        bool valid() { Diagnostics diagnostics; return verifyAIR(module,types,diagnostics); }
    };
}
int main()
{
    for (const bool from : {false,true}) {
        Fixture good(from); expect(good.valid(),"exact closed scalar conversion");
        const auto reject = [&](const char* label,const auto& mutation) { Fixture bad(from); mutation(bad); expect(!bad.valid(),label); };
        reject("unknown operation",[](auto& f){f.operation().operation="Char.unchecked";});
        reject("missing input",[](auto& f){f.operation().operands.clear();});
        reject("extra input",[](auto& f){f.operation().operands.push_back(0);});
        reject("missing result",[](auto& f){f.operation().result=InvalidAIRId;});
        reject("signed source",[](auto& f){f.input(f.types.builtin("i32"));});
        reject("wide source",[](auto& f){f.input(f.types.builtin("u64"));});
        reject("pointer-sized source",[](auto& f){f.input(f.types.builtin("usize"));});
        reject("byte source",[](auto& f){f.input(f.types.builtin("byte"));});
        reject("float source",[](auto& f){f.input(f.types.builtin("f32"));});
        reject("borrowed source",[](auto& f){f.input(f.types.reference(f.module.functions[0].valueTypes[0],false));});
        reject("wrong output",[](auto& f){f.output(f.types.builtin("i32"));});
        reject("foreign callee",[](auto& f){f.operation().callee=90;});
        reject("dynamic signature",[](auto& f){f.operation().dynamicSignature=f.word;});
        reject("dynamic targets",[](auto& f){f.operation().dynamicTargets={90};});
        reject("local place",[](auto& f){f.operation().place.local=0;});
        reject("global place",[](auto& f){f.operation().place.global=90;});
        reject("pointer place",[](auto& f){f.operation().place.pointer=0;});
        reject("place type",[](auto& f){f.operation().place.type=f.word;});
        reject("projected place",[](auto& f){f.operation().place.projections.push_back({});});
        reject("atomic metadata",[](auto& f){f.operation().atomicElement=f.word;});
        reject("sync metadata",[](auto& f){f.operation().syncElement=f.word;});
        reject("task payload",[](auto& f){f.operation().taskElement=f.word;});
        reject("task callable",[](auto& f){f.operation().taskCallable=f.word;});
        reject("task invoke",[](auto& f){f.operation().taskInvoke=90;});
        reject("unguarded numeric conversion",[](auto& f){f.operation().opcode=AIROpcode::NumericConvert;});
        reject("numeric status bypass",[](auto& f){f.operation().opcode=AIROpcode::NumericStatus;});
    }
    const auto rejectShape = [&](const char* label,const auto& mutation) { Fixture bad(true); mutation(bad); expect(!bad.valid(),label); };
    rejectShape("missing Result record",[](auto& f){f.module.records.pop_back();});
    rejectShape("Result is a struct",[](auto& f){f.module.records[1].isEnum=false;});
    rejectShape("missing Result error",[](auto& f){f.module.records[1].variants.pop_back();});
    rejectShape("wrong success payload",[](auto& f){f.module.records[1].variants[0].fieldTypes[0]=f.word;});
    rejectShape("wrong success tag",[](auto& f){f.module.records[1].variants[0].tag=1;});
    rejectShape("duplicate success tag",[](auto& f){f.module.records[1].variants[1].tag=0;});
    rejectShape("missing success field",[](auto& f){f.module.records[1].variants[0].fields.clear();});
    rejectShape("wrong variant owner",[](auto& f){f.module.records[1].variants[0].type=f.error;});
    rejectShape("wrong error payload",[](auto& f){f.module.records[1].variants[1].fieldTypes[0]=f.word;});
    rejectShape("wrong error name",[](auto& f){const auto other=f.types.nominal(30,"OtherError");f.module.records[0].type=other;f.module.records[0].variants[0].type=other;f.module.records[1].variants[1].fieldTypes[0]=other;});
    rejectShape("error is a struct",[](auto& f){f.module.records[0].isEnum=false;});
    rejectShape("missing error variant",[](auto& f){f.module.records[0].variants.clear();});
    rejectShape("extra error variant",[](auto& f){auto extra=f.module.records[0].variants[0];extra.tag=1;f.module.records[0].variants.push_back(extra);});
    rejectShape("error missing original",[](auto& f){f.module.records[0].variants[0].fieldTypes.clear();});
    rejectShape("error signed original",[](auto& f){f.module.records[0].variants[0].fieldTypes[0]=f.types.builtin("i32");});
    rejectShape("error truncated original",[](auto& f){f.module.records[0].variants[0].fieldTypes[0]=f.types.builtin("u16");});
    rejectShape("error missing field identity",[](auto& f){f.module.records[0].variants[0].fields.clear();});
    rejectShape("error extra original",[](auto& f){f.module.records[0].variants[0].fieldTypes.push_back(f.word);f.module.records[0].variants[0].fields.push_back(13);});
    rejectShape("error wrong tag",[](auto& f){f.module.records[0].variants[0].tag=1;});
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
