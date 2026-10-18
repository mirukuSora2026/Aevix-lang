// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_thread_safety_analysis_tests.cpp
//
// Description :
//     봉인된 전달 특성, 재귀 효과 고정점과 간접 소멸자의 작업 경계를 검사한다.
//     Check sealed transfer traits, recursive effect convergence, and indirect destructor worker boundaries.
//
// ========================
#include "aevix/Frontend/Ownership/ThreadSafetyAnalysis.hpp"
#include <iostream>
#include <cstdlib>

namespace
{
    using namespace aevix;
    unsigned checks = 0, failures = 0;
    void expect(bool condition, const char* label)
    { ++checks; if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; } }
    struct Fixture final
    {
        TypeCheckResult typed;
        AIRModule module;
        Diagnostics diagnostics;
        SymbolId nextType = 100;
        Fixture() { function(1); module.entry = 1; }
        TypeId unit() const { return typed.types.builtin("unit"); }
        TypeId integer() const { return typed.types.builtin("i32"); }
        AIRFunction& function(SymbolId symbol, TypeId result = InvalidTypeId)
        {
            AIRFunction fn; fn.symbol = symbol; fn.name = "function_" + std::to_string(symbol);
            fn.result = result == InvalidTypeId ? unit() : result; fn.blocks.resize(1); fn.scopes.resize(1);
            module.functions.push_back(std::move(fn));
            auto& added = module.functions.back(); added.blocks[0].terminator.kind = AIRTerminatorKind::Return;
            if (typed.types.get(added.result).kind != TypeKind::Void)
                added.blocks[0].terminator.value = constant(symbol, added.result);
            return added;
        }
        AIRFunction& get(SymbolId symbol)
        { for (auto& fn : module.functions) if (fn.symbol == symbol) return fn; std::abort(); }
        AIRValueId constant(SymbolId symbol, TypeId type)
        {
            auto& fn = get(symbol); AIRInstruction item; item.opcode = AIROpcode::Constant; item.type = type;
            item.operation = "0"; item.result = fn.valueTypes.size(); fn.valueTypes.push_back(type);
            fn.blocks[0].instructions.push_back(item); return item.result;
        }
        AIRValueId append(SymbolId symbol, AIRInstruction item)
        {
            auto& fn = get(symbol);
            if (item.type != InvalidTypeId && typed.types.get(item.type).kind != TypeKind::Void)
            { item.result = fn.valueTypes.size(); fn.valueTypes.push_back(item.type); }
            fn.blocks[0].instructions.push_back(item); return item.result;
        }
        AIRLocalId local(SymbolId symbol, TypeId type, bool parameter = false)
        {
            auto& fn = get(symbol); AIRLocal item; item.type = type; item.isParameter = parameter; item.scope = 0;
            const auto slot = fn.locals.size(); fn.locals.push_back(item); fn.scopes[0].locals.push_back(slot);
            if (parameter) fn.parameters.push_back(slot); return slot;
        }
        TypeId record(std::vector<TypeId> fields, const char* name = "Record")
        {
            const auto type = typed.types.nominal(nextType++, name); AIRRecord value; value.type = type; value.fieldTypes = fields;
            for (std::size_t index = 0; index < fields.size(); ++index) value.fields.push_back(nextType++);
            module.records.push_back(value); local(1, type); return type;
        }
        void call(SymbolId from, SymbolId to)
        {
            const auto result = get(to).result;
            std::vector<TypeId> parameters;
            for (const auto slot : get(to).parameters) parameters.push_back(get(to).locals[slot].type);
            AIRInstruction item; item.opcode = AIROpcode::Call; item.type = result; item.callee = to;
            for (const auto type : parameters) item.operands.push_back(constant(from, type));
            append(from, item);
        }
        void global(SymbolId id, TypeId type, bool mutableValue)
        { AIRGlobal value; value.symbol = id; value.name = "global_" + std::to_string(id); value.type = type; value.isMutable = mutableValue; module.globals.push_back(value); }
        void globalRead(SymbolId fn, SymbolId global, TypeId type, bool borrow = false)
        {
            AIRInstruction item; item.opcode = borrow ? AIROpcode::BorrowShared : AIROpcode::Read;
            item.type = borrow ? typed.types.reference(type, false) : type; item.place.global = global; item.place.type = type; append(fn, item);
        }
        void globalWrite(SymbolId fn, SymbolId global, TypeId type)
        {
            AIRInstruction item; item.opcode = AIROpcode::Write; item.place.global = global; item.place.type = type;
            item.operands = {constant(fn, type)}; append(fn, item);
        }
        ThreadSafetyResult analyze()
        {
            return ThreadSafetyAnalysis(diagnostics).analyze(module, typed);
        }
        bool admit(const ThreadSafetyResult& result, SymbolId target = 1, TypeId capture = InvalidTypeId, TypeId output = InvalidTypeId)
        { return ThreadSafetyAnalysis(diagnostics).validateOwnedWorker(result, target, capture == InvalidTypeId ? unit() : capture, output == InvalidTypeId ? unit() : output); }
    };
}

