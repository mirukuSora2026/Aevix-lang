// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_float_bits_air_tests.cpp
//
// Description :
//     부동소수 비트 재해석의 정확한 타입·순수 명령 형태와 위조 메타데이터 거부를 검사한다.
//     Check exact float-bit types, pure instruction shape, and forged metadata rejection.
//
// ========================
#include "aevix/IR/AIR/Module.hpp"
#include <iostream>

namespace
{
    using namespace aevix;
    unsigned checks = 0, failures = 0;
    void expect(bool condition, const char* message)
    { ++checks; if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
    struct Fixture final
    {
        TypeContext types;
        AIRModule module;
        TypeId floating, word;
        Fixture(const char* format, bool toBits)
        {
            floating = types.builtin(format);
            word = types.builtin("u" + std::to_string(types.get(floating).bits));
            AIRFunction function; function.symbol = 1; function.name = "main"; function.result = types.builtin("i32"); function.blocks.resize(1);
            const auto input = toBits ? floating : word, output = toBits ? word : floating;
            function.valueTypes = {input, output, function.result};
            AIRInstruction source; source.opcode = AIROpcode::Constant; source.result = 0; source.type = input; source.operation = "0";
            AIRInstruction convert; convert.opcode = AIROpcode::NumericConvert; convert.result = 1; convert.type = output;
            convert.operation = toBits ? "Float.to_bits" : "Float.from_bits"; convert.operands = {0};
            AIRInstruction result; result.opcode = AIROpcode::Constant; result.result = 2; result.type = function.result; result.operation = "0";
            function.blocks[0].instructions = {source,convert,result};
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
    for (const auto format : {"f16","bf16","f32","f64"}) for (const bool toBits : {false,true}) {
        Fixture good(format,toBits); expect(good.valid(),"exact float/unsigned bit pair");
        const auto reject = [&](const char* label, const auto& mutation) { Fixture bad(format,toBits); mutation(bad); expect(!bad.valid(),label); };
        reject("missing operand",[](auto& f){f.operation().operands.clear();});
        reject("extra operand",[](auto& f){f.operation().operands.push_back(0);});
        reject("missing result",[](auto& f){f.operation().result=InvalidAIRId;});
        reject("status opcode",[](auto& f){f.operation().opcode=AIROpcode::NumericStatus;f.operation().dynamicSignature=f.operation().type;f.output(f.types.builtin("i32"));});
        reject("foreign target",[](auto& f){f.operation().callee=90;});
        reject("dynamic target",[](auto& f){f.operation().dynamicTargets={90};});
        reject("dynamic numeric/status metadata",[](auto& f){f.operation().dynamicSignature=f.floating;});
        reject("local storage metadata",[](auto& f){f.operation().place.local=0;});
        reject("global storage metadata",[](auto& f){f.operation().place.global=10;});
        reject("pointer storage metadata",[](auto& f){f.operation().place.pointer=0;});
        reject("place type metadata",[](auto& f){f.operation().place.type=f.floating;});
        reject("projected storage metadata",[](auto& f){f.operation().place.projections.push_back({});});
        reject("atomic metadata",[](auto& f){f.operation().atomicElement=f.word;});
        reject("lock metadata",[](auto& f){f.operation().syncElement=f.word;});
        reject("task result metadata",[](auto& f){f.operation().taskElement=f.word;});
        reject("task capture metadata",[](auto& f){f.operation().taskCallable=f.word;});
        reject("task invoke metadata",[](auto& f){f.operation().taskInvoke=10;});
        const auto badWord = [&](TypeId wrong, Fixture& f){if(toBits) f.output(wrong);else f.input(wrong);};
        reject("signed word",[&](auto& f){badWord(f.types.builtin("i"+std::to_string(f.types.get(f.word).bits)),f);});
        reject("usize is not fixed word",[&](auto& f){badWord(f.types.builtin("usize"),f);});
        reject("isize is not fixed word",[&](auto& f){badWord(f.types.builtin("isize"),f);});
        reject("bool is not raw word",[&](auto& f){badWord(f.types.builtin("bool"),f);});
        reject("byte is distinct",[&](auto& f){badWord(f.types.builtin("byte"),f);});
        reject("wrong unsigned width",[&](auto& f){badWord(f.types.builtin(f.types.get(f.word).bits==64?"u32":"u64"),f);});
        reject("borrowed input",[](auto& f){f.input(f.types.reference(f.module.functions[0].valueTypes[0],false));});
        reject("vector output",[](auto& f){f.output(f.types.vector(f.operation().type));});
        reject("nonfloating side",[&](auto& f){if(toBits)f.input(f.word);else f.output(f.word);});
    }
    // 기존 수치 변환과 bool 반환 적응은 새 비트 재해석 규칙의 대상이 아니다.
    // Existing numeric conversions and bool-return adaptation remain outside the new bit reinterpretation rules.
    Fixture ordinary("f32",true); ordinary.operation().operation="Float.to_integer_exact"; ordinary.output(ordinary.types.builtin("i64")); expect(ordinary.valid(),"existing float/integer conversion remains valid");
    Fixture boolean("f32",true); boolean.operation().operation="return.bool_to_integer"; boolean.input(boolean.types.builtin("bool")); boolean.output(boolean.types.builtin("i32")); expect(boolean.valid(),"existing bool return conversion remains valid");
    const auto numeric = [](const char* name,const char* source,const char* destination,bool status) {
        Fixture value("f32",true); value.operation().operation=name; value.input(value.types.builtin(source));
        value.output(value.types.builtin(status?"i32":destination));
        if(status) { value.operation().opcode=AIROpcode::NumericStatus; value.operation().dynamicSignature=value.types.builtin(destination); }
        return value;
    };
    for (const auto status : {false,true}) {
        for (const auto name : {"Integer.convert","Float.convert","Float.convert_exact","Float.to_integer_exact"}) {
            const bool integer=std::string_view(name)=="Integer.convert", toInteger=std::string_view(name)=="Float.to_integer_exact";
            auto valid=numeric(name,integer?"i64":"f32",integer?"u32":toInteger?"i32":"f64",status);
            expect(valid.valid(),"existing closed numeric domain");
            auto bad=valid;bad.operation().operation="Float.arbitrary_conversion";expect(!bad.valid(),"unknown numeric operation rejected");
            bad=valid;bad.operation().callee=90;expect(!bad.valid(),"numeric external target rejected");
            bad=valid;bad.operation().dynamicTargets={90};expect(!bad.valid(),"numeric dynamic targets rejected");
            bad=valid;bad.operation().place.type=bad.types.builtin("i64");expect(!bad.valid(),"numeric storage metadata rejected");
            bad=valid;bad.operation().taskInvoke=90;expect(!bad.valid(),"numeric task callback rejected");
        }
        expect(!numeric("Integer.convert","f32","i64",status).valid(),"Integer.convert rejects float source");
        expect(!numeric("Integer.convert","i32","f64",status).valid(),"Integer.convert rejects float destination");
        expect(!numeric("Float.convert","f64","i32",status).valid(),"Float.convert rejects integer destination");
        expect(!numeric("Float.convert_exact","bool","f32",status).valid(),"Float.convert_exact rejects boolean source");
        expect(!numeric("Float.to_integer_exact","i32","i64",status).valid(),"Float.to_integer_exact requires float source");
        expect(!numeric("Float.to_integer_exact","f32","f64",status).valid(),"Float.to_integer_exact requires integer destination");
    }
    expect(!numeric("return.bool_to_integer","bool","i32",true).valid(),"bool return has no status form");
    expect(!numeric("return.bool_to_integer","i32","i32",false).valid(),"bool return rejects integer source");
    auto status=numeric("Integer.convert","i32","i64",true);status.output(status.types.builtin("i64"));expect(!status.valid(),"status requires i32 result");
    auto conversion=numeric("Integer.convert","i32","i64",false);conversion.operation().dynamicSignature=conversion.types.builtin("i64");expect(!conversion.valid(),"value conversion rejects status destination metadata");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
