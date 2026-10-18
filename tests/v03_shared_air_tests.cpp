// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_shared_air_tests.cpp
//
// Description :
//     공유 소유권 AIR의 닫힌 연산, 페이로드와 오류 반환 형태를 검사한다.
//     Check closed Shared/Weak AIR operations, payloads, and error-carrier shapes.
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
        std::size_t instructionIndex = 0;
        TypeId payload, shared, weak;
        TypeId record(std::vector<std::vector<TypeId>> fields)
        {
            const auto type = types.nominal(100 + module.records.size(), "Carrier");
            AIRRecord result; result.type = type; result.isEnum = true;
            for (std::size_t index = 0; index < fields.size(); ++index)
            { EnumVariantInfo variant; variant.type = type; variant.tag = index; variant.fieldTypes = fields[index]; result.variants.push_back(std::move(variant)); }
            module.records.push_back(std::move(result)); return type;
        }
        explicit Fixture(const std::string& operation)
        {
            payload = types.builtin("i32"); shared = types.shared(10, "Shared", payload); weak = types.shared(11, "Weak", payload, true);
            const bool isWeak = operation.starts_with("weak.");
            const auto name = operation.substr(operation.find('.') + 1);
            const auto owner = isWeak ? weak : shared;
            auto result = types.builtin("unit");
            if (name == "new") result = isWeak ? weak : record({{shared},{record({{payload},{payload},{payload}})}});
            else if (name == "strong_count" || name == "weak_count") result = types.builtin("usize");
            else if (name == "is_empty" || name == "is_expired" || name == "ptr_equal") result = types.builtin("bool");
            else if (name == "as_ref") result = types.reference(payload, false);
            else if (name == "get_mut") result = record({{types.reference(payload, true)}, {}});
            else if (name == "try_unwrap") result = record({{payload},{shared}});
            else if (name != "clear")
            {
                auto value = name == "downgrade" || (isWeak && name == "clone") ? weak : shared;
                if (name == "upgrade") value = record({{value},{}});
                result = record({{value},{record({{}})}});
            }
            AIRFunction function; function.symbol = 1; function.name = "main"; function.result = payload; function.blocks.resize(1);
            auto& block = function.blocks[0];
            const auto constant = [&](TypeId type)
            {
                const auto id = function.valueTypes.size(); function.valueTypes.push_back(type);
                AIRInstruction item; item.opcode = AIROpcode::Constant; item.type = type; item.result = id; item.operation = "0";
                block.instructions.push_back(std::move(item)); return id;
            };
            AIRInstruction item; item.opcode = AIROpcode::SharedIntrinsic; item.type = result; item.operation = operation;
            if (!(isWeak && name == "new")) item.operands.push_back(constant(name == "new" ? payload : name == "try_unwrap" ? owner : types.reference(owner, name == "get_mut" || name == "clear")));
            if (name == "ptr_equal") item.operands.push_back(constant(types.reference(owner, false)));
            item.result = function.valueTypes.size(); function.valueTypes.push_back(result); instructionIndex = block.instructions.size(); block.instructions.push_back(std::move(item));
            block.terminator.kind = AIRTerminatorKind::Return; block.terminator.value = constant(payload);
            module.functions.push_back(std::move(function)); module.entry = 1;
        }
        AIRInstruction& operation() { return module.functions[0].blocks[0].instructions[instructionIndex]; }
        void changeInput(TypeId type)
        { module.functions[0].valueTypes[0] = type; module.functions[0].blocks[0].instructions[0].type = type; }
        bool valid() { Diagnostics errors; return verifyAIR(module, types, errors); }
    };
}

int main()
{
    for (const auto name : {"shared.new", "shared.clone", "shared.as_ref", "shared.downgrade", "shared.get_mut", "shared.try_unwrap", "shared.strong_count", "shared.weak_count", "shared.ptr_equal",
                            "weak.new", "weak.clone", "weak.upgrade", "weak.strong_count", "weak.weak_count", "weak.ptr_equal", "weak.is_empty", "weak.is_expired", "weak.clear"})
    {
        Fixture good(name); expect(good.valid(), "well-typed shared operation");
        auto bad = good; bad.operation().operation = "shared.raw_payload"; expect(!bad.valid(), "unknown operation rejected");
        bad = good; bad.operation().operands.push_back(0); expect(!bad.valid(), "extra operand rejected");
        bad = good; bad.operation().callee = 42; expect(!bad.valid(), "external callee rejected");
        bad = good; bad.operation().place.local = 0; expect(!bad.valid(), "storage base rejected");
        bad = good; bad.operation().place.type = bad.payload; expect(!bad.valid(), "unexpected storage type rejected");
        bad = good; bad.operation().dynamicTargets = {42}; expect(!bad.valid(), "dynamic target rejected");
        bad = good; bad.operation().dynamicSignature = bad.types.function(bad.payload, {}); expect(!bad.valid(), "dynamic signature rejected");
        bad = good; bad.operation().type = bad.types.builtin("byte"); bad.module.functions[0].valueTypes[bad.operation().result] = bad.operation().type;
        expect(!bad.valid(), "incorrect result rejected");
        bad = good; bad.operation().result = InvalidAIRId; expect(!bad.valid(), "missing result rejected");
        if (!good.operation().operands.empty())
        {
            bad = good; bad.operation().operands.pop_back(); expect(!bad.valid(), "missing input rejected");
            bad = good; bad.changeInput(bad.types.reference(bad.types.builtin("bool"), false)); expect(!bad.valid(), "incorrect payload/owner rejected");
        }
        if (std::string(name) == "shared.get_mut" || std::string(name) == "weak.clear")
        { bad = good; bad.changeInput(bad.types.reference(std::string(name).starts_with("weak") ? bad.weak : bad.shared, false)); expect(!bad.valid(), "shared mutation receiver rejected"); }
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
