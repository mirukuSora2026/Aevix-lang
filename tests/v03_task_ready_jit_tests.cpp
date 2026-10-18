// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_task_ready_jit_tests.cpp
//
// Description :
//     작업자 없는 Ready의 반복 JIT 정리, 코드 수명과 준비 중 입장 종료를 검사한다.
//     Check repeated workerless Ready cleanup, code lifetime, and admission closure during real preparation.
//
// ========================

#include "aevix/Backend/LLVM/LLVMCodeGen.hpp"
#include "aevix/Frontend/Modules/ModuleLoader.hpp"
#include "aevix/Frontend/Ownership/BorrowChecker.hpp"
#include "aevix/Frontend/Ownership/ThreadSafetyAnalysis.hpp"
#include "aevix/Frontend/Sema/ControlFlowChecker.hpp"
#include "aevix/Frontend/Sema/CoreErrorTypes.hpp"
#include "aevix/Frontend/Sema/ForeignValidator.hpp"
#include "aevix/Frontend/Sema/GenericResolver.hpp"
#include "aevix/Frontend/Sema/NameResolver.hpp"
#include "aevix/IR/Lowering/ASTLowering.hpp"
#include "aevix/JIT/JITEngine.hpp"
#include "aevix/Runtime/TaskScopeRuntime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace
{
    unsigned checks = 0, failures = 0;
    std::atomic<unsigned> shutdownCallbacks = 0, teardownCallbacks = 0, activeCallbacks = 0;
    std::atomic<bool> tearingDown = false, engineAlive = false, lateCallback = false;

    void expect(bool condition, std::string_view label, std::string_view detail = {})
    {
        ++checks;
        if (condition) return;
        ++failures;
        std::cerr << "FAIL: " << label;
        if (!detail.empty()) std::cerr << ": " << detail;
        std::cerr << '\n';
    }

    // 실제 생성된 종료 함수가 실행될 때만 관측한다. 작업 실행이나 소멸을 대신하지 않는다.
    // Observe only the actual generated shutdown callback; never simulate task execution or cleanup.
    extern "C" void observeTaskShutdown() noexcept
    {
        ++activeCallbacks;
        if (!engineAlive.load()) lateCallback = true;
        if (tearingDown.load()) ++teardownCallbacks;
        ++shutdownCallbacks;
        --activeCallbacks;
    }

    struct Fixture final
    {
        std::filesystem::path directory;
        aevix::SourceManager sources;
        aevix::Diagnostics diagnostics;
        std::optional<aevix::ModuleGraph> graph;
        aevix::NameResolution names;
        aevix::TypeCheckResult types;
        std::optional<aevix::AIRModule> air;
        std::optional<std::string> llvm;
        std::string stage = "module";

        explicit Fixture(std::string_view source)
        {
            static std::atomic<unsigned> sequence = 0;
            directory = std::filesystem::temp_directory_path() /
                ("aevix-task-jit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                 + "-" + std::to_string(++sequence));
            std::filesystem::create_directories(directory);
            { std::ofstream output(directory / "main.avx"); output << source; }
            graph = aevix::ModuleLoader(sources, diagnostics).load(directory / "main.avx", {});
            if (!graph || !diagnostics.empty()) return;
            stage = "core";
            if (!aevix::installCoreErrorTypes(*graph, sources, diagnostics)) return;
            stage = "names";
            names = aevix::NameResolver(diagnostics).resolve(*graph);
            if (!names.succeeded || !diagnostics.empty()) return;
            stage = "generics";
            auto concrete = aevix::GenericResolver(diagnostics).resolve(*graph, names);
            if (!concrete || !diagnostics.empty()) return;
            names = std::move(*concrete);
            stage = "types";
            types = aevix::TypeChecker(diagnostics).check(*graph, names);
            if (!types.succeeded || !diagnostics.empty()) return;
            stage = "foreign";
            if (!aevix::validateForeignDeclarations(names, types, diagnostics)) return;
            stage = "flow";
            if (!aevix::ControlFlowChecker(diagnostics).check(*graph, names, types)) return;
            stage = "lowering";
            air = aevix::lowerToAIR(*graph, names, types, diagnostics);
            if (!air || !diagnostics.empty()) return;
            stage = "borrow";
            if (!aevix::BorrowChecker(diagnostics).check(*air, names, types).succeeded) return;
            stage = "verify";
            if (!aevix::verifyAIR(*air, types.types, diagnostics)) return;
            stage = "workers";
            if (!aevix::validateTaskWorkers(*air, types, diagnostics)) return;
            stage = "llvm";
            llvm = aevix::emitLLVM(*air, types, names, diagnostics);
            if (llvm && diagnostics.empty()) stage = "complete";
        }

        ~Fixture()
        {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }

        std::string messages() const
        {
            std::string output = stage + ": ";
            for (const auto& diagnostic : diagnostics) output += diagnostic.message + "\n";
            return output;
        }
    };

    const std::string imports = R"AVX(
import task { Task, TaskScope, Executor, ScopeOptions };
import memory { Shared };
import atomic { Atomic, MemoryOrder };
)AVX";

    const std::string source = imports + R"AVX(
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; default: Never.unreachable(); break; } }
struct Capture implements Drop {
    public Shared<Atomic<i32>> counter;
    public i32 amount;
    public void drop(&mut Self self) { success(Atomic.fetch_add(Shared.as_ref(&self.counter),1,MemoryOrder.SeqCst)); return; }
};
Capture capture(&Shared<Atomic<i32>> counter,i32 amount) { return Capture { counter = success(Shared.clone(counter)), amount = amount }; }
i32 main() {
    Shared<Atomic<i32>> counter = success(Shared.new(Atomic.new(0)));
    Executor executor = Executor.native_threads();
    {
        TaskScope scope = success(Task.open_scope(&executor,ScopeOptions.defaults()));
        Task<Capture> first = success(Task.ready(&scope,capture(&counter,42)));
        { Capture result = success(Task.join(first)); if (result.amount != 42) return 1; }
        Task<Capture> second = success(Task.ready(&scope,capture(&counter,7)));
        success(Task.discard_result(second));
        Task<Capture> implicit = success(Task.ready(&scope,capture(&counter,8)));
    }
    if (success(Atomic.load(Shared.as_ref(&counter),MemoryOrder.SeqCst)) != 3 || Shared.strong_count(&counter) != 1usize) return 2;
    return 42;
}
)AVX";

    std::optional<std::string> observeGeneratedShutdown(std::string ir)
    {
        const std::string name = "define void @aevix.module.shutdown() {";
        const auto begin = ir.find(name);
        const auto end = begin == std::string::npos ? std::string::npos : ir.find("\n}", begin);
        const auto returned = end == std::string::npos ? std::string::npos : ir.rfind("  ret void", end);
        if (returned == std::string::npos || returned < begin) return std::nullopt;
        // 검증된 Ready AIR·결과 정리 thunk·전역 콜백을 보존하고 종료 관측 호출 하나만 넣는다.
        // Preserve checked Ready AIR, result Drop thunks and global callbacks; insert one shutdown observation only.
        ir.insert(returned, "  call void @aevix_test_task_shutdown_observed()\n");
        ir += "\ndeclare void @aevix_test_task_shutdown_observed()\n";
        return ir;
    }

    bool repeatedSession(const Fixture& fixture, unsigned optimization)
    {
        constexpr unsigned invocations = 8;
        shutdownCallbacks = teardownCallbacks = activeCallbacks = 0;
        tearingDown = lateCallback = false;
        engineAlive = true;
        aevix::JITOptions options;
        options.optimization = optimization;
        std::string error;
        auto engine = aevix::JITEngine::create(options, error);
        expect(engine != nullptr && error.empty(), "create source Task JIT session", error);
        if (!engine) { engineAlive = false; return false; }
        const auto instrumented = observeGeneratedShutdown(*fixture.llvm);
        expect(instrumented.has_value(), "instrument actual generated module shutdown");
        if (!instrumented) { engine.reset(); engineAlive = false; return false; }
        const bool registered = engine->defineSymbol("aevix_test_task_shutdown_observed",
            reinterpret_cast<std::uintptr_t>(&observeTaskShutdown), error);
        expect(registered, "register shutdown observer through the public JIT API", error);
        if (!registered) { engine.reset(); engineAlive = false; return false; }
        const auto module = engine->addModuleTracked(*instrumented, error);
        expect(module.has_value(), "add actual source Task module with tracked resources", error);
        if (!module) { engine.reset(); engineAlive = false; return false; }
        error.clear();
        expect(!engine->removeModule(*module, error) && error.find("callbacks") != std::string::npos,
               "generated callbacks pin Task code even without persist", error);
        bool completed = true;
        for (unsigned invocation = 0; invocation < invocations; ++invocation)
        {
            const auto result = engine->invokeI32("main");
            expect(result.succeeded && result.value == 42 && result.message.empty(),
                   "repeat source Scope admission, join, discard and exact Drop in one engine",
                   result.message + " (returned " + std::to_string(result.value) + ")");
            if (!result.succeeded || result.value != 42) { completed = false; break; }
            expect(shutdownCallbacks == invocation + 1 && teardownCallbacks == 0 && activeCallbacks == 0,
                   "main returns only after its generated shutdown observer completes");
            error.clear();
            expect(!engine->removeModule(*module, error) && error.find("callbacks") != std::string::npos,
                   "rejected code unload preserves later source invocations", error);
        }
        const auto beforeTeardown = shutdownCallbacks.load();
        tearingDown = true;
        engine.reset();
        engineAlive = false;
        tearingDown = false;
        expect(teardownCallbacks == 1 && shutdownCallbacks == beforeTeardown + 1,
               "ORC deinitialization calls actual generated destructor once before releasing code");
        expect(activeCallbacks == 0 && !lateCallback,
               "all generated callbacks finish while the engine and tracker-backed code remain alive");
        return completed;
    }

    std::atomic<unsigned> admissionChecks{0};
    std::atomic<bool> invalidPreparedState{false};
    extern "C" std::int32_t readyPrepared(std::uint64_t scope,std::uint64_t bytes,std::uint64_t alignment,
        aevix::TaskDrop cleanup,std::uint64_t* observation,void** storage,std::uint64_t* identity) noexcept
    {
        const auto result = aevix_task_scope_prepare_ready(scope,bytes,alignment,cleanup,observation,storage,identity);
        if (!result)
        {
            std::uint64_t active=99;
            if (aevix_task_scope_active_count(scope,&active) || active != 1 || aevix_task_scope_close_admission(scope))
                invalidPreparedState = true;
            ++admissionChecks;
        }
        return result;
    }
    void checkedPreparedAdmission()
    {
        Fixture fixture(R"AVX(
import task { Task, TaskScope, Executor, ScopeOptions, ScopeReport, SpawnError };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; default: Never.unreachable(); break; } }
i32 main() {
    Executor executor = Executor.native_threads(); TaskScope scope = success(Task.open_scope(&executor,ScopeOptions.defaults()));
    Task<i32> work = success(Task.ready(&scope,42));
    Result<Task<i32>,SpawnError> rejected = Task.ready(&scope,9);
    switch (move rejected) { case Result<Task<i32>,SpawnError>.Err { SpawnError reason = error }: switch (reason) { case SpawnError.ScopeClosed: break; default: return 1; break; } break; default: return 2; break; }
    i32 result = success(Task.join(work)); ScopeReport report = success(Task.close_scope(scope));
    return result == 42 && report.accepted == 1u64 && report.completed == 1u64 && report.observed == 1u64 ? 42 : 3;
}
)AVX");
        expect(fixture.llvm.has_value(),"compile real Ready admission transaction",fixture.messages());
        if (!fixture.llvm) return;
        auto ir=*fixture.llvm;
        const std::string original="@aevix_task_scope_prepare_ready(", replacement="@aevix_test_ready_prepared(";
        std::size_t position=0, replaced=0;
        while ((position=ir.find(original,position))!=std::string::npos)
        { ir.replace(position,original.size(),replacement);position+=replacement.size();++replaced; }
        expect(replaced>=3,"wrap exact Ready prepare calls and declaration");
        std::string error;auto engine=aevix::JITEngine::create({},error);
        expect(engine!=nullptr,"create Ready admission JIT engine",error);if(!engine)return;
        expect(engine->defineSymbol("aevix_test_ready_prepared",reinterpret_cast<std::uintptr_t>(&readyPrepared),error),"bind actual Ready prepare observer",error);
        const auto module=engine->addModuleTracked(ir,error);expect(module.has_value(),"load Ready admission module",error);if(!module)return;
        const auto result=engine->invokeI32("main");
        expect(result.succeeded && result.value==42,"Ready commit survives admission closure before publication",result.message);
        expect(admissionChecks==1 && !invalidPreparedState,"prepared Ready counted active once with no fake execution");
    }

    void checkedForeignBoundaries()
    {
        for (const auto prototype : {
                 "extern \"C\" Task<i32> leak_task();",
                 "extern \"C\" i32 pass_task(Task<i32> owner);",
                 "extern \"C\" i32 pass_scope(TaskScope owner);"})
        {
            Fixture invalid(imports + prototype + "\ni32 main() { return 0; }\n");
            expect(!invalid.llvm && !invalid.diagnostics.empty() && invalid.stage == "foreign",
                   "checked C FFI cannot expose Task or Scope owners", invalid.messages());
        }
    }
}

