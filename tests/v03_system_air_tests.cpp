// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_system_air_tests.cpp
//
// Description :
//     변조한 시스템 AIR의 중첩 오류·선택 값·로그 수준 페이로드를 실행 전에 거부한다.
//     Reject malformed nested error, Optional, and log-level payloads in system AIR before execution.
//
// ========================
#include "aevix/IR/AIR/Module.hpp"
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace
{
    using namespace aevix;
    unsigned checks = 0, failures = 0;
    void expect(bool condition, const char* name)
    { ++checks; if (!condition) { ++failures; std::cerr << "FAIL: " << name << '\n'; } }
    struct Fixture final
    {
        TypeContext types;
        AIRModule module;
        TypeId io, parse, read, level, optional, result;
        explicit Fixture(const std::string& operation)
        {
            io = types.nominal(1, "IoError"); parse = types.nominal(2, "ParseError");
            read = types.nominal(3, "ReadError"); level = types.nominal(4, "LogLevel");
            optional = types.nominal(5, "Optional"); result = types.nominal(6, "Result");
            const auto i32 = types.builtin("i32"), usize = types.builtin("usize"), unit = types.builtin("unit"), text = types.builtin("string_view");
            add(io, {{i32,usize},{i32,usize},{i32},{},{},{},{},{},{},{},{}});
            add(parse, {{},{},{},{},{}}); add(read, {{io},{parse}}); add(level, {{},{},{}});
            add(optional, {{i32},{}}); add(result, {{operation == "input" ? optional : unit},{operation == "input" ? read : io}});
            AIRFunction function; function.symbol = 42; function.name = "main"; function.result = i32; function.blocks.resize(1);
            auto& block = function.blocks[0];
            const auto constant = [&](TypeId type, const char* spelling)
            {
                const auto id = function.valueTypes.size(); function.valueTypes.push_back(type);
                AIRInstruction instruction; instruction.opcode = AIROpcode::Constant; instruction.result = id; instruction.type = type; instruction.operation = spelling;
                block.instructions.push_back(std::move(instruction)); return id;
            };
            std::vector<AIRValueId> operands;
            if (operation == "log") { operands.push_back(constant(level,"zeroinitializer")); operands.push_back(constant(text,"\"message\"")); operands.push_back(constant(text,"\"\"")); }
            else if (operation != "input") operands.push_back(constant(text,"\"message\""));
            AIRInstruction instruction; instruction.opcode = AIROpcode::SystemIntrinsic; instruction.type = result;
            instruction.result = function.valueTypes.size(); instruction.operation = operation; instruction.operands = std::move(operands);
            if (operation == "input") instruction.dynamicSignature = i32;
            function.valueTypes.push_back(result); block.instructions.push_back(std::move(instruction));
            block.terminator.kind = AIRTerminatorKind::Return; block.terminator.value = constant(i32,"0");
            module.functions.push_back(std::move(function)); module.entry = 42;
        }
        void add(TypeId type, const std::vector<std::vector<TypeId>>& fields)
        {
            AIRRecord record; record.type = type; record.isEnum = true;
            for (std::size_t tag = 0; tag < fields.size(); ++tag)
            {
                EnumVariantInfo variant; variant.variant = 100 + module.records.size() * 20 + tag;
                variant.type = type; variant.tag = tag; variant.fieldTypes = fields[tag]; record.variants.push_back(std::move(variant));
            }
            module.records.push_back(std::move(record));
        }
        AIRRecord& record(TypeId type)
        { for (auto& record : module.records) if (record.type == type) return record; std::abort(); }
        bool valid()
        { Diagnostics diagnostics; return verifyAIR(module, types, diagnostics); }
    };
}

int main()
{
    using namespace aevix;
    for (const auto operation : {"print", "input", "log"})
    {
        Fixture good(operation); expect(good.valid(), "complete system carrier is valid");
        for (const auto tag : {0u,1u,2u,6u,8u,9u,10u})
        {
            auto bad = good; bad.record(bad.io).variants[tag].fieldTypes.push_back(bad.types.builtin("bool"));
            expect(!bad.valid(), "wrong native IO error payload is rejected");
        }
        auto bad = good; bad.record(bad.io).variants.pop_back(); expect(!bad.valid(), "missing IoError variant is rejected");
        bad = good; bad.record(bad.io).variants[1].tag = 0; expect(!bad.valid(), "duplicated IoError tags are rejected");
        bad = good; bad.record(bad.result).variants[1].tag = 0; expect(!bad.valid(), "duplicated Result tags are rejected");
    }
    Fixture input("input");
    for (const auto tag : {0u,1u})
    {
        auto bad = input; bad.record(bad.read).variants[tag].fieldTypes.clear(); expect(!bad.valid(), "missing ReadError nested payload is rejected");
        bad = input; bad.record(bad.read).variants[tag].fieldTypes = {bad.types.builtin("i32")}; expect(!bad.valid(), "wrong ReadError nested type is rejected");
    }
    auto bad = input; bad.record(bad.parse).variants[0].fieldTypes = {bad.types.builtin("i32")}; expect(!bad.valid(), "ParseError cannot unexpectedly own a payload");
    bad = input; bad.record(bad.parse).variants[4].tag = 0; expect(!bad.valid(), "duplicated ParseError tags are rejected");
    bad = input; bad.record(bad.optional).variants[0].fieldTypes = {bad.types.builtin("f64")}; expect(!bad.valid(), "Optional scalar must match input destination");
    bad = input; bad.record(bad.optional).variants[1].tag = 0; expect(!bad.valid(), "duplicated Optional tags are rejected");
    Fixture log("log"); bad = log; bad.record(bad.level).variants[0].fieldTypes = {bad.types.builtin("i32")}; expect(!bad.valid(), "LogLevel payload is rejected");
    bad = log; bad.record(bad.level).variants[2].tag = 1; expect(!bad.valid(), "duplicated LogLevel tags are rejected");
    bad = log; bad.record(bad.level).variants.pop_back(); expect(!bad.valid(), "missing LogLevel tag is rejected");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
