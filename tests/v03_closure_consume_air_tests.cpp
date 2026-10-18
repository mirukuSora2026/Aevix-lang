// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_closure_consume_air_tests.cpp
//
// Description :
//     FnOnce 전체 환경 분해의 정확한 소유자·목적지·진입 위치를 검증한다.
//     Verify exact ownership, destinations, and entry placement for complete FnOnce environment splitting.
//
// ========================
#include "aevix/IR/AIR/Module.hpp"
#include <functional>
#include <iostream>

namespace
{
    using namespace aevix;
    unsigned checks = 0, failures = 0;
    void expect(bool value, const char* name)
    { ++checks; if (!value) { ++failures; std::cerr << "FAIL: " << name << '\n'; } }
    struct Fixture final
    {
        TypeContext types;
        AIRModule module;
        NodePtr source = makeNode("Struct", "$Closure1");
        TypeId environment = types.nominal(100, "$Closure1");
        Fixture()
        {
            source->children.push_back(makeNode("CompilerClosure", "FnOnce"));
            auto invoke = makeNode("Function", "__invoke");
            invoke->children.push_back(makeNode("Block", ""));
            const auto* body = invoke.get(); source->children.push_back(std::move(invoke));
            AIRRecord record; record.type = environment; record.source = source.get();
            record.fields = {101, 102, 103};
            record.fieldTypes = {types.builtin("string"), types.reference(types.builtin("i32"), true), types.builtin("i32")};
            module.records.push_back(record);
            AIRFunction function; function.symbol = 1; function.name = "__invoke"; function.source = body;
            function.result = types.builtin("i32"); function.parameters = {0, 1};
            function.locals.push_back({"self", 10, environment, true, true, 0, nullptr});
            function.locals.push_back({"argument", 11, types.builtin("string"), false, true, 0, nullptr});
            for (std::size_t field = 0; field < record.fieldTypes.size(); ++field)
                function.locals.push_back({"$capturelocal." + std::to_string(field), InvalidSymbolId, record.fieldTypes[field], true, false, 0, nullptr});
            function.scopes.push_back({InvalidAIRId, {0, 2, 3, 4, 1}});
            function.blocks.resize(1);
            AIRInstruction begin; begin.opcode = AIROpcode::ScopeBegin; begin.scope = 0;
            function.blocks[0].instructions.push_back(begin);
            AIRInstruction consume; consume.opcode = AIROpcode::ClosureConsume; consume.type = environment;
            consume.operation = "consume"; consume.source = body; consume.place.local = 0;
            consume.place.type = environment; consume.closureLocals = {2, 3, 4};
            function.blocks[0].instructions.push_back(consume);
            AIRInstruction value; value.opcode = AIROpcode::Constant; value.type = function.result;
            value.operation = "0"; value.result = 0; function.valueTypes.push_back(function.result);
            function.blocks[0].instructions.push_back(value);
            function.blocks[0].terminator.kind = AIRTerminatorKind::Return;
            function.blocks[0].terminator.value = 0;
            module.functions.push_back(std::move(function)); module.entry = 1;
        }
        AIRFunction& function() { return module.functions[0]; }
        AIRInstruction& operation() { return function().blocks[0].instructions[1]; }
        bool valid() { Diagnostics diagnostics; return verifyAIR(module, types, diagnostics); }
    };
    void reject(const char* name, const std::function<void(Fixture&)>& mutate)
    { Fixture fixture; mutate(fixture); expect(!fixture.valid(), name); }
}