int main()
{
    {
        Fixture f; f.function(2); f.function(3); f.call(1,2); f.call(2,3); f.call(3,2);
        auto result = f.analyze(); expect(result.validAIR, "recursive innocent helper AIR valid");
        expect(result.functions.at(1).workerSafe && result.functions.at(2).workerSafe && result.functions.at(3).workerSafe, "pure function SCC reaches safe fixed point");
        expect(f.admit(result), "zero-capture innocent static worker admitted");
        expect(result.traits(f.integer()).send && result.traits(f.integer()).sync, "primitive transfer is sealed and safe");
    }
    {
        Fixture f; f.function(2); f.function(3); f.call(1,2); f.call(2,3); f.call(3,2);
        f.global(50, f.integer(), true); f.globalWrite(3,50,f.integer());
        auto result = f.analyze(); expect(result.validAIR, "global write module remains valid ordinary AIR");
        expect(!result.functions.at(1).workerSafe && !result.functions.at(2).workerSafe && !result.functions.at(3).workerSafe, "global mutation propagates through call SCC");
        expect(!f.admit(result), "zero captures cannot bypass global effect checks");
        const auto& path = result.functions.at(1).issue->callPath;
        expect(path == std::vector<SymbolId>{1,2,3}, "transitive global issue preserves deterministic call path");
    }
    {
        Fixture f; f.global(50,f.integer(),false); f.globalRead(1,50,f.integer());
        auto result = f.analyze(); expect(f.admit(result), "immutable Sync global read admitted");
        f.module.globals[0].isMutable = true; result = f.analyze();
        expect(!f.admit(result), "readonly read of ordinary mutable global still rejected");
    }
    {
        Fixture f; const auto scalar = f.integer(); const auto guard = f.typed.types.sync(TypeKind::MutexGuard,30,"MutexGuard",scalar);
        const auto hidden = f.record({guard}, "Send");
        const auto tuple = f.typed.types.tuple({hidden}); f.local(1, tuple);
        const auto vector = f.typed.types.vector(tuple); f.local(1, vector);
        const auto shared = f.typed.types.shared(31,"Shared",scalar);
        const auto badShared = f.typed.types.shared(32,"Shared",guard);
        f.local(1,shared); f.local(1,badShared);
        auto result = f.analyze(); expect(!result.traits(guard).send && !result.traits(guard).sync, "native guard cannot cross threads");
        expect(!result.traits(hidden).send && !result.traits(tuple).send && !result.traits(vector).send, "nested guard found through record Tuple Vector");
        expect(result.traits(shared).send && result.traits(shared).sync, "Shared requires safe primitive payload");
        expect(!result.traits(badShared).send && !result.traits(badShared).sync, "Shared cannot hide nontransferable guard");
        expect(!result.traits(hidden).sync, "user type name Send cannot authorize a guarded field");
    }
    {
        Fixture f;
        const auto sharedLoan = f.typed.types.reference(f.integer(),false), mutableLoan = f.typed.types.reference(f.integer(),true);
        const auto borrowed = f.record({sharedLoan});
        const auto mutableBorrowed = f.record({mutableLoan});
        const auto mutex = f.typed.types.sync(TypeKind::Mutex,30,"Mutex",mutableLoan);
        const auto rwlock = f.typed.types.sync(TypeKind::RwLock,31,"RwLock",mutableLoan);
        f.local(1,mutex); f.local(1,rwlock);
        f.function(2,f.integer()); f.local(2,f.typed.types.reference(borrowed,false),true);
        auto result = f.analyze();
        expect(result.traits(borrowed).send && result.traits(borrowed).sync && result.traits(borrowed).containsBorrow, "borrowed traits separated from actual lifetime proof");
        expect(result.traits(mutableBorrowed).send && !result.traits(mutableBorrowed).sync, "exclusive reference transfers but is conservatively nonSync");
        expect(result.traits(mutex).send && result.traits(mutex).sync, "Mutex Sync depends on T Send");
        expect(result.traits(rwlock).send && !result.traits(rwlock).sync, "RwLock readers require T Send and Sync");
        expect(!f.admit(result,2,borrowed,f.integer()), "owned worker rejects borrowed capture without Scope proof");
    }
    {
        Fixture f;
        const auto atomic = f.typed.types.atomic(30,"Atomic",f.integer());
        const auto wide = f.typed.types.atomic(31,"Atomic",f.typed.types.builtin("i128"));
        const auto source = f.typed.types.cancellation(32,"CancellationSource",false);
        const auto token = f.typed.types.cancellation(33,"CancellationToken",true);
        f.local(1,atomic);f.local(1,wide);f.local(1,source);f.local(1,token);
        auto result = f.analyze(); expect(result.traits(atomic).send && result.traits(atomic).sync, "closed scalar Atomic transfers safely");
        expect(!result.traits(wide).send && !result.traits(wide).sync, "unsupported i128 Atomic not assumed lockfree or transferable");
        expect(result.traits(source).send && result.traits(token).sync, "audited independent cancellation state is transferable");
        f.global(50,atomic,false);f.globalRead(1,50,atomic,true); result=f.analyze();
        expect(f.admit(result), "immutable Atomic global shared borrow admitted");
    }
    {
        Fixture f;
        const auto first = f.record({}), second = f.record({});
        const auto firstBox = f.typed.types.boxed(30,"Box",first), secondBox = f.typed.types.boxed(31,"Box",second);
        f.module.records[0].fieldTypes = {secondBox}; f.module.records[0].fields = {200};
        f.module.records[1].fieldTypes = {firstBox}; f.module.records[1].fields = {201};
        auto result=f.analyze();expect(result.traits(first).send && result.traits(second).sync, "recursive owned type SCC converges without arbitrary depth cutoff");
        const auto loan=f.typed.types.reference(f.integer(),false);
        f.module.records[1].fieldTypes.push_back(loan); f.module.records[1].fields.push_back(202);
        result=f.analyze();expect(result.traits(first).containsBorrow && result.traits(second).containsBorrow, "contained reference fact propagates through recursive type SCC");
    }
    {
        Fixture f; const auto owner=f.record({f.integer()});
        f.function(2,f.typed.types.builtin("void")); f.local(2,f.typed.types.reference(owner,true),true);
        f.function(3); f.call(2,3); f.typed.dropMethods[owner]=2;
        f.global(50,f.integer(),true); f.globalWrite(3,50,f.integer());
        auto result=f.analyze(); expect(!result.functions.at(1).workerSafe, "temporary custom Drop transitive global effect found");
        expect(result.functions.at(1).cleanupCalls.contains(2), "implicit cleanup adds a real destructor call edge");
        expect(!result.traits(owner).send, "unsafe Drop prevents owner transfer");
        expect(!f.admit(result), "worker rejected even if unsafe Drop only runs for a local temporary");
    }
    {
        Fixture f;const auto owner=f.record({f.integer()});
        f.function(2,f.typed.types.builtin("void"));f.local(2,f.typed.types.reference(owner,true),true);f.typed.dropMethods[owner]=2;
        f.function(3);f.call(2,3);f.call(3,2);
        auto result=f.analyze();expect(result.functions.at(1).workerSafe && result.traits(owner).send, "safe recursive Drop/helper SCC admitted");
        expect(f.admit(result), "verified custom Drop is supported rather than blanket denied");
    }
    {
        Fixture f; AIRExternal foreign;foreign.symbol=90;foreign.name="looks_pure";foreign.result=f.unit();f.module.externals.push_back(foreign);
        AIRInstruction call;call.opcode=AIROpcode::Call;call.type=f.unit();call.callee=90;f.append(1,call);
        auto result=f.analyze();expect(result.validAIR && !result.functions.at(1).workerSafe, "ordinary FFI remains valid but unapproved on worker");
        expect(!f.admit(result), "extern spelling cannot grant worker permission");
    }
    {
        Fixture f;AIRInstruction call;call.opcode=AIROpcode::Call;call.operation="dyn.call.0";call.type=f.unit();
        call.dynamicSignature=f.typed.types.function(f.unit(),{});f.append(1,call);
        auto result=f.analyze();expect(result.validAIR && !result.functions.at(1).workerSafe, "unproven dynamic effect contract rejected");
    }
    {
        Fixture f;f.module.functions.clear();f.function(1,f.typed.types.builtin("string_view"));
        auto result=f.analyze();expect(result.traits(f.typed.types.builtin("string_view")).send, "text view structural transfer fact exists");
        expect(!f.admit(result,1,f.unit(),f.typed.types.builtin("string_view")), "owned worker result cannot return an unproven borrowed view");
    }
    {
        Fixture f;auto result=f.analyze();expect(!f.admit(result,999), "unknown worker target rejected");
        expect(!f.admit(result,1,f.unit(),f.integer()), "incorrect worker result rejected");
        f.module.functions[0].blocks.clear();result=f.analyze();expect(!result.validAIR, "malformed AIR cannot yield admitted thread facts");
        expect(!f.admit(result), "invalid analysis cannot authorize worker");
    }
    {
        Fixture f; AIRInstruction unknown; unknown.opcode = static_cast<AIROpcode>(999); unknown.type=f.unit(); f.append(1,unknown);
        auto result=f.analyze();expect(!result.functions.at(1).workerSafe, "future opcode has no implicit worker permission");
    }
    {
        Fixture f; f.function(2); f.function(3); f.global(50,f.integer(),false);
        f.module.globals[0].isPersist=true; f.module.globals[0].isLocalPersist=true; f.module.globals[0].initializerFunction=2;
        AIRInstruction ensure;ensure.opcode=AIROpcode::EnsurePersist;ensure.place.global=50;ensure.place.type=f.integer();f.append(1,ensure);
        f.call(2,3); AIRExternal foreign;foreign.symbol=90;foreign.result=f.unit();f.module.externals.push_back(foreign);
        AIRInstruction call;call.opcode=AIROpcode::Call;call.type=f.unit();call.callee=90;f.append(3,call);
        auto result=f.analyze();expect(!result.functions.at(1).workerSafe, "lazy persist initializer effects are transitive worker effects");
        expect(result.functions.at(1).directCalls.contains(2), "lazy initializer contributes an explicit call edge");
    }
    {
        Fixture f;
        const auto task = f.typed.types.taskOwner(TypeKind::Task, 80, "Task<i32>", f.integer());
        const auto scope = f.typed.types.taskOwner(TypeKind::TaskScope, 81, "TaskScope");
        f.local(1, task); f.local(1, scope);
        const auto wrapped = f.record({task}, "TaskHolder");
        const auto result = f.analyze();
        expect(result.validAIR, "sealed Task and Scope types have valid analysis storage");
        expect(!result.traits(task).send && result.traits(task).containsBorrow,
            "owned Task result still carries its hidden Scope borrow");
        expect(!result.traits(scope).send && !result.traits(scope).sync,
            "owned-only profile cannot transfer Scope ownership to a worker");
        expect(!result.traits(wrapped).send && result.traits(wrapped).containsBorrow,
            "ordinary wrappers cannot hide a Task Scope dependency");
    }
    std::cout<<checks<<" thread safety analysis checks, "<<failures<<" failures\n";
    return failures?1:0;
}
