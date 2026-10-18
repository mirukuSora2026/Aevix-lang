// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_range_inspect_air_tests.cpp
//
// Description :
//     범위 조회의 정확한 대여·반환 타입과 닫힌 명령 메타데이터를 검사한다.
//     Check exact borrowed Range inspection types and closed instruction metadata.
//
// ========================
#include "aevix/IR/AIR/Module.hpp"
#include "aevix/Native/RangeOperation.hpp"
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
        TypeId element, descriptor, magnitude;
        Fixture(const char* operation, const char* scalar = "i128", bool mutableLoan = false)
        {
            element = types.builtin(scalar); descriptor = types.range(element);
            magnitude = types.builtin(std::string_view(scalar)=="isize" || std::string_view(scalar)=="usize" ? "usize" : "u"+std::to_string(types.get(element).bits));
            const auto* action = rangeInspectOperation(operation);
            const auto output = action->result == RangeInspectResult::Element ? element : action->result == RangeInspectResult::Magnitude ? magnitude : types.builtin("bool");
            const auto input = types.reference(descriptor,mutableLoan);
            AIRFunction function; function.symbol = 1; function.name = "main"; function.result = types.builtin("i32"); function.blocks.resize(1);
            function.valueTypes = {input,output,function.result};
            AIRInstruction source; source.opcode = AIROpcode::Constant; source.result = 0; source.type = input; source.operation = "0";
            AIRInstruction inspect; inspect.opcode = AIROpcode::RangeInspect; inspect.result = 1; inspect.type = output; inspect.operation = operation; inspect.operands = {0};
            AIRInstruction result; result.opcode = AIROpcode::Constant; result.result = 2; result.type = function.result; result.operation = "0";
            function.blocks[0].instructions = {source,inspect,result};
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
    for (const auto name : {"Range.start","Range.end","Range.step_magnitude","Range.step","Range.is_inclusive","Range.is_descending","Range.is_empty"}) {
        for (const auto scalar : {"i8","u8","i16","u16","i32","u32","i64","u64","i128","u128","isize","usize"})
            for (const auto mutableLoan : {false,true}) { Fixture good(name,scalar,mutableLoan); expect(good.valid(),"integer width and compatible explicit borrow"); }
        const auto reject = [&](const char* label, const auto& change) { Fixture bad(name); change(bad); expect(!bad.valid(),label); };
        reject("unknown getter",[](auto& f){f.operation().operation="Range.cursor_address";});
        reject("missing input",[](auto& f){f.operation().operands.clear();});
        reject("extra input",[](auto& f){f.operation().operands.push_back(0);});
        reject("missing result",[](auto& f){f.operation().result=InvalidAIRId;});
        reject("unborrowed descriptor",[](auto& f){f.input(f.descriptor);});
        reject("nonrange referent",[](auto& f){f.input(f.types.reference(f.element,false));});
        reject("float range",[](auto& f){f.input(f.types.reference(f.types.range(f.types.builtin("f64")),false));});
        reject("bool range",[](auto& f){f.input(f.types.reference(f.types.range(f.types.builtin("bool")),false));});
        reject("wrong scalar output",[](auto& f){f.output(f.types.get(f.operation().type).kind==TypeKind::Bool?f.element:f.types.builtin("bool"));});
        reject("foreign target",[](auto& f){f.operation().callee=90;});
        reject("dynamic target",[](auto& f){f.operation().dynamicTargets={90};});
        reject("dynamic result metadata",[](auto& f){f.operation().dynamicSignature=f.element;});
        reject("local place",[](auto& f){f.operation().place.local=0;});
        reject("global place",[](auto& f){f.operation().place.global=10;});
        reject("pointer place",[](auto& f){f.operation().place.pointer=0;});
        reject("place type",[](auto& f){f.operation().place.type=f.descriptor;});
        reject("projection",[](auto& f){f.operation().place.projections.push_back({});});
        reject("atomic metadata",[](auto& f){f.operation().atomicElement=f.element;});
        reject("sync metadata",[](auto& f){f.operation().syncElement=f.element;});
        reject("task payload",[](auto& f){f.operation().taskElement=f.element;});
        reject("task callable",[](auto& f){f.operation().taskCallable=f.element;});
        reject("task invoke",[](auto& f){f.operation().taskInvoke=10;});
    }
    Fixture fixed("Range.step_magnitude","i64"); fixed.output(fixed.types.builtin("usize")); expect(!fixed.valid(),"fixed i64 magnitude remains u64");
    Fixture pointer("Range.step_magnitude","isize"); pointer.output(pointer.types.builtin("u64")); expect(!pointer.valid(),"pointer-sized magnitude remains usize");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