int main()
{
    Fixture valid; expect(valid.valid(), "complete owned environment is accepted");
    Fixture empty; empty.module.records[0].fields.clear(); empty.module.records[0].fieldTypes.clear();
    empty.operation().closureLocals.clear(); empty.function().locals.resize(2); empty.function().scopes[0].locals = {0, 1};
    expect(empty.valid(), "zero-capture owned invocation is accepted");
    expect(dumpAIR(valid.module, valid.types).find("closure_consume consume") != std::string::npos, "dump names the closed operation");
    reject("ordinary record", [](auto& f) { f.source->children[0]->kind = "UserAttribute"; });
    reject("Fn receiver", [](auto& f) { f.source->children[0]->value = "Fn"; });
    reject("FnMut receiver", [](auto& f) { f.source->children[0]->value = "FnMut"; });
    reject("duplicate marker", [](auto& f) { f.source->children.push_back(makeNode("CompilerClosure", "FnOnce")); });
    reject("enum environment", [](auto& f) { f.module.records[0].isEnum = true; });
    reject("non-struct source", [](auto& f) { f.source->kind = "Enum"; });
    reject("custom Drop environment", [](auto& f) { auto contracts = makeNode("Implements", ""); contracts->children.push_back(makeNode("Type", "Drop")); f.source->children.push_back(std::move(contracts)); });
    reject("unrelated invoke source", [](auto& f) { f.function().source = f.source.get(); });
    reject("missing invoke source", [](auto& f) { f.function().source = nullptr; });
    reject("wrong source method name", [](auto& f) { f.source->children[1]->value = "other"; });
    reject("wrong function name", [](auto& f) { f.function().name = "other"; });
    reject("duplicate invocation", [](auto& f) { f.source->children.push_back(makeNode("Function", "__invoke")); });
    reject("unrelated operation source", [](auto& f) { f.operation().source = f.source.get(); });
    reject("missing receiver", [](auto& f) { f.function().parameters.clear(); });
    reject("wrong first parameter", [](auto& f) { std::swap(f.function().parameters[0], f.function().parameters[1]); });
    reject("borrowed receiver", [](auto& f) { f.function().locals[0].type = f.types.reference(f.environment, true); });
    reject("renamed receiver", [](auto& f) { f.function().locals[0].name = "other"; });
    reject("nonparameter receiver", [](auto& f) { f.function().locals[0].isParameter = false; });
    reject("missing destination", [](auto& f) { f.operation().closureLocals.pop_back(); });
    reject("extra destination", [](auto& f) { f.operation().closureLocals.push_back(4); });
    reject("duplicate destination", [](auto& f) { f.operation().closureLocals[1] = 2; });
    reject("destination aliases receiver", [](auto& f) { f.operation().closureLocals[0] = 0; });
    reject("destination aliases parameter", [](auto& f) { f.operation().closureLocals[0] = 1; });
    reject("out-of-range destination", [](auto& f) { f.operation().closureLocals[0] = 99; });
    reject("destination type mismatch", [](auto& f) { f.function().locals[2].type = f.types.builtin("u64"); });
    reject("destination mutability escalation", [](auto& f) { f.function().locals[0].isMutable = false; });
    reject("destination mutability mismatch", [](auto& f) { f.function().locals[2].isMutable = false; });
    reject("destination loses mutable-reference type", [](auto& f) { f.function().locals[3].type = f.types.reference(f.types.builtin("i32"), false); });
    reject("destination name mismatch", [](auto& f) { f.function().locals[2].name = "ordinary"; });
    reject("destination marked parameter", [](auto& f) { f.function().locals[2].isParameter = true; });
    reject("cleanup capture order changed", [](auto& f) { std::swap(f.function().scopes[0].locals[1], f.function().scopes[0].locals[2]); });
    reject("capture cleanup after argument", [](auto& f) { std::swap(f.function().scopes[0].locals[1], f.function().scopes[0].locals[4]); });
    reject("duplicate cleanup destination", [](auto& f) { f.function().scopes[0].locals.push_back(2); });
    reject("destination in multiple scopes", [](auto& f) { f.function().scopes.push_back({0, {2}}); });
    reject("not a root scope", [](auto& f) { f.function().scopes[0].parent = 0; });
    reject("field metadata incomplete", [](auto& f) { f.module.records[0].fields.pop_back(); });
    reject("unexpected result", [](auto& f) { f.operation().result = 0; });
    reject("unexpected operand", [](auto& f) { f.operation().operands.push_back(0); });
    reject("unexpected callee", [](auto& f) { f.operation().callee = 1; });
    reject("unexpected dynamic target", [](auto& f) { f.operation().dynamicTargets.push_back(1); });
    reject("unexpected dynamic signature", [](auto& f) { f.operation().dynamicSignature = f.environment; });
    reject("unexpected atomic element", [](auto& f) { f.operation().atomicElement = f.types.builtin("i32"); });
    reject("unexpected sync element", [](auto& f) { f.operation().syncElement = f.types.builtin("i32"); });
    reject("unexpected Task element", [](auto& f) { f.operation().taskElement = f.types.builtin("i32"); });
    reject("unexpected Task callable", [](auto& f) { f.operation().taskCallable = f.environment; });
    reject("unexpected Task invoke", [](auto& f) { f.operation().taskInvoke = 1; });
    reject("unknown operation", [](auto& f) { f.operation().operation = "partial"; });
    reject("wrong type", [](auto& f) { f.operation().type = f.types.builtin("unit"); });
    reject("wrong place type", [](auto& f) { f.operation().place.type = f.types.builtin("i32"); });
    reject("global source", [](auto& f) { f.operation().place.global = 99; });
    reject("pointer source", [](auto& f) { f.operation().place.pointer = 0; });
    reject("projected source", [](auto& f) { f.operation().place.projections.push_back({AIRProjectionKind::Field, 0, InvalidAIRId, f.types.builtin("string")}); });
    reject("missing root begin", [](auto& f) { f.function().blocks[0].instructions[0].opcode = AIROpcode::ScopeEnd; });
    reject("not at entry", [](auto& f) { f.function().blocks[0].instructions.insert(f.function().blocks[0].instructions.begin() + 1, f.function().blocks[0].instructions[0]); });
    reject("double consume", [](auto& f) { const auto duplicate = f.operation(); f.function().blocks[0].instructions.push_back(duplicate); });
    reject("metadata on ordinary opcode", [](auto& f) { f.operation().opcode = AIROpcode::Drop; });
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
