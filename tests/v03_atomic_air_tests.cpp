// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_atomic_air_tests.cpp
//
// Description :
//     원자 AIR의 스칼라 프로필, 순서 열거형과 닫힌 반환 형태를 검사한다.
//     Check atomic AIR scalar profiles, ordering enums, and closed return shapes.
//
// ========================
#include "aevix/IR/AIR/Module.hpp"
#include "aevix/Native/AtomicOperation.hpp"
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
        std::size_t instructionIndex = 0;
        TypeId scalar, cell, order;
        TypeId record(const std::string& name, std::vector<std::vector<TypeId>> fields, bool isEnum)
        {
            const auto type = types.nominal(100 + module.records.size(), name);
            AIRRecord result; result.type = type; result.isEnum = isEnum;
            if (!isEnum) result.fieldTypes = fields[0];
            else for (std::size_t index = 0; index < fields.size(); ++index)
            { EnumVariantInfo variant; variant.type = type; variant.tag = index; variant.fieldTypes = fields[index]; result.variants.push_back(std::move(variant)); }
            module.records.push_back(std::move(result)); return type;
        }
        explicit Fixture(const std::string& name)
        {
            const auto& operation = *atomicOperation(name);
            scalar = types.builtin(name == "fence" || name == "compiler_fence" ? "unit" : "i32");
            cell = types.atomic(10, "Atomic", scalar);
            order = record("MemoryOrder", {{},{},{},{},{}}, true);
            const auto error = record("AtomicError", {{},{},{}}, true);
            const auto duration = record("Duration", {{types.builtin("u64")}}, false);
            auto returned = types.builtin("unit");
            switch (operation.result)
            {
                case AtomicResult::Cell: returned = cell; break;
                case AtomicResult::Scalar: returned = scalar; break;
                case AtomicResult::Unit: break;
                case AtomicResult::MutableReference: returned = types.reference(scalar, true); break;
                case AtomicResult::Cas: returned = record("CasOutcome", {{types.builtin("bool"), scalar}}, false); break;
                case AtomicResult::Wait: returned = record("WaitOutcome", {{scalar, types.builtin("bool"), types.builtin("bool")}}, false); break;
                case AtomicResult::Boolean: returned = types.builtin("bool"); break;
                case AtomicResult::Alignment: returned = types.builtin("usize"); break;
            }
            if (operation.fallible) returned = record("Result", {{returned}, {error}}, true);
            AIRFunction function; function.symbol = 1; function.name = "main"; function.result = types.builtin("i32"); function.blocks.resize(1);
            auto& block = function.blocks[0];
            const auto constant = [&](TypeId type)
            {
                const auto id = function.valueTypes.size(); function.valueTypes.push_back(type);
                AIRInstruction item; item.opcode = AIROpcode::Constant; item.type = type; item.result = id; item.operation = "0";
                block.instructions.push_back(std::move(item)); return id;
            };
            AIRInstruction item; item.opcode = AIROpcode::AtomicIntrinsic; item.type = returned; item.operation = name; item.atomicElement = scalar;
            for (std::size_t index = 0; index < operation.operands; ++index)
            {
                TypeId input = scalar;
                if (operation.cell && index == 0) input = name == "into_inner" ? cell : types.reference(cell, name == "get_mut");
                else if (static_cast<int>(index) == operation.order || static_cast<int>(index) == operation.failure) input = order;
                else if (name == "wait_timeout" && index == 2) input = duration;
                item.operands.push_back(constant(input));
            }
            item.result = function.valueTypes.size(); function.valueTypes.push_back(returned); instructionIndex = block.instructions.size(); block.instructions.push_back(std::move(item));
            block.terminator.kind = AIRTerminatorKind::Return; block.terminator.value = constant(function.result);
            module.functions.push_back(std::move(function)); module.entry = 1;
        }
        AIRInstruction& operation() { return module.functions[0].blocks[0].instructions[instructionIndex]; }
        void changeInput(std::size_t index, TypeId type)
        { module.functions[0].valueTypes[index] = type; module.functions[0].blocks[0].instructions[index].type = type; }
        bool valid() { Diagnostics errors; return verifyAIR(module, types, errors); }
    };
}
int main()
{
    for (const auto name : {"new","load","store","swap","compare_exchange","compare_exchange_weak","fetch_add","fetch_sub","fetch_and","fetch_or","fetch_xor","fetch_nand","fetch_min","fetch_max","checked_fetch_add","checked_fetch_sub","saturating_fetch_add","saturating_fetch_sub","get_mut","into_inner","is_supported","is_lock_free","required_alignment","fence","compiler_fence","wait","wait_timeout","notify_one","notify_all"})
    {
        Fixture good(name); expect(good.valid(), "well-typed atomic operation");
        auto bad = good; bad.operation().operation = "raw_compare_exchange"; expect(!bad.valid(), "unknown atomic operation rejected");
        bad = good; bad.operation().operands.push_back(0); expect(!bad.valid(), "extra operand rejected");
        bad = good; bad.operation().atomicElement = InvalidTypeId; expect(!bad.valid(), "missing scalar metadata rejected");
        bad = good; bad.operation().callee = 42; expect(!bad.valid(), "external callee rejected");
        bad = good; bad.operation().place.type = bad.scalar; expect(!bad.valid(), "unexpected storage metadata rejected");
        bad = good; bad.operation().dynamicTargets = {42}; expect(!bad.valid(), "dynamic target rejected");
        bad = good; bad.operation().dynamicSignature = bad.types.function(bad.scalar, {}); expect(!bad.valid(), "dynamic signature rejected");
        bad = good; bad.operation().type = bad.types.builtin("byte"); bad.module.functions[0].valueTypes[bad.operation().result] = bad.operation().type; expect(!bad.valid(), "wrong result rejected");
        bad = good; bad.operation().result = InvalidAIRId; expect(!bad.valid(), "missing result rejected");
        if (!good.operation().operands.empty())
        {
            bad = good; bad.operation().operands.pop_back(); expect(!bad.valid(), "missing operand rejected");
            bad = good; bad.changeInput(0, bad.types.builtin("byte")); expect(!bad.valid(), "incompatible first operand rejected");
        }
        if (std::string(name) == "get_mut")
        { bad = good; bad.changeInput(0, bad.types.reference(bad.cell, false)); expect(!bad.valid(), "read-only cell rejected for exclusive scalar access"); }
        if (atomicOperation(name)->order >= 0)
        {
            bad = good;
            for (auto& record : bad.module.records) if (record.type == bad.order) record.variants[0].tag = 9;
            expect(!bad.valid(), "invalid ordering enum tags rejected");
        }
    }
    Fixture unsupported("is_supported"); unsupported.operation().atomicElement = unsupported.types.builtin("i128");
    expect(unsupported.valid(), "unsupported width remains queryable without constructing an atomic cell");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