int main()
{
    if (!aevix::JITEngine::available())
    {
        std::cout << "Source Task JIT lifetime checks require ORC JIT support.\n";
        return 77;
    }
    Fixture fixture(source);
    expect(fixture.llvm.has_value() && fixture.diagnostics.empty() && fixture.stage == "complete",
           "actual Aevix Task source passes the complete checked compilation pipeline", fixture.messages());
    if (fixture.llvm)
    {
        expect(fixture.air && std::none_of(fixture.air->globals.begin(), fixture.air->globals.end(),
            [](const auto& global) { return global.isPersist; }), "lifecycle regression has no persist storage");
        expect(fixture.llvm->find("@llvm.global_ctors") != std::string::npos
            && fixture.llvm->find("@llvm.global_dtors") != std::string::npos,
            "real source compilation emits both module lifecycle callback lists");
        expect(fixture.llvm->find("@aevix.task.ready.drop.") != std::string::npos
            && fixture.llvm->find("define internal i32 @aevix.task.invoke.") == std::string::npos
            && fixture.llvm->find("call i32 @aevix_task_scope_prepare_identified(") == std::string::npos,
            "Ready-only module retains real result Drop thunks and no worker invoke/preparation");
        if (repeatedSession(fixture, 0)) repeatedSession(fixture, 2);
        checkedPreparedAdmission();
        checkedForeignBoundaries();
    }
    std::cout << checks << " workerless Ready JIT checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
