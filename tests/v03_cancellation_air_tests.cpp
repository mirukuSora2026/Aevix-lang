// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_cancellation_air_tests.cpp
//
// Description :
//     취소 AIR의 정확한 역할과 반환 형태 및 잘못된 메타데이터를 검사한다.
//     Check exact cancellation AIR roles, return shapes, and malformed metadata.
//
// ========================
#include "aevix/IR/AIR/Module.hpp"
#include "aevix/Native/TaskOperation.hpp"
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
        std::size_t index;
        TypeId source, token, error;
        TypeId record(const char* name, std::vector<std::vector<TypeId>> fields)
        {
            const auto type = types.nominal(100 + module.records.size(), name);
            AIRRecord value; value.type = type; value.isEnum = true;
            for (std::size_t i = 0; i < fields.size(); ++i)
            { EnumVariantInfo variant; variant.type = type; variant.tag = i; variant.fieldTypes = fields[i]; value.variants.push_back(std::move(variant)); }
            module.records.push_back(std::move(value)); return type;
        }
        explicit Fixture(const char* name)
        {
            source = types.cancellation(10,"CancellationSource",false);
            token = types.cancellation(11,"CancellationToken",true);
            error = record("CancellationError",{{},{}});
            const auto request = record("CancelRequest",{{},{}});
            const auto& descriptor = *cancellationOperation(name);
            auto output = descriptor.result == CancellationResult::Source ? source : descriptor.result == CancellationResult::Token ? token
                : descriptor.result == CancellationResult::Boolean ? types.builtin("bool") : request;
            if (descriptor.fallible) output = record("Result",{{output},{error}});
            AIRFunction function; function.symbol = 1; function.name = "main"; function.result = types.builtin("i32"); function.blocks.resize(1);
            auto& block = function.blocks[0];
            const auto constant = [&](TypeId type)
            {
                const auto id = function.valueTypes.size(); function.valueTypes.push_back(type);
                AIRInstruction item; item.opcode = AIROpcode::Constant; item.type = type; item.result = id; item.operation = "0";
                block.instructions.push_back(std::move(item)); return id;
            };
            AIRInstruction item; item.opcode = AIROpcode::CancellationIntrinsic; item.operation = name; item.type = output;
            if (descriptor.operands) item.operands = {constant(types.reference(descriptor.input == TypeKind::CancellationSource ? source : token,false))};
            item.result = function.valueTypes.size(); function.valueTypes.push_back(output); index = block.instructions.size(); block.instructions.push_back(std::move(item));
            block.terminator.kind = AIRTerminatorKind::Return; block.terminator.value = constant(function.result);
            module.functions.push_back(std::move(function)); module.entry = 1;
        }
        AIRInstruction& operation() { return module.functions[0].blocks[0].instructions[index]; }
        void input(TypeId type)
        { module.functions[0].valueTypes[0] = type; module.functions[0].blocks[0].instructions[0].type = type; }
        bool valid() { Diagnostics errors; return verifyAIR(module,types,errors); }
    };
}
int main()
{
    for (const auto name : {"source_new","source_clone","token_from_source","token_clone","source_request","token_requested"})
    {
        Fixture good(name); expect(good.valid(),"valid closed cancellation operation");
        auto bad = good; bad.operation().operation = "release_raw"; expect(!bad.valid(),"unknown operation");
        bad = good; bad.operation().operands.push_back(0); expect(!bad.valid(),"extra operand");
        bad = good; bad.operation().result = InvalidAIRId; expect(!bad.valid(),"missing result");
        bad = good; bad.operation().callee = 50; expect(!bad.valid(),"foreign callee metadata");
        bad = good; bad.operation().place.type = bad.source; expect(!bad.valid(),"storage metadata");
        bad = good; bad.operation().atomicElement = bad.source; expect(!bad.valid(),"atomic metadata");
        bad = good; bad.operation().syncElement = bad.token; expect(!bad.valid(),"sync metadata");
        bad = good; bad.operation().dynamicTargets = {50}; expect(!bad.valid(),"dynamic target");
        bad = good; bad.operation().dynamicSignature = bad.types.function(bad.source,{}); expect(!bad.valid(),"dynamic signature");
        bad = good; bad.operation().type = bad.types.builtin("u64"); bad.module.functions[0].valueTypes[bad.operation().result] = bad.operation().type;
        expect(!bad.valid(),"raw integer result");
        if (cancellationOperation(name)->fallible)
        {
            bad = good; for (auto& record : bad.module.records) if (record.type == bad.error) record.variants[1].tag = 0;
            expect(!bad.valid(),"duplicate error tag");
            bad = good; for (auto& record : bad.module.records) if (record.type == bad.error) record.variants[0].fieldTypes = {bad.source};
            expect(!bad.valid(),"error payload ownership forgery");
        }
        if (cancellationOperation(name)->operands)
        {
            bad = good; bad.operation().operands.clear(); expect(!bad.valid(),"missing operand");
            const auto role = cancellationOperation(name)->input;
            bad = good; bad.input(bad.types.reference(role == TypeKind::CancellationSource ? bad.token : bad.source,false)); expect(!bad.valid(),"wrong owner role");
            bad = good; bad.input(role == TypeKind::CancellationSource ? bad.source : bad.token); expect(!bad.valid(),"owner instead of borrow");
            bad = good; bad.input(bad.types.reference(role == TypeKind::CancellationSource ? bad.source : bad.token,true)); expect(!bad.valid(),"unapproved mutable ABI borrow");
        }
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
