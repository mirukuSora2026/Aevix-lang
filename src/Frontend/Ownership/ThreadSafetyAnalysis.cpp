// ========================
//
// Created By : miruku_sora2026
//
// File name : ThreadSafetyAnalysis.cpp
//
// Description :
//     타입·함수·소멸자의 유한 고정점으로 작업 스레드의 안전한 실행 경계를 계산한다.
//     Compute worker execution safety with a finite fixed point over types, functions, and destructors.
//
// ========================
#include "aevix/Frontend/Ownership/ThreadSafetyAnalysis.hpp"
#include "aevix/Native/TaskOperation.hpp"
#include "aevix/Native/SyncOperation.hpp"
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace aevix
{
    namespace
    {
        struct Cleanup final { bool known = true; std::set<SymbolId> calls; };
        struct GlobalUse final { SymbolId global; bool writing; SourceRange range; };
        struct FunctionInfo final
        {
            std::vector<GlobalUse> globals;
            std::optional<ThreadEffectIssue> directIssue;
        };
        ThreadEffectIssue issue(ThreadEffectKind kind, SymbolId function, SymbolId subject,
            SourceRange range, std::string message)
        { return {kind, function, subject, range, std::move(message), {function}}; }

        class Analysis final
        {
        public:
            Analysis(const AIRModule& module, const TypeCheckResult& typed)
                : module_(module), typed_(typed)
            {
                for (const auto& record : module.records) records_[record.type] = &record;
                for (const auto& function : module.functions) functions_[function.symbol] = &function;
                for (const auto& global : module.globals) globals_[global.symbol] = &global;
            }
            ThreadSafetyResult run()
            {
                for (const auto* name : {"unit", "void", "never", "bool", "char", "byte", "string", "string_view",
                    "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "i128", "u128", "isize", "usize", "f16", "bf16", "f32", "f64"})
                    collect(typed_.types.builtin(name));
                for (const auto& record : module_.records) collect(record.type);
                for (const auto& global : module_.globals) collect(global.type);
                for (const auto& function : module_.functions)
                {
                    collect(function.result);
                    for (const auto& local : function.locals) collect(local.type);
                    for (const auto type : function.valueTypes) collect(type);
                    auto& signature = result_.signatures[function.symbol]; signature.result = function.result;
                    for (const auto local : function.parameters) signature.parameters.push_back(function.locals[local].type);
                    result_.functions[function.symbol] = {};
                }
                for (const auto& [type, description] : result_.descriptions)
                { (void)description; result_.types[type] = {true, true, false}; }
                for (const auto& [type, description] : result_.descriptions)
                { (void)description; std::unordered_set<TypeId> visiting; cleanup_[type] = cleanup(type, visiting); }
                for (const auto& function : module_.functions) scan(function);

                // Send/Sync·workerSafe는 참에서 거짓으로만, containsBorrow는 거짓에서 참으로만 변한다.
                // Send/Sync and workerSafe only decrease; containsBorrow only increases, bounding every SCC.
                bool changed = true;
                while (changed)
                {
                    changed = false;
                    for (auto& [type, facts] : result_.types)
                    {
                        const auto next = derive(type);
                        const ThreadTypeFacts merged{facts.send && next.send, facts.sync && next.sync, facts.containsBorrow || next.containsBorrow};
                        if (facts != merged) { facts = merged; changed = true; }
                    }
                    for (auto& [symbol, effects] : result_.functions)
                    {
                        if (!effects.workerSafe) continue;
                        std::optional<ThreadEffectIssue> failure = information_[symbol].directIssue;
                        if (!failure)
                            for (const auto& use : information_[symbol].globals)
                            {
                                const auto found = globals_.find(use.global);
                                if (found == globals_.end()) failure = issue(ThreadEffectKind::MissingGlobal, symbol, use.global, use.range, "Worker access has no resolved global declaration.");
                                else if (use.writing || found->second->isMutable)
                                    failure = issue(ThreadEffectKind::MutableGlobal, symbol, use.global, use.range, "Worker code cannot access unsynchronized mutable global storage.");
                                else if (!result_.traits(found->second->type).sync)
                                    failure = issue(ThreadEffectKind::GlobalNotSync, symbol, use.global, use.range, "Shared worker access requires a compiler-derived Sync global type.");
                                if (failure) break;
                            }
                        if (!failure)
                        {
                            auto calls = effects.directCalls; calls.insert(effects.cleanupCalls.begin(), effects.cleanupCalls.end());
                            for (const auto callee : calls)
                            {
                                const auto target = result_.functions.find(callee);
                                if (target == result_.functions.end())
                                    failure = issue(ThreadEffectKind::MissingFunction, symbol, callee, {}, "Worker call or cleanup has no concrete AIR implementation.");
                                else if (!target->second.workerSafe)
                                {
                                    failure = target->second.issue;
                                    if (failure) failure->callPath.insert(failure->callPath.begin(), symbol);
                                }
                                if (failure) break;
                            }
                        }
                        if (failure) { effects.workerSafe = false; effects.issue = std::move(failure); changed = true; }
                    }
                }
                result_.validAIR = true;
                return std::move(result_);
            }
        private:
            const AIRModule& module_;
            const TypeCheckResult& typed_;
            ThreadSafetyResult result_;
            std::unordered_map<TypeId, const AIRRecord*> records_;
            std::unordered_map<SymbolId, const AIRFunction*> functions_;
            std::unordered_map<SymbolId, const AIRGlobal*> globals_;
            std::map<TypeId, Cleanup> cleanup_;
            std::map<SymbolId, FunctionInfo> information_;

            std::vector<TypeId> fields(TypeId type) const
            {
                const auto found = records_.find(type);
                if (found == records_.end()) return {};
                auto result = found->second->fieldTypes;
                for (const auto& variant : found->second->variants)
                    result.insert(result.end(), variant.fieldTypes.begin(), variant.fieldTypes.end());
                return result;
            }
            void collect(TypeId type)
            {
                std::vector<TypeId> pending{type};
                while (!pending.empty())
                {
                    const auto next = pending.back(); pending.pop_back();
                    if (next == InvalidTypeId || result_.descriptions.contains(next)) continue;
                    const auto& value = typed_.types.get(next); result_.descriptions[next] = value;
                    if (value.element != InvalidTypeId) pending.push_back(value.element);
                    if (value.result != InvalidTypeId) pending.push_back(value.result);
                    pending.insert(pending.end(), value.parameters.begin(), value.parameters.end());
                    const auto nested = fields(next); pending.insert(pending.end(), nested.begin(), nested.end());
                }
            }
            Cleanup cleanup(TypeId type, std::unordered_set<TypeId>& visited) const
            {
                Cleanup result;
                std::vector<TypeId> pending{type};
                // 정리의 호출 집합에는 횟수가 필요 없으므로 소유 타입마다 한 번만 방문한다.
                // Cleanup summaries need call sets, not multiplicity; visit each owned type once.
                while (!pending.empty())
                {
                    const auto next = pending.back(); pending.pop_back();
                    if (!visited.insert(next).second) continue;
                    const auto& description = typed_.types.get(next);
                    if (const auto method = typed_.dropMethods.find(next); method != typed_.dropMethods.end()) result.calls.insert(method->second);
                    switch (description.kind)
                    {
                        case TypeKind::Error: case TypeKind::Dynamic: result.known = false; break;
                        case TypeKind::Box: case TypeKind::Shared: case TypeKind::Array: case TypeKind::Vector:
                        case TypeKind::Mutex: case TypeKind::RwLock: pending.push_back(description.element); break;
                        case TypeKind::Tuple: pending.insert(pending.end(), description.parameters.begin(), description.parameters.end()); break;
                        case TypeKind::Nominal:
                        {
                            result.known = result.known && records_.contains(next);
                            const auto nested = fields(next); pending.insert(pending.end(), nested.begin(), nested.end()); break;
                        }
                        case TypeKind::Void: case TypeKind::Never: case TypeKind::Unit: case TypeKind::Bool:
                        case TypeKind::Integer: case TypeKind::Float: case TypeKind::Char: case TypeKind::Byte:
                        case TypeKind::String: case TypeKind::StringView: case TypeKind::Reference: case TypeKind::Function:
                        case TypeKind::Range: case TypeKind::Slice: case TypeKind::Simd: case TypeKind::SimdMask:
                        case TypeKind::Weak: case TypeKind::Atomic: case TypeKind::MutexGuard:
                        case TypeKind::RwLockReadGuard: case TypeKind::RwLockWriteGuard:
                        case TypeKind::CancellationSource: case TypeKind::CancellationToken: break;
                        default: result.known = false; break;
                    }
                }
                return result;
            }
            bool cleanupSafe(TypeId type) const
            {
                const auto found = cleanup_.find(type);
                if (found == cleanup_.end() || !found->second.known) return false;
                for (const auto method : found->second.calls)
                {
                    const auto effects = result_.functions.find(method);
                    if (effects == result_.functions.end() || !effects->second.workerSafe) return false;
                }
                return true;
            }
            ThreadTypeFacts derive(TypeId type) const
            {
                const auto& value = typed_.types.get(type);
                const auto inner = result_.traits(value.element);
                ThreadTypeFacts result{true, true, false};
                const auto aggregate = [&](const std::vector<TypeId>& nested) {
                    for (const auto field : nested)
                    { const auto facts = result_.traits(field); result.send = result.send && facts.send; result.sync = result.sync && facts.sync; result.containsBorrow = result.containsBorrow || facts.containsBorrow; }
                };
                switch (value.kind)
                {
                    case TypeKind::Error: case TypeKind::Void: case TypeKind::Never: case TypeKind::Dynamic:
                    case TypeKind::Function: return {};
                    case TypeKind::Task: return {false, false, true};
                    case TypeKind::TaskScope: return {};
                    case TypeKind::Unit: case TypeKind::Bool: case TypeKind::Integer: case TypeKind::Float:
                    case TypeKind::Char: case TypeKind::Byte: case TypeKind::String: case TypeKind::SimdMask:
                    case TypeKind::CancellationSource: case TypeKind::CancellationToken: break;
                    case TypeKind::StringView: result.containsBorrow = true; break;
                    case TypeKind::Reference: case TypeKind::Slice:
                        result.send = value.isMutable ? inner.send : inner.sync;
                        result.sync = !value.isMutable && inner.sync; result.containsBorrow = true; break;
                    case TypeKind::Box: case TypeKind::Array: case TypeKind::Vector: case TypeKind::Range: case TypeKind::Simd:
                        result = inner; break;
                    case TypeKind::Tuple: aggregate(value.parameters); break;
                    case TypeKind::Nominal:
                        if (!records_.contains(type)) return {};
                        aggregate(fields(type)); break;
                    case TypeKind::Shared: case TypeKind::Weak:
                        result.send = result.sync = inner.send && inner.sync; result.containsBorrow = inner.containsBorrow; break;
                    case TypeKind::Atomic:
                    {
                        const auto& scalar = typed_.types.get(value.element);
                        result.send = result.sync = scalar.kind == TypeKind::Bool || (scalar.kind == TypeKind::Integer &&
                            (scalar.bits == 8 || scalar.bits == 16 || scalar.bits == 32 || scalar.bits == 64));
                        break;
                    }
                    case TypeKind::Mutex:
                        result.send = result.sync = inner.send; result.containsBorrow = inner.containsBorrow; break;
                    case TypeKind::RwLock:
                        result.send = inner.send; result.sync = inner.send && inner.sync; result.containsBorrow = inner.containsBorrow; break;
                    case TypeKind::MutexGuard: case TypeKind::RwLockReadGuard: case TypeKind::RwLockWriteGuard:
                        return {false, false, true};
                    default: return {};
                }
                result.send = result.send && cleanupSafe(type);
                return result;
            }
            void scan(const AIRFunction& function)
            {
                auto& effects = result_.functions[function.symbol]; auto& details = information_[function.symbol];
                const auto failure = [&](ThreadEffectKind kind, SymbolId subject, SourceRange range, std::string message) {
                    if (!details.directIssue) details.directIssue = issue(kind, function.symbol, subject, range, std::move(message));
                };
                const auto addCleanup = [&](TypeId type, SourceRange range) {
                    const auto found = cleanup_.find(type);
                    if (found == cleanup_.end() || !found->second.known)
                        failure(ThreadEffectKind::UnknownCleanup, typed_.types.get(type).declaration, range, "Worker cleanup has an erased or unresolved owning payload.");
                    else effects.cleanupCalls.insert(found->second.calls.begin(), found->second.calls.end());
                };
                addCleanup(function.result, {});
                for (const auto& local : function.locals) addCleanup(local.type, {});
                for (const auto& block : function.blocks)
                    for (const auto& instruction : block.instructions)
                    {
                        if (instruction.type != InvalidTypeId) addCleanup(instruction.type, instruction.range);
                        if (instruction.place.global != InvalidSymbolId)
                        {
                            const bool writing = instruction.opcode == AIROpcode::Write || instruction.opcode == AIROpcode::Move || instruction.opcode == AIROpcode::BorrowMut || instruction.opcode == AIROpcode::Drop;
                            details.globals.push_back({instruction.place.global, writing, instruction.range});
                            (writing ? effects.globalsWritten : effects.globalsRead).insert(instruction.place.global);
                            if (instruction.opcode == AIROpcode::EnsurePersist)
                                if (const auto global = globals_.find(instruction.place.global); global != globals_.end()
                                    && global->second->isLocalPersist && global->second->initializerFunction != InvalidSymbolId)
                                    effects.directCalls.insert(global->second->initializerFunction);
                        }
                        if (instruction.opcode == AIROpcode::Call)
                        {
                            if (!instruction.dynamicTargets.empty() || instruction.dynamicSignature != InvalidTypeId)
                                failure(ThreadEffectKind::DynamicCall, instruction.callee, instruction.range, "Worker code requires a closed static call target; dynamic calls have no checked thread-effect contract.");
                            else if (!functions_.contains(instruction.callee))
                                failure(ThreadEffectKind::ForeignCall, instruction.callee, instruction.range, "Worker code cannot call external or unresolved implementations without an audited thread-effect contract.");
                            else effects.directCalls.insert(instruction.callee);
                        }
                        else if (instruction.opcode == AIROpcode::BoxExtract)
                        {
                            // free는 사용자 콜백을 실행하지 않는다. T의 정리는 결과 소유자의 효과로 유지한다.
                            // Outer free invokes no user callback; T cleanup remains an effect of the result owner.
                            if (!validBoxExtractionShape(function, instruction, typed_.types))
                                failure(ThreadEffectKind::UnapprovedIntrinsic, instruction.callee, instruction.range,
                                    "Worker Box extraction requires exact complete owner and sized payload metadata.");
                        }
                        else if (instruction.opcode == AIROpcode::OsIntrinsic)
                        {
                            // 검증된 닫힌 파일 호출은 엔트리별 잠금·동기 버퍼·스레드별 오류를 사용한다.
                            // Verified closed file calls use per-entry locks, synchronous buffers, and thread-local errors.
                            // sleep_ns는 작업 스레드를 막으며 async 대기나 취소 가능한 타이머가 아니다.
                            // sleep_ns blocks its worker thread; it is neither async waiting nor a cancellable timer.
                            const auto& operation = instruction.operation;
                            if (operation != "file_open" && operation != "file_read" && operation != "file_write"
                                && operation != "file_seek" && operation != "file_flush" && operation != "file_close"
                                && operation != "file_size" && operation != "file_set_len" && operation != "file_cleanup"
                                && operation != "monotonic_ns" && operation != "unix_time_ns" && operation != "sleep_ns")
                                failure(ThreadEffectKind::UnapprovedIntrinsic, instruction.callee, instruction.range,
                                    "This native OS operation has no admitted worker-effect contract.");
                        }
                        else if (instruction.opcode == AIROpcode::SystemIntrinsic)
                        {
                            // 봉인된 표준 입출력은 호출 단위 잠금과 스레드별 오류 상태를 사용하며 버퍼를 보관하지 않는다.
                            // Sealed standard IO uses per-call locks and thread-local errors without retaining buffers.
                            const auto& operation = instruction.operation;
                            if (operation != "input" && operation != "flush" && operation != "log"
                                && operation != "print" && operation != "println" && operation != "printf")
                                failure(ThreadEffectKind::UnapprovedIntrinsic, instruction.callee, instruction.range,
                                    "This system operation has no admitted worker-effect contract.");
                        }
                        else if (instruction.opcode == AIROpcode::TaskIntrinsic)
                        {
                            // 취소 확인만 동기화된 플래그를 읽으며 작업 수명이나 실행 흐름을 변경하지 않는다.
                            // Only a cancellation checkpoint reads synchronized state without changing task lifetime or control flow.
                            const auto* operation = taskOperation(instruction.operation);
                            if (!operation || operation->result != TaskResult::CheckCancel)
                                failure(ThreadEffectKind::UnapprovedIntrinsic, instruction.callee, instruction.range,
                                    "This Task operation has no admitted worker-effect contract.");
                        }
                        else if (instruction.opcode == AIROpcode::ParallelIntrinsic)
                        {
                            if (instruction.operation == "for_each") effects.directCalls.insert(instruction.callee);
                        }
                        else
                        {
                            // 새 네이티브 opcode는 효과 계약을 명시적으로 추가하기 전에는 작업에 허용하지 않는다.
                            // New native opcodes remain denied until their worker-effect contracts are explicitly admitted.
                            switch (instruction.opcode)
                            {
                                case AIROpcode::Constant: case AIROpcode::Read: case AIROpcode::Move: case AIROpcode::Write:
                                case AIROpcode::BorrowShared: case AIROpcode::BorrowMut: case AIROpcode::ClosureConsume: case AIROpcode::Unary: case AIROpcode::Binary:
                                case AIROpcode::Aggregate: case AIROpcode::EnumConstruct: case AIROpcode::EnumTag:
                                case AIROpcode::EnsurePersist: case AIROpcode::ScopeBegin: case AIROpcode::ScopeEnd: case AIROpcode::Drop:
                                case AIROpcode::RangeConstruct: case AIROpcode::RangeTest: case AIROpcode::RangeNext: case AIROpcode::RangeValue:
                                case AIROpcode::RangeWithStep: case AIROpcode::RangeCount: case AIROpcode::RangeCountFits: case AIROpcode::RangeInspect:
                                case AIROpcode::SliceConstruct: case AIROpcode::SliceLength: case AIROpcode::SliceSubview:
                                case AIROpcode::VectorAllocate: case AIROpcode::VectorAvailable: case AIROpcode::VectorAppend:
                                case AIROpcode::VectorLength: case AIROpcode::VectorReserve: case AIROpcode::VectorTake:
                                case AIROpcode::VectorTruncate: case AIROpcode::VectorInsert: case AIROpcode::ArrayRepeat:
                                case AIROpcode::NumericStatus: case AIROpcode::NumericConvert: case AIROpcode::CharScalar: case AIROpcode::Simd:
                                case AIROpcode::TextIntrinsic: case AIROpcode::ComplexIntrinsic: case AIROpcode::ParseIntrinsic:
                                case AIROpcode::MemoryIntrinsic: case AIROpcode::SharedIntrinsic: case AIROpcode::AtomicIntrinsic:
                                case AIROpcode::SyncIntrinsic: case AIROpcode::CancellationIntrinsic: break;
                                default: failure(ThreadEffectKind::UnapprovedIntrinsic, instruction.callee, instruction.range,
                                    "This operation has no admitted worker-effect contract."); break;
                            }
                        }
                    }
            }
        };
    }

    ThreadTypeFacts ThreadSafetyResult::traits(TypeId type) const
    { const auto found = types.find(type); return found == types.end() ? ThreadTypeFacts{} : found->second; }
    ThreadSafetyAnalysis::ThreadSafetyAnalysis(Diagnostics& diagnostics) : diagnostics_(diagnostics) {}
    ThreadSafetyResult ThreadSafetyAnalysis::analyze(const AIRModule& module, const TypeCheckResult& typed)
    {
        if (!verifyAIR(module, typed.types, diagnostics_)) return {};
        return Analysis(module, typed).run();
    }
    bool ThreadSafetyAnalysis::validateOwnedWorker(const ThreadSafetyResult& analysis, SymbolId invoke,
        TypeId callable, TypeId result, SourceRange range)
    {
        const auto fail = [&](std::string message) { diagnostics_.push_back({range, std::move(message)}); return false; };
        if (!analysis.validAIR) return fail("Worker admission requires verified AIR and a completed thread-safety analysis.");
        const auto capture = analysis.traits(callable), output = analysis.traits(result);
        if (!capture.send || !output.send) return fail("Worker callable captures and result must satisfy compiler-derived Send, including their cleanup effects.");
        if (capture.containsBorrow || output.containsBorrow)
            return fail("Owned worker captures and results cannot contain borrowed views; scoped/static lifetime proof is not enabled in this profile.");
        const auto signature = analysis.signatures.find(invoke);
        if (signature == analysis.signatures.end() || signature->second.result != result)
            return fail("Worker invocation must resolve to a concrete function with the exact result type.");
        const auto& parameters = signature->second.parameters;
        bool compatible = parameters.size() == 1 && parameters[0] == callable;
        if (parameters.size() == 1)
            if (const auto parameter = analysis.descriptions.find(parameters[0]); parameter != analysis.descriptions.end()
                && parameter->second.kind == TypeKind::Reference && parameter->second.element == callable) compatible = true;
        if (parameters.empty())
            if (const auto captureType = analysis.descriptions.find(callable); captureType != analysis.descriptions.end() && captureType->second.kind == TypeKind::Unit) compatible = true;
        if (!compatible) return fail("Worker invocation must accept only its exact concrete callable environment.");
        const auto effects = analysis.functions.find(invoke);
        if (effects == analysis.functions.end() || !effects->second.workerSafe)
        {
            std::string message = "Worker invocation has an unapproved transitive effect";
            if (effects != analysis.functions.end() && effects->second.issue)
            {
                const auto& found = *effects->second.issue; message += ": " + found.message + " [call path";
                for (const auto symbol : found.callPath) message += " @" + std::to_string(symbol);
                message += ']';
            }
            return fail(std::move(message));
        }
        return true;
    }

    bool ThreadSafetyAnalysis::validateOwnedReady(const ThreadSafetyResult& analysis, TypeId result, SourceRange range)
    {
        if (!analysis.validAIR)
        { diagnostics_.push_back({range,"Ready admission requires verified AIR and a completed cleanup analysis."}); return false; }
        const auto output = analysis.traits(result);
        if (!output.send || output.containsBorrow)
        { diagnostics_.push_back({range,"Task.ready initially requires owned data with compiler-approved cleanup and no borrowed views or hidden Task loans; it creates no worker."}); return false; }
        return true;
    }

    bool validateTaskWorkers(const AIRModule& module, const TypeCheckResult& typed,
                             Diagnostics& diagnostics)
    {
        std::vector<const AIRInstruction*> workers, ready;
        for (const auto& function : module.functions)
            for (const auto& block : function.blocks)
                for (const auto& instruction : block.instructions)
                    if (instruction.opcode == AIROpcode::TaskIntrinsic)
                        if (const auto* operation = taskOperation(instruction.operation); operation)
                        {
                            if (operation->worker) workers.push_back(&instruction);
                            else if (operation->preparation == TaskPreparation::Ready) ready.push_back(&instruction);
                        }
        if (workers.empty() && ready.empty()) return diagnostics.empty();
        ThreadSafetyAnalysis checker(diagnostics);
        const auto analysis = checker.analyze(module, typed);
        if (!analysis.validAIR) return false;
        bool valid = true;
        for (const auto* instruction : workers)
            valid = checker.validateOwnedWorker(analysis, instruction->taskInvoke,
                instruction->taskCallable, instruction->taskElement, instruction->range) && valid;
        for (const auto* instruction : ready)
            valid = checker.validateOwnedReady(analysis,instruction->taskElement,instruction->range) && valid;
        return valid && diagnostics.empty();
    }
}
