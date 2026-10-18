// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_task_scope_jit_prepare_tests.cpp
//
// Description :
//     실제 소스의 준비와 확정 사이에서 입장을 닫아도 JIT 작업과 소유권이 올바르게 완료되는지 검사한다.
//     Close admission between actual source prepare and commit, then verify JIT work and ownership complete correctly.
//
// ========================
#include "aevix/Backend/LLVM/LLVMCodeGen.hpp"
#include "aevix/Frontend/Modules/ModuleLoader.hpp"
#include "aevix/Frontend/Ownership/BorrowChecker.hpp"
#include "aevix/Frontend/Sema/ControlFlowChecker.hpp"
#include "aevix/Frontend/Sema/CoreErrorTypes.hpp"
#include "aevix/Frontend/Sema/ForeignValidator.hpp"
#include "aevix/Frontend/Sema/GenericResolver.hpp"
#include "aevix/Frontend/Sema/NameResolver.hpp"
#include "aevix/IR/Lowering/ASTLowering.hpp"
#include "aevix/JIT/JITEngine.hpp"
#include "aevix/Runtime/TaskScopeRuntime.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

namespace
{
    unsigned checks = 0, failures = 0;
    std::atomic<unsigned> observations{0}, failureObservations{0};
    std::atomic<bool> invalidObservation{false};
    void expect(bool valid, const char* label, const std::string& detail = {})
    {
        ++checks;
        if (!valid) { ++failures; std::cerr << "FAIL: " << label << ' ' << detail << '\n'; }
    }
    // 이 래퍼는 테스트의 닫힌 네이티브 ABI만 치환한다. 소스에 토큰이나 준비 저장소를 노출하지 않는다.
    // Replace only the test's closed native ABI; expose neither tokens nor prepared storage to source code.
    extern "C" std::int32_t preparedAdmission(std::uint64_t scope, std::uint64_t captureSize, std::uint64_t captureAlignment,
        std::uint64_t resultSize, std::uint64_t resultAlignment, aevix::TaskInvoke invoke, aevix::TaskDrop captureDrop,
        aevix::TaskDrop resultDrop, std::uint64_t* owner, void** storage, std::uint64_t* diagnosticSerial) noexcept
    {
        const auto result = aevix_task_scope_prepare_identified(scope,captureSize,captureAlignment,resultSize,resultAlignment,
            invoke,captureDrop,resultDrop,owner,storage,diagnosticSerial);
        if (result == 0)
        {
            std::uint64_t active = 99;
            std::uint32_t state = 99;
            const bool valid = aevix_task_scope_close_admission(scope) == 0
                && aevix_task_scope_active_count(scope,&active) == 0 && active == 1
                && aevix_task_scope_status(scope,&state) == 0 && state == std::uint32_t(aevix::TaskScopeState::Closing);
            if (!valid) invalidObservation = true;
            ++observations;
        }
        return result;
    }
    extern "C" std::int32_t borrowedAdmissionFailure(std::uint64_t) noexcept
    {
        ++failureObservations;
        return std::int32_t(aevix::TaskRuntimeStatus::SizeOverflow);
    }
    std::optional<std::string> compile(aevix::Diagnostics& diagnostics)
    {
        const auto directory = std::filesystem::temp_directory_path() /
            ("aevix-scope-prepare-jit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        struct Cleanup final { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path,ignored); } } cleanup{directory};
        const auto path = directory / "main.avx";
        { std::ofstream file(path); file << R"AVX(
import task { Task, TaskScope, Executor, ScopeOptions, ScopeStatus, ScopeReport, SpawnError, ScopeError, TaskError };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
i32 main() {
    Executor executor = Executor.native_threads();
    TaskScope scope = success(Task.open_scope(&executor, ScopeOptions.defaults()));
    Result<unit,ScopeError> failed = Task.close_admission(&scope);
    switch (move failed) { case Result<unit,ScopeError>.Ok: return 5; break; case Result<unit,ScopeError>.Err { ScopeError error = error }: switch (move error) { case ScopeError.Failure { TaskError reason = reason }: switch (reason) { case TaskError.Overflow: break; default: return 6; break; } break; case ScopeError.Retry: return 7; break; } break; }
    switch (Task.scope_status(&scope)) { case ScopeStatus.Open: break; case ScopeStatus.Closing: return 8; break; }
    if (Task.active_count(&scope) != 0usize) return 9;
    i32 input = 41;
    Task<i32> work = success(Task.spawn_owned(&scope, [input]() -> i32 { return input + 1; }));
    switch (Task.scope_status(&scope)) { case ScopeStatus.Open: return 1; break; case ScopeStatus.Closing: break; }
    Result<Task<i32>,SpawnError> rejected = Task.spawn_owned(&scope, []() -> i32 { return 0; });
    switch (move rejected) { case Result<Task<i32>,SpawnError>.Ok: return 2; break; case Result<Task<i32>,SpawnError>.Err { SpawnError error = error }: switch (error) { case SpawnError.ScopeClosed: break; default: return 3; break; } break; }
    i32 value = success(Task.join(work));
    ScopeReport report = success(Task.close_scope(scope));
    return value == 42 && report.accepted == 1u64 && report.observed == 1u64 && report.completed == 1u64 ? 0 : 4;
}
)AVX"; }
        aevix::SourceManager sources;
        auto graph = aevix::ModuleLoader(sources,diagnostics).load(path,{});
        if (!graph || !diagnostics.empty() || !aevix::installCoreErrorTypes(*graph,sources,diagnostics)) return std::nullopt;
        auto names = aevix::NameResolver(diagnostics).resolve(*graph);
        if (!names.succeeded || !diagnostics.empty()) return std::nullopt;
        auto concrete = aevix::GenericResolver(diagnostics).resolve(*graph,names);
        if (!concrete || !diagnostics.empty()) return std::nullopt;
        names = std::move(*concrete);
        auto typed = aevix::TypeChecker(diagnostics).check(*graph,names);
        if (!typed.succeeded || !diagnostics.empty() || !aevix::validateForeignDeclarations(names,typed,diagnostics)
            || !aevix::ControlFlowChecker(diagnostics).check(*graph,names,typed)) return std::nullopt;
        auto air = aevix::lowerToAIR(*graph,names,typed,diagnostics);
        if (!air || !diagnostics.empty() || !aevix::BorrowChecker(diagnostics).check(*air,names,typed).succeeded) return std::nullopt;
        return aevix::emitLLVM(*air,typed,names,diagnostics);
    }
}
int main()
{
    if (!aevix::JITEngine::available()) return 77;
    aevix::Diagnostics diagnostics;
    auto ir = compile(diagnostics);
    std::string messages;
    for (const auto& error : diagnostics) messages += error.message + '\n';
    expect(ir && diagnostics.empty(),"complete checked source pipeline",messages);
    if (!ir) return 1;
    const std::string original = "@aevix_task_scope_prepare_identified(", replacement = "@aevix_test_scope_prepared_admission(";
    std::size_t position = 0, replaced = 0;
    while ((position = ir->find(original,position)) != std::string::npos)
    { ir->replace(position,original.size(),replacement); position += replacement.size(); ++replaced; }
    expect(replaced >= 2,"replace actual native prepare call and declaration only");
    const std::string closeOriginal = "@aevix_task_scope_close_admission(", closeReplacement = "@aevix_test_scope_admission_failure(";
    position = 0; replaced = 0;
    while ((position = ir->find(closeOriginal,position)) != std::string::npos)
    { ir->replace(position,closeOriginal.size(),closeReplacement); position += closeReplacement.size(); ++replaced; }
    expect(replaced >= 2,"replace borrowed-operation fault call and declaration only");
    for (const auto optimization : {0u,2u})
    {
        aevix::JITOptions options; options.optimization = optimization;
        std::string error;
        auto engine = aevix::JITEngine::create(options,error);
        expect(engine != nullptr,"create JIT engine",error);
        if (!engine) continue;
        const bool defined = engine->defineSymbol("aevix_test_scope_prepared_admission",reinterpret_cast<std::uintptr_t>(&preparedAdmission),error);
        expect(defined,"define exact test ABI signature",error);
        if (!defined) continue;
        const bool definedFailure = engine->defineSymbol("aevix_test_scope_admission_failure",reinterpret_cast<std::uintptr_t>(&borrowedAdmissionFailure),error);
        expect(definedFailure,"define exact non-owning Failure test ABI",error);
        if (!definedFailure) continue;
        const bool added = engine->addModule(*ir,error);
        expect(added,"add checked source module",error);
        if (!added) continue;
        for (unsigned repeat = 0; repeat < 8; ++repeat)
        {
            const auto before = observations.load(), beforeFailure = failureObservations.load();
            const auto result = engine->invokeI32();
            expect(result.succeeded && result.value == 0,"commit accepted preparation after admission closes",result.message + " value=" + std::to_string(result.value));
            expect(observations == before + 1 && failureObservations == beforeFailure + 1 && !invalidObservation,"prepared commit and non-owning Failure preserve the exact original Scope");
        }
    }
    std::cout << checks << " source prepared-admission JIT checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
