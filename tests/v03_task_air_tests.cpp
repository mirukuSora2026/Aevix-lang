// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_task_air_tests.cpp
//
// Description :
//     작업 AIR의 봉인된 소유자·콜백 정체성·정확한 결과 및 오류 형태를 검사한다.
//     Check sealed Task owners, callback identity, and exact result/error shapes in AIR.
//
// ========================
#include "aevix/IR/AIR/Module.hpp"
#include "aevix/Native/TaskOperation.hpp"
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
        NodePtr closure = makeNode("Struct","Closure");
        NodePtr identity = makeNode("Struct","TaskId");
        std::size_t index = 0;
        TypeId scope, task, payload, callable, error, spawnError, scopeError, cancelled, token, taskId;
        TypeId enumeration(const char* name, std::vector<std::vector<TypeId>> fields)
        {
            const auto id = types.nominal(100 + module.records.size(),name);
            AIRRecord record; record.type = id; record.isEnum = true;
            for (std::size_t tag = 0; tag < fields.size(); ++tag)
            { EnumVariantInfo variant; variant.type = id; variant.tag = tag; variant.fieldTypes = fields[tag];
                for (std::size_t field = 0; field < fields[tag].size(); ++field) variant.fields.push_back(10000 + module.records.size() * 100 + tag * 10 + field);
                record.variants.push_back(std::move(variant)); }
            module.records.push_back(std::move(record)); return id;
        }
        TypeId structure(const char* name, std::vector<TypeId> fields)
        {
            const auto id = types.nominal(100 + module.records.size(),name);
            AIRRecord record; record.type = id; record.fieldTypes = std::move(fields);
            module.records.push_back(std::move(record)); return id;
        }
        static AIRValueId constant(AIRFunction& function, TypeId type)
        {
            const auto id = function.valueTypes.size(); function.valueTypes.push_back(type);
            AIRInstruction item; item.opcode = AIROpcode::Constant; item.type = type; item.result = id; item.operation = "0";
            function.blocks[0].instructions.push_back(std::move(item)); return id;
        }
        explicit Fixture(const char* name)
        {
            payload = types.builtin("i32");
            scope = types.taskOwner(TypeKind::TaskScope,10,"TaskScope");
            task = types.taskOwner(TypeKind::Task,11,"Task",payload);
            callable = structure("Closure",{});
            module.records.back().source = closure.get();
            closure->children.push_back(makeNode("CompilerClosure","FnOnce"));
            auto method = makeNode("Function","__invoke"); method->children.push_back(makeNode("Block",""));
            closure->children.push_back(std::move(method));
            error = enumeration("TaskError",{{},{},{},{payload,payload},{payload},{},{},{},{},{},{}});
            spawnError = enumeration("SpawnError",{{},{},{},{payload,payload}});
            scopeError = enumeration("ScopeError",{{scope,error},{error}});
            cancelled = enumeration("Cancelled",{{}});
            token = types.cancellation(12,"CancellationToken",true);
            taskId = structure("TaskId",{types.builtin("u64")});
            module.records.back().source = identity.get();
            module.records.back().fields = {19999};
            identity->children.push_back(makeNode("CompilerTaskId",""));
            auto serial = makeNode("Variable","serial"); serial->children.push_back(makeNode("Type","u64")); serial->children.push_back(makeNode("ArraySuffixes",""));
            identity->children.push_back(std::move(serial));
            const auto executorId = enumeration("ExecutorId",{{}});
            const auto scopeState = enumeration("ScopeStatus",{{},{}});
            const auto executor = structure("Executor",{types.builtin("u8")});
            const auto options = structure("ScopeOptions",{types.builtin("u8")});
            const auto duration = structure("Duration",{types.builtin("u64")});
            const auto phase = enumeration("TaskPhase",{{},{},{},{},{},{},{},{}});
            const auto state = structure("TaskStatus",{phase,types.builtin("bool")});
            const auto wait = enumeration("WaitStatus",{{},{}});
            const auto request = enumeration("TaskCancelRequest",{{},{},{}});
            const auto report = structure("ScopeReport",std::vector<TypeId>(7,types.builtin("u64")));
            const auto optional = enumeration("Optional",{{payload},{}});
            const auto& op = *taskOperation(name);
            const auto output = [&]() {
                switch (op.result) {
                    case TaskResult::Scope: return scope; case TaskResult::Spawn: return task;
                    case TaskResult::Join: return payload; case TaskResult::TryTake: return optional;
                    case TaskResult::Discard: return types.builtin("unit"); case TaskResult::Status: return state;
                    case TaskResult::Boolean: return types.builtin("bool");
                    case TaskResult::Token: return token;
                    case TaskResult::Executor: return executorId;
                    case TaskResult::Identity: return taskId;
                    case TaskResult::CloseAdmission: return types.builtin("unit");
                    case TaskResult::ActiveCount: return types.builtin("usize");
                    case TaskResult::ScopeState: return scopeState;
                    case TaskResult::Completion: case TaskResult::CheckCancel: return types.builtin("unit");
                    case TaskResult::Wait: return wait; case TaskResult::Request: return request; case TaskResult::Close: return report;
                }
                return InvalidTypeId;
            }();
            auto result = output;
            if (taskFallible(op.result)) result = enumeration("Result",{{output},{op.result == TaskResult::CheckCancel ? cancelled : (op.result == TaskResult::Close || op.result == TaskResult::CloseAdmission) ? scopeError : op.result == TaskResult::Scope || op.result == TaskResult::Spawn ? spawnError : error}});
            AIRFunction main; main.symbol = 1; main.name = "main"; main.result = payload; main.blocks.resize(1);
            AIRInstruction item; item.opcode = AIROpcode::TaskIntrinsic; item.operation = name; item.type = result;
            item.taskElement = op.result == TaskResult::Scope || op.result == TaskResult::Close || op.result == TaskResult::CheckCancel || (!op.worker && op.preparation == TaskPreparation::None && op.access == TaskAccess::ScopeShared) ? types.builtin("unit") : payload;
            item.taskCallable = op.worker ? callable : types.builtin("unit");
            item.taskInvoke = op.worker ? 2 : InvalidSymbolId;
            if (op.access == TaskAccess::None) item.operands = {constant(main,types.reference(executor,false)),constant(main,options)};
            else {
                const auto input = op.access == TaskAccess::CancellationShared ? token : op.access == TaskAccess::ScopeShared || op.access == TaskAccess::ScopeValue ? scope : task;
                const bool owned = op.access == TaskAccess::ScopeValue || op.access == TaskAccess::TaskValue;
                item.operands.push_back(constant(main,owned ? input : types.reference(input,op.access == TaskAccess::TaskExclusive)));
                if (op.worker) item.operands.push_back(constant(main,callable));
                else if (op.preparation == TaskPreparation::Ready) item.operands.push_back(constant(main,payload));
                else if (op.result == TaskResult::Wait) item.operands.push_back(constant(main,duration));
            }
            item.result = main.valueTypes.size(); main.valueTypes.push_back(result);
            index = main.blocks[0].instructions.size(); main.blocks[0].instructions.push_back(std::move(item));
            main.blocks[0].terminator.kind = AIRTerminatorKind::Return; main.blocks[0].terminator.value = constant(main,payload);
            module.functions.push_back(std::move(main)); module.entry = 1;
            if (op.worker) {
                AIRFunction invoke; invoke.symbol = 2; invoke.name = "Closure.__invoke"; invoke.result = payload; invoke.source = closure->children[1].get();
                invoke.locals.push_back({"self",20,callable,false,true,0,nullptr}); invoke.parameters = {0}; invoke.scopes.resize(1); invoke.scopes[0].locals = {0}; invoke.blocks.resize(1);
                invoke.blocks[0].terminator.kind = AIRTerminatorKind::Return; invoke.blocks[0].terminator.value = constant(invoke,payload); module.functions.push_back(std::move(invoke));
            }
        }
        AIRInstruction& operation() { return module.functions[0].blocks[0].instructions[index]; }
        void first(TypeId type) { module.functions[0].valueTypes[0] = type; module.functions[0].blocks[0].instructions[0].type = type; }
        bool valid() { Diagnostics diagnostics; return verifyAIR(module,types,diagnostics); }
    };
}
int main()
{
    for (const auto name : {"scope_open","scope_close","spawn_owned","spawn","ready","join","try_take","discard_result","status","wait_timeout","request_cancel","is_finished","is_cancel_requested","cancellation","wait","cancel_and_join","check_cancel","executor","id","close_admission","active_count","scope_status"}) {
        Fixture good(name); expect(good.valid(),"valid closed Task operation");
        const auto reject = [&](const char* label, const auto& mutation) { Fixture bad(name); mutation(bad); expect(!bad.valid(),label); };
        reject("unknown operation",[](auto& f){ f.operation().operation = "detach_raw"; });
        reject("missing operand",[](auto& f){ f.operation().operands.clear(); });
        reject("extra operand",[](auto& f){ f.operation().operands.push_back(0); });
        reject("foreign callee",[](auto& f){ f.operation().callee = 900; });
        reject("storage metadata",[](auto& f){ f.operation().place.type = f.task; });
        reject("atomic metadata",[](auto& f){ f.operation().atomicElement = f.payload; });
        reject("sync metadata",[](auto& f){ f.operation().syncElement = f.payload; });
        reject("dynamic targets",[](auto& f){ f.operation().dynamicTargets = {2}; });
        reject("dynamic signature",[](auto& f){ f.operation().dynamicSignature = f.types.function(f.payload,{}); });
        reject("missing result",[](auto& f){ f.operation().result = InvalidAIRId; });
        reject("wrong result shape",[](auto& f){ auto& i=f.operation(); i.type=f.types.builtin("u64"); f.module.functions[0].valueTypes[i.result]=i.type; });
        reject("wrong application result",[](auto& f){ f.operation().taskElement = f.types.builtin("bool"); });
        reject("wrong owner operand",[](auto& f){ f.first(f.types.builtin("u64")); });
        if (taskOperation(name)->worker) {
            reject("missing invoke",[](auto& f){ f.operation().taskInvoke = InvalidSymbolId; });
            reject("nonclosure source",[](auto& f){ f.closure->children[0]->kind = "UserAttribute"; });
            reject("wrong callback owner",[](auto& f){ f.module.functions[1].source = f.closure.get(); });
            reject("different callback name",[](auto& f){ f.closure->children[1]->value = "callback"; });
            reject("wrong callback result",[](auto& f){ f.module.functions[1].result = f.types.builtin("bool"); });
            reject("wrong callback arity",[](auto& f){ f.module.functions[1].parameters.clear(); });
            reject("wrong receiver mode",[](auto& f){ f.closure->children[0]->value = "FnMut"; });
            reject("wrong callable type",[](auto& f){ f.operation().taskCallable = f.types.builtin("unit"); });
        } else {
            reject("unexpected callback",[](auto& f){ f.operation().taskInvoke = 2; });
            reject("unexpected capture metadata",[](auto& f){ f.operation().taskCallable = f.callable; });
        }
        if (taskFallible(taskOperation(name)->result)) {
            reject("forged failure shape",[](auto& f){ for(auto& r:f.module.records) if(r.type==f.error || r.type==f.spawnError || r.type==f.cancelled) r.variants[0].fieldTypes={f.task}; });
        }
        if (taskOperation(name)->result == TaskResult::CheckCancel) {
            reject("checkpoint rejects request authority",[](auto& f){f.first(f.types.reference(f.types.cancellation(13,"CancellationSource",false),false));});
            reject("checkpoint requires borrowed observer",[](auto& f){f.first(f.token);});
            reject("checkpoint cannot reuse TaskError",[](auto& f){for(auto& r:f.module.records) if(r.type==f.cancelled) r.variants.push_back({});});
        }
        if (taskOperation(name)->result == TaskResult::Identity) {
            reject("identity must be canonical sealed source",[](auto& f){f.identity->children[0]->kind="UserAttribute";});
            reject("identity source name must match",[](auto& f){f.identity->value="OtherId";});
            reject("identity source must be struct",[](auto& f){f.identity->kind="Enum";});
            reject("identity marker has no payload",[](auto& f){f.identity->children[0]->value="native_token";});
            reject("identity marker has no children",[](auto& f){f.identity->children[0]->children.push_back(makeNode("Type","u64"));});
            reject("identity marker must be unique",[](auto& f){f.identity->children.push_back(makeNode("CompilerTaskId",""));});
            reject("identity must retain one private serial",[](auto& f){f.identity->children[1]->value="native_token";});
            reject("identity source serial must be u64",[](auto& f){f.identity->children[1]->children[0]->value="usize";});
            reject("identity source serial must be Type",[](auto& f){f.identity->children[1]->children[0]->kind="Identifier";});
            reject("identity serial cannot be array",[](auto& f){f.identity->children[1]->children[1]->children.push_back(makeNode("ArraySuffix",""));});
            reject("identity serial cannot be generic",[](auto& f){f.identity->children[1]->children[0]->children.push_back(makeNode("Type","u8"));});
            reject("identity cannot add public field",[](auto& f){f.identity->children[1]->children.push_back(makeNode("Modifier","public"));});
            reject("identity cannot add mutable field",[](auto& f){f.identity->children[1]->children.push_back(makeNode("Modifier","mut"));});
            reject("identity cannot add persist field",[](auto& f){f.identity->children[1]->children.push_back(makeNode("Modifier","persist"));});
            reject("identity cannot add second field",[](auto& f){auto v=makeNode("Variable","other");v->children.push_back(makeNode("Type","u64"));f.identity->children.push_back(std::move(v));});
            reject("identity needs source",[](auto& f){for(auto& r:f.module.records) if(r.type==f.taskId) r.source=nullptr;});
            reject("identity cannot alias absent field",[](auto& f){for(auto& r:f.module.records) if(r.type==f.taskId) r.fields={InvalidSymbolId};});
            reject("identity has exact field symbol count",[](auto& f){for(auto& r:f.module.records) if(r.type==f.taskId) r.fields={123,124};});
            reject("identity has exact u64 storage",[](auto& f){for(auto& r:f.module.records) if(r.type==f.taskId) r.fieldTypes={f.types.builtin("usize")};});
            reject("identity cannot be enum",[](auto& f){for(auto& r:f.module.records) if(r.type==f.taskId) r.isEnum=true;});
            reject("identity cannot carry variants",[](auto& f){for(auto& r:f.module.records) if(r.type==f.taskId) r.variants.push_back({});});
            reject("identity receiver is borrowed",[](auto& f){f.first(f.task);});
        }
        if (taskOperation(name)->result == TaskResult::Token) {
            reject("derived token cannot be request authority",[](auto& f){for(auto& r:f.module.records) for(auto& v:r.variants) if(v.fieldTypes==std::vector<TypeId>{f.token}) v.fieldTypes[0]=f.types.cancellation(91,"CancellationSource",false);});
            reject("derived token role has canonical name",[](auto& f){for(auto& r:f.module.records) for(auto& v:r.variants) if(v.fieldTypes==std::vector<TypeId>{f.token}) v.fieldTypes[0]=f.types.cancellation(91,"PretendObserver",true);});
        }
        if (taskOperation(name)->result == TaskResult::Close || taskOperation(name)->result == TaskResult::CloseAdmission) {
            reject("Failure belongs to ScopeError",[](auto& f){for(auto& r:f.module.records) if(r.type==f.scopeError) r.variants[1].type=f.task;});
            reject("Retry belongs to ScopeError",[](auto& f){for(auto& r:f.module.records) if(r.type==f.scopeError) r.variants[0].type=f.task;});
            reject("Failure has exactly one field symbol",[](auto& f){for(auto& r:f.module.records) if(r.type==f.scopeError) r.variants[1].fields.clear();});
            reject("Retry has exactly two field symbols",[](auto& f){for(auto& r:f.module.records) if(r.type==f.scopeError) r.variants[0].fields.pop_back();});
            reject("Failure reason cannot alias Retry scope",[](auto& f){for(auto& r:f.module.records) if(r.type==f.scopeError) r.variants[1].fields[0]=r.variants[0].fields[0];});
            reject("Retry fields cannot alias each other",[](auto& f){for(auto& r:f.module.records) if(r.type==f.scopeError) r.variants[0].fields[1]=r.variants[0].fields[0];});
            reject("Failure field identity cannot be absent",[](auto& f){for(auto& r:f.module.records) if(r.type==f.scopeError) r.variants[1].fields[0]=InvalidSymbolId;});
            reject("non-owning Failure must not contain Scope",[](auto& f){for(auto& r:f.module.records) if(r.type==f.scopeError) r.variants[1].fieldTypes={f.scope,f.error};});
            reject("ScopeError must retain Failure variant",[](auto& f){for(auto& r:f.module.records) if(r.type==f.scopeError) r.variants.resize(1);});
            reject("Failure must retain exact TaskError",[](auto& f){for(auto& r:f.module.records) if(r.type==f.scopeError) r.variants[1].fieldTypes={f.types.builtin("u64")};});
            reject("Retry must retain actual Scope",[](auto& f){for(auto& r:f.module.records) if(r.type==f.scopeError) r.variants[0].fieldTypes={f.error};});
            reject("Retry Scope must match receiver",[](auto& f){const auto other=f.types.taskOwner(TypeKind::TaskScope,77,"OtherScope");for(auto& r:f.module.records) if(r.type==f.scopeError) r.variants[0].fieldTypes[0]=other;});
        }
        if (taskOperation(name)->access == TaskAccess::TaskExclusive) reject("readonly task receiver",[](auto& f){f.first(f.types.reference(f.task,false));});
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
