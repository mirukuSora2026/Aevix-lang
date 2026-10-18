// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_vector_mutation_air_tests.cpp
//
// Description :
//     동적 배열 변경 AIR의 독점 저장소, 원소 타입과 닫힌 연산 집합을 검사한다.
//     Check exclusive storage, element types, and the closed set of vector mutation AIR operations.
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
        explicit Fixture(const std::string& operation)
        {
            const auto integer = types.builtin("i32"), vector = types.vector(integer);
            AIRFunction function; function.symbol = 1; function.name = "main"; function.result = integer;
            function.blocks.resize(1); auto& block = function.blocks[0];
            const auto constant = [&](TypeId type, const char* text)
            {
                const auto id = function.valueTypes.size(); function.valueTypes.push_back(type);
                AIRInstruction value; value.opcode = AIROpcode::Constant; value.type = type; value.result = id; value.operation = text;
                block.instructions.push_back(std::move(value)); return id;
            };
            const auto receiver = constant(types.reference(vector, true), "null");
            const auto index = constant(types.builtin("usize"), "0"), input = constant(integer, "1");
            AIRInstruction mutation;
            mutation.opcode = operation == "insert" ? AIROpcode::VectorInsert
                : operation == "clear" || operation == "truncate" ? AIROpcode::VectorTruncate : AIROpcode::VectorTake;
            mutation.operation = operation;
            mutation.type = mutation.opcode == AIROpcode::VectorTake ? integer : types.builtin("unit");
            mutation.result = function.valueTypes.size(); function.valueTypes.push_back(mutation.type);
            mutation.place.pointer = receiver; mutation.place.type = vector; mutation.operands = {index};
            if (operation == "insert") mutation.operands.push_back(input);
            block.instructions.push_back(mutation);
            block.terminator.kind = AIRTerminatorKind::Return; block.terminator.value = constant(integer, "0");
            module.functions.push_back(std::move(function)); module.entry = 1;
        }
        AIRInstruction& operation() { return module.functions[0].blocks[0].instructions[3]; }
        void changeInput(std::size_t index, TypeId type)
        { module.functions[0].valueTypes[index] = type; module.functions[0].blocks[0].instructions[index].type = type; }
        bool valid() { Diagnostics errors; return verifyAIR(module, types, errors); }
    };
}

int main()
{
    for (const auto name : {"pop", "remove", "swap_remove", "clear", "truncate", "insert"})
    {
        Fixture good(name); expect(good.valid(), "well-typed vector mutation");
        auto bad = good; bad.operation().operation = "erase_raw"; expect(!bad.valid(), "unknown operation rejected");
        bad = good; bad.operation().operands.pop_back(); expect(!bad.valid(), "missing operand rejected");
        bad = good; bad.operation().operands.push_back(1); expect(!bad.valid(), "extra operand rejected");
        bad = good; bad.operation().callee = 42; expect(!bad.valid(), "arbitrary callee rejected");
        bad = good; bad.operation().place.local = 0; expect(!bad.valid(), "unexpected local base rejected");
        bad = good; bad.changeInput(0, bad.types.reference(bad.operation().place.type, false)); expect(!bad.valid(), "shared receiver rejected");
        bad = good; bad.changeInput(0, bad.types.reference(bad.types.builtin("i32"), true)); expect(!bad.valid(), "nonvector receiver rejected");
        bad = good; bad.changeInput(1, bad.types.builtin("i32")); expect(!bad.valid(), "signed index rejected");
        bad = good; bad.operation().type = bad.types.builtin("bool"); bad.module.functions[0].valueTypes[3] = bad.operation().type;
        expect(!bad.valid(), "wrong result rejected");
        bad = good; bad.operation().result = InvalidAIRId; expect(!bad.valid(), "missing result rejected");
        if (std::string(name) == "insert")
        { bad = good; bad.changeInput(2, bad.types.builtin("bool")); expect(!bad.valid(), "insertion element mismatch rejected"); }
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
