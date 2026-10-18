// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_memory_air_tests.cpp
//
// Description :
//     메모리 교체·교환 AIR의 타입, 피연산자와 독점 대여 불변식을 검사한다.
//     Check type, operand, and exclusive-loan invariants for memory replacement and swap AIR.
//
// ========================

#include "aevix/IR/AIR/Module.hpp"
#include <iostream>
#include <string>

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
        Fixture(const std::string& operation)
        {
            const auto integer = types.builtin("i32"), reference = types.reference(integer, true);
            AIRFunction function; function.symbol = 1; function.name = "main"; function.result = integer;
            function.blocks.resize(1); auto& block = function.blocks[0];
            const auto constant = [&](TypeId type, std::string text)
            {
                const auto id = function.valueTypes.size(); function.valueTypes.push_back(type);
                AIRInstruction value; value.opcode = AIROpcode::Constant; value.type = type; value.result = id; value.operation = std::move(text);
                block.instructions.push_back(std::move(value)); return id;
            };
            const auto left = constant(reference, "null"), right = constant(operation == "replace" ? integer : reference, "0");
            AIRInstruction memory; memory.opcode = AIROpcode::MemoryIntrinsic; memory.operation = operation;
            memory.type = operation == "replace" ? integer : types.builtin("unit"); memory.result = function.valueTypes.size();
            function.valueTypes.push_back(memory.type); memory.operands = {left, right}; block.instructions.push_back(memory);
            block.terminator.kind = AIRTerminatorKind::Return; block.terminator.value = constant(integer, "0");
            module.functions.push_back(std::move(function)); module.entry = 1;
        }
        AIRInstruction& operation() { return module.functions[0].blocks[0].instructions[2]; }
        bool valid() { Diagnostics errors; return verifyAIR(module, types, errors); }
    };
}

int main()
{
    for (const auto name : {"replace", "swap"})
    {
        Fixture good(name); expect(good.valid(), "well-typed memory operation");
        auto bad = good; bad.operation().operation = "copy"; expect(!bad.valid(), "unknown operation rejected");
        bad = good; bad.operation().operands.pop_back(); expect(!bad.valid(), "missing operand rejected");
        bad = good; bad.operation().callee = 42; expect(!bad.valid(), "arbitrary external callee rejected");
        bad = good; bad.operation().place.local = 0; expect(!bad.valid(), "unexpected storage base rejected");
        bad = good; bad.module.functions[0].valueTypes[0] = bad.types.reference(bad.types.builtin("i32"), false);
        bad.module.functions[0].blocks[0].instructions[0].type = bad.module.functions[0].valueTypes[0];
        expect(!bad.valid(), "shared target rejected");
        bad = good; bad.module.functions[0].valueTypes[1] = bad.types.builtin("bool");
        bad.module.functions[0].blocks[0].instructions[1].type = bad.types.builtin("bool");
        expect(!bad.valid(), "incompatible second operand rejected");
        bad = good; bad.operation().type = bad.types.builtin("bool"); bad.module.functions[0].valueTypes[2] = bad.types.builtin("bool");
        expect(!bad.valid(), "incompatible result rejected");
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
