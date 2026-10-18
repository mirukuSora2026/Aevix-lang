// ========================
//
// Created By : miruku_sora2026
//
// File name : ORCJIT.cpp
//
// Description :
//     LLVM IR을 검증하고 ORC LLJIT 세션에 연결하여 네이티브 함수와 C 심벌을 실행한다.
//     Verify LLVM IR and connect it to an ORC LLJIT session for native function and C-symbol execution.
//
// ========================

#include "aevix/JIT/JITEngine.hpp"

#ifndef AEVIX_HAS_LLVM_ORC
#define AEVIX_HAS_LLVM_ORC 0
#endif

#if AEVIX_HAS_LLVM_ORC
#include "aevix/Runtime/PersistRuntime.hpp"
#include "aevix/Runtime/TextRuntime.hpp"
#include "aevix/Runtime/ParseRuntime.hpp"
#include "aevix/Runtime/SystemRuntime.hpp"
#include "aevix/Runtime/OsRuntime.hpp"
#include "aevix/Runtime/SharedRuntime.hpp"
#include "aevix/Runtime/AtomicRuntime.hpp"
#include "aevix/Runtime/LockRuntime.hpp"
#include "aevix/Runtime/TaskRuntime.hpp"
#include "aevix/Runtime/TaskScopeRuntime.hpp"
#include "aevix/Runtime/CancellationRuntime.hpp"
#include <cmath>
#include "aevix/Runtime/Integer128Runtime.hpp"
#include "aevix/Runtime/ComplexRuntime.hpp"
#include "aevix/Runtime/BenchRuntime.hpp"
#include "aevix/Runtime/ParallelRuntime.hpp"

#include <llvm/AsmParser/Parser.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include <map>
#include <mutex>
#include <set>
#include <utility>
#endif

namespace aevix
{
#if AEVIX_HAS_LLVM_ORC
    namespace
    {
        enum class CallableSignature { Unsupported, I32, Void };

        CallableSignature signature(const llvm::Function& function)
        {
            if (function.isVarArg() || function.arg_size() != 0
                || function.getCallingConv() != llvm::CallingConv::C) return CallableSignature::Unsupported;
            if (function.getReturnType()->isIntegerTy(32)) return CallableSignature::I32;
            if (function.getReturnType()->isVoidTy()) return CallableSignature::Void;
            return CallableSignature::Unsupported;
        }

        bool initializeNativeTarget()
        {
            static std::once_flag initialized;
            static bool succeeded = false;
            std::call_once(initialized, []
            {
                succeeded = !llvm::InitializeNativeTarget()
                    && !llvm::InitializeNativeTargetAsmPrinter()
                    && !llvm::InitializeNativeTargetAsmParser();
            });
            return succeeded;
        }
    }

    struct JITEngine::Impl final
    {
        std::mutex operations;
        std::mutex errors;
        std::string asynchronousErrors;
        std::map<std::string, CallableSignature, std::less<>> signatures;
        struct Module final
        {
            llvm::orc::ResourceTrackerSP resources;
            std::set<std::string, std::less<>> definitions;
            std::set<std::string, std::less<>> references;
            bool hasExitCallbacks = false;
        };
        unsigned optimization = 0;
        JITModuleId nextModule = 1;
        std::map<JITModuleId, Module> modules;
        std::map<std::string, JITModuleId, std::less<>> definitions;
        std::map<std::string, std::uintptr_t, std::less<>> addresses;
        // 엔진을 마지막에 선언하여 엔진 종료 중 오류 콜백의 저장소가 아직 살아 있게 한다.
        // Declare the engine last so error-callback storage survives engine shutdown.
        std::unique_ptr<llvm::orc::LLJIT> engine;

        ~Impl()
        {
            if (engine)
                if (auto error = engine->deinitialize(engine->getMainJITDylib()))
                    llvm::consumeError(std::move(error));
            // 추적기의 마지막 참조도 ExecutionSession이 살아 있는 동안 해제한다.
            // Release final tracker references while their ExecutionSession is still alive.
            modules.clear();
        }

        std::string errorMessage(llvm::Error error)
        {
            auto message = llvm::toString(std::move(error));
            std::lock_guard guard(errors);
            if (!asynchronousErrors.empty())
            {
                message += "\n" + asynchronousErrors;
                asynchronousErrors.clear();
            }
            return message;
        }

        JITInvocationResult invoke(std::string_view function, CallableSignature wanted)
        {
            std::lock_guard guard(operations);
            const auto found = signatures.find(function);
            if (found == signatures.end()) return {false, 0, "JIT function is not defined in this session: " + std::string(function)};
            if (found->second != wanted)
                return {false, 0, wanted == CallableSignature::I32
                    ? "JIT invocation requires a defined C-convention i32 function with no parameters."
                    : "JIT invocation requires a defined C-convention void function with no parameters."};
            if (auto error = engine->initialize(engine->getMainJITDylib()))
                return {false, 0, errorMessage(std::move(error))};
            auto cached = addresses.find(function);
            if (cached == addresses.end())
            {
                auto address = engine->lookup(llvm::StringRef(function.data(), function.size()));
                if (!address) return {false, 0, errorMessage(address.takeError())};
                cached = addresses.emplace(std::string(function), static_cast<std::uintptr_t>(address->getValue())).first;
            }
            if (wanted == CallableSignature::I32)
                return {true, reinterpret_cast<std::int32_t (*)()>(cached->second)(), {}};
            reinterpret_cast<void (*)()>(cached->second)();
            return {true, 0, {}};
        }
    };

    bool JITEngine::available() noexcept { return true; }

    std::unique_ptr<JITEngine> JITEngine::create(const JITOptions& options, std::string& error)
    {
        error.clear();
        if (options.optimization > 3) { error = "JIT optimization must be between 0 and 3."; return nullptr; }
        if (!initializeNativeTarget()) { error = "LLVM cannot initialize the native JIT target."; return nullptr; }
        auto target = llvm::orc::JITTargetMachineBuilder::detectHost();
        if (!target) { error = llvm::toString(target.takeError()); return nullptr; }
        target->setCodeGenOptLevel(static_cast<llvm::CodeGenOptLevel>(options.optimization));
        auto built = llvm::orc::LLJITBuilder()
            .setJITTargetMachineBuilder(std::move(*target))
            .setLinkProcessSymbolsByDefault(false)
            .setProcessSymbolsJITDylibSetup([](llvm::orc::LLJIT& engine) -> llvm::Expected<llvm::orc::JITDylibSP>
            {
                auto& process = engine.getExecutionSession().createBareJITDylib("Aevix runtime process");
                auto generator = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
                    engine.getDataLayout().getGlobalPrefix());
                if (!generator) return generator.takeError();
                process.addGenerator(std::move(*generator));
                return llvm::orc::JITDylibSP(&process);
            })
            .setPlatformSetUp(llvm::orc::setUpGenericLLVMIRPlatform)
            .create();
        if (!built) { error = llvm::toString(built.takeError()); return nullptr; }
        auto implementation = std::make_unique<Impl>();
        implementation->engine = std::move(*built);
        implementation->optimization = options.optimization;
        auto* state = implementation.get();
        implementation->engine->getExecutionSession().setErrorReporter([state](llvm::Error failure)
        {
            auto message = llvm::toString(std::move(failure));
            std::lock_guard guard(state->errors);
            if (!state->asynchronousErrors.empty()) state->asynchronousErrors += '\n';
            state->asynchronousErrors += message;
        });
        const auto prefix = implementation->engine->getDataLayout().getGlobalPrefix();
        auto& dylib = implementation->engine->getMainJITDylib();
        for (const auto& library : options.sharedLibraries)
        {
            if (library.empty()) { error = "JIT shared-library paths cannot be empty."; return nullptr; }
            auto generator = llvm::orc::DynamicLibrarySearchGenerator::Load(library.string().c_str(), prefix);
            if (!generator) { error = llvm::toString(generator.takeError()); return nullptr; }
            dylib.addGenerator(std::move(*generator));
        }
        if (options.processSymbols)
        {
            auto generator = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(prefix);
            if (!generator) { error = llvm::toString(generator.takeError()); return nullptr; }
            dylib.addGenerator(std::move(*generator));
        }
        auto session = std::unique_ptr<JITEngine>(new JITEngine(std::move(implementation)));
        if (!session->defineSymbol("aevix_persist_ensure",
                reinterpret_cast<std::uintptr_t>(&aevix_persist_ensure), error)
            || !session->defineSymbol("aevix_persist_shutdown",
                reinterpret_cast<std::uintptr_t>(&aevix_persist_shutdown), error)) return nullptr;
        for (const auto& [name, address] : std::initializer_list<std::pair<const char*, std::uintptr_t>>{
#define AEVIX_SCALAR_MATH_UNARY(name) \
            {#name, reinterpret_cast<std::uintptr_t>(static_cast<double(*)(double)>(&std::name))}, \
            {#name "f", reinterpret_cast<std::uintptr_t>(static_cast<float(*)(float)>(&std::name))},
#define AEVIX_SCALAR_MATH_BINARY(name) \
            {#name, reinterpret_cast<std::uintptr_t>(static_cast<double(*)(double, double)>(&std::name))}, \
            {#name "f", reinterpret_cast<std::uintptr_t>(static_cast<float(*)(float, float)>(&std::name))},
#define AEVIX_SCALAR_MATH_TERNARY(name) \
            {#name, reinterpret_cast<std::uintptr_t>(static_cast<double(*)(double, double, double)>(&std::name))}, \
            {#name "f", reinterpret_cast<std::uintptr_t>(static_cast<float(*)(float, float, float)>(&std::name))},
#include "aevix/Native/ScalarMath.def"
#undef AEVIX_SCALAR_MATH_UNARY
#undef AEVIX_SCALAR_MATH_BINARY
#undef AEVIX_SCALAR_MATH_TERNARY
            {"aevix_parallel_for_each", reinterpret_cast<std::uintptr_t>(&aevix_parallel_for_each)},
            {"aevix_integer128_binary", reinterpret_cast<std::uintptr_t>(&aevix_integer128_binary)},
            {"aevix_integer128_to_float", reinterpret_cast<std::uintptr_t>(&aevix_integer128_to_float)},
            {"aevix_integer128_from_float", reinterpret_cast<std::uintptr_t>(&aevix_integer128_from_float)},
            {"aevix_integer128_range_count", reinterpret_cast<std::uintptr_t>(&aevix_integer128_range_count)},
            {"aevix_complex_magnitude", reinterpret_cast<std::uintptr_t>(&aevix_complex_magnitude)},
            {"aevix_complex_divide", reinterpret_cast<std::uintptr_t>(&aevix_complex_divide)},
            {"aevix_parallel_worker_count", reinterpret_cast<std::uintptr_t>(&aevix_parallel_worker_count)},
            {"aevix_bench_begin", reinterpret_cast<std::uintptr_t>(&aevix_bench_begin)},
            {"aevix_bench_end", reinterpret_cast<std::uintptr_t>(&aevix_bench_end)},
            {"aevix_bench_report", reinterpret_cast<std::uintptr_t>(&aevix_bench_report)},
            {"aevix_text_copy", reinterpret_cast<std::uintptr_t>(&aevix_text_copy)},
            {"aevix_text_append", reinterpret_cast<std::uintptr_t>(&aevix_text_append)},
            {"aevix_text_push_char", reinterpret_cast<std::uintptr_t>(&aevix_text_push_char)},
            {"aevix_text_count", reinterpret_cast<std::uintptr_t>(&aevix_text_count)},
            {"aevix_text_decode", reinterpret_cast<std::uintptr_t>(&aevix_text_decode)},
            {"aevix_text_boundary", reinterpret_cast<std::uintptr_t>(&aevix_text_boundary)},
            {"aevix_text_equal", reinterpret_cast<std::uintptr_t>(&aevix_text_equal)},
            {"aevix_parse_scalar", reinterpret_cast<std::uintptr_t>(&aevix_parse_scalar)},
            {"aevix_system_write", reinterpret_cast<std::uintptr_t>(&aevix_system_write)},
            {"aevix_system_flush", reinterpret_cast<std::uintptr_t>(&aevix_system_flush)},
            {"aevix_system_log", reinterpret_cast<std::uintptr_t>(&aevix_system_log)},
            {"aevix_system_input", reinterpret_cast<std::uintptr_t>(&aevix_system_input)},
            {"aevix_system_output", reinterpret_cast<std::uintptr_t>(&aevix_system_output)},
            {"aevix_system_error_code", reinterpret_cast<std::uintptr_t>(&aevix_system_error_code)},
            {"aevix_system_error_progress", reinterpret_cast<std::uintptr_t>(&aevix_system_error_progress)},
            {"aevix_system_format_scalar", reinterpret_cast<std::uintptr_t>(&aevix_system_format_scalar)},
            {"aevix_os_file_open_utf8", reinterpret_cast<std::uintptr_t>(&aevix_os_file_open_utf8)},
            {"aevix_os_file_read", reinterpret_cast<std::uintptr_t>(&aevix_os_file_read)},
            {"aevix_os_file_write", reinterpret_cast<std::uintptr_t>(&aevix_os_file_write)},
            {"aevix_os_file_seek", reinterpret_cast<std::uintptr_t>(&aevix_os_file_seek)},
            {"aevix_os_file_flush", reinterpret_cast<std::uintptr_t>(&aevix_os_file_flush)},
            {"aevix_os_file_close", reinterpret_cast<std::uintptr_t>(&aevix_os_file_close)},
            {"aevix_os_file_cleanup", reinterpret_cast<std::uintptr_t>(&aevix_os_file_cleanup)},
            {"aevix_os_file_size", reinterpret_cast<std::uintptr_t>(&aevix_os_file_size)},
            {"aevix_os_file_set_len", reinterpret_cast<std::uintptr_t>(&aevix_os_file_set_len)},
            {"aevix_os_last_error", reinterpret_cast<std::uintptr_t>(&aevix_os_last_error)},
            {"aevix_os_error_kind", reinterpret_cast<std::uintptr_t>(&aevix_os_error_kind)},
            {"aevix_os_env_get_utf8", reinterpret_cast<std::uintptr_t>(&aevix_os_env_get_utf8)},
            {"aevix_os_env_set_utf8", reinterpret_cast<std::uintptr_t>(&aevix_os_env_set_utf8)},
            {"aevix_os_env_remove_utf8", reinterpret_cast<std::uintptr_t>(&aevix_os_env_remove_utf8)},
            {"aevix_os_monotonic_ns", reinterpret_cast<std::uintptr_t>(&aevix_os_monotonic_ns)},
            {"aevix_os_unix_time_ns", reinterpret_cast<std::uintptr_t>(&aevix_os_unix_time_ns)},
            {"aevix_os_sleep_ns", reinterpret_cast<std::uintptr_t>(&aevix_os_sleep_ns)},
            {"aevix_os_path_normalize_utf8", reinterpret_cast<std::uintptr_t>(&aevix_os_path_normalize_utf8)},
            {"aevix_os_path_metadata_utf8", reinterpret_cast<std::uintptr_t>(&aevix_os_path_metadata_utf8)},
            {"aevix_os_path_mkdir_utf8", reinterpret_cast<std::uintptr_t>(&aevix_os_path_mkdir_utf8)},
            {"aevix_os_path_remove_utf8", reinterpret_cast<std::uintptr_t>(&aevix_os_path_remove_utf8)},
            {"aevix_os_process_spawn", reinterpret_cast<std::uintptr_t>(&aevix_os_process_spawn)},
            {"aevix_os_process_wait", reinterpret_cast<std::uintptr_t>(&aevix_os_process_wait)},
            {"aevix_os_process_try_wait", reinterpret_cast<std::uintptr_t>(&aevix_os_process_try_wait)},
            {"aevix_os_process_kill", reinterpret_cast<std::uintptr_t>(&aevix_os_process_kill)},
            {"aevix_os_process_cleanup", reinterpret_cast<std::uintptr_t>(&aevix_os_process_cleanup)},
            {"aevix_shared_prepare", reinterpret_cast<std::uintptr_t>(&aevix_shared_prepare)},
            {"aevix_shared_commit", reinterpret_cast<std::uintptr_t>(&aevix_shared_commit)},
            {"aevix_shared_abort", reinterpret_cast<std::uintptr_t>(&aevix_shared_abort)},
            {"aevix_shared_clone", reinterpret_cast<std::uintptr_t>(&aevix_shared_clone)},
            {"aevix_shared_release", reinterpret_cast<std::uintptr_t>(&aevix_shared_release)},
            {"aevix_shared_downgrade", reinterpret_cast<std::uintptr_t>(&aevix_shared_downgrade)},
            {"aevix_weak_clone", reinterpret_cast<std::uintptr_t>(&aevix_weak_clone)},
            {"aevix_weak_release", reinterpret_cast<std::uintptr_t>(&aevix_weak_release)},
            {"aevix_weak_upgrade", reinterpret_cast<std::uintptr_t>(&aevix_weak_upgrade)},
            {"aevix_shared_payload", reinterpret_cast<std::uintptr_t>(&aevix_shared_payload)},
            {"aevix_shared_unique_payload", reinterpret_cast<std::uintptr_t>(&aevix_shared_unique_payload)},
            {"aevix_shared_take", reinterpret_cast<std::uintptr_t>(&aevix_shared_take)},
            {"aevix_shared_counts", reinterpret_cast<std::uintptr_t>(&aevix_shared_counts)},
            {"aevix_shared_same_allocation", reinterpret_cast<std::uintptr_t>(&aevix_shared_same_allocation)},
            {"aevix_atomic_operation", reinterpret_cast<std::uintptr_t>(&aevix_atomic_operation)},
            {"aevix_atomic_fence", reinterpret_cast<std::uintptr_t>(&aevix_atomic_fence)},
            {"aevix_atomic_wait", reinterpret_cast<std::uintptr_t>(&aevix_atomic_wait)},
            {"aevix_atomic_alignment", reinterpret_cast<std::uintptr_t>(&aevix_atomic_alignment)},
            {"aevix_atomic_lock_free", reinterpret_cast<std::uintptr_t>(&aevix_atomic_lock_free)},
            {"aevix_mutex_init", reinterpret_cast<std::uintptr_t>(&aevix_mutex_init)},
            {"aevix_mutex_lock", reinterpret_cast<std::uintptr_t>(&aevix_mutex_lock)},
            {"aevix_mutex_try_lock", reinterpret_cast<std::uintptr_t>(&aevix_mutex_try_lock)},
            {"aevix_mutex_lock_timeout", reinterpret_cast<std::uintptr_t>(&aevix_mutex_lock_timeout)},
            {"aevix_mutex_unlock", reinterpret_cast<std::uintptr_t>(&aevix_mutex_unlock)},
            {"aevix_mutex_is_poisoned", reinterpret_cast<std::uintptr_t>(&aevix_mutex_is_poisoned)},
            {"aevix_mutex_clear_poison", reinterpret_cast<std::uintptr_t>(&aevix_mutex_clear_poison)},
            {"aevix_rwlock_init", reinterpret_cast<std::uintptr_t>(&aevix_rwlock_init)},
            {"aevix_rwlock_read", reinterpret_cast<std::uintptr_t>(&aevix_rwlock_read)},
            {"aevix_rwlock_write", reinterpret_cast<std::uintptr_t>(&aevix_rwlock_write)},
            {"aevix_rwlock_try_read", reinterpret_cast<std::uintptr_t>(&aevix_rwlock_try_read)},
            {"aevix_rwlock_try_write", reinterpret_cast<std::uintptr_t>(&aevix_rwlock_try_write)},
            {"aevix_rwlock_read_timeout", reinterpret_cast<std::uintptr_t>(&aevix_rwlock_read_timeout)},
            {"aevix_rwlock_write_timeout", reinterpret_cast<std::uintptr_t>(&aevix_rwlock_write_timeout)},
            {"aevix_rwlock_read_unlock", reinterpret_cast<std::uintptr_t>(&aevix_rwlock_read_unlock)},
            {"aevix_rwlock_write_unlock", reinterpret_cast<std::uintptr_t>(&aevix_rwlock_write_unlock)},
            {"aevix_rwlock_is_poisoned", reinterpret_cast<std::uintptr_t>(&aevix_rwlock_is_poisoned)},
            {"aevix_rwlock_clear_poison", reinterpret_cast<std::uintptr_t>(&aevix_rwlock_clear_poison)},
            {"aevix_cancel_source_new", reinterpret_cast<std::uintptr_t>(&aevix_cancel_source_new)},
            {"aevix_cancel_source_clone", reinterpret_cast<std::uintptr_t>(&aevix_cancel_source_clone)},
            {"aevix_cancel_token_from_source", reinterpret_cast<std::uintptr_t>(&aevix_cancel_token_from_source)},
            {"aevix_cancel_token_clone", reinterpret_cast<std::uintptr_t>(&aevix_cancel_token_clone)},
            {"aevix_cancel_source_request", reinterpret_cast<std::uintptr_t>(&aevix_cancel_source_request)},
            {"aevix_cancel_token_requested", reinterpret_cast<std::uintptr_t>(&aevix_cancel_token_requested)},
            {"aevix_cancel_source_release", reinterpret_cast<std::uintptr_t>(&aevix_cancel_source_release)},
            {"aevix_cancel_token_release", reinterpret_cast<std::uintptr_t>(&aevix_cancel_token_release)},
            {"aevix_task_cancel_token", reinterpret_cast<std::uintptr_t>(&aevix_task_cancel_token)},
            {"aevix_scoped_task_cancel_token", reinterpret_cast<std::uintptr_t>(&aevix_scoped_task_cancel_token)},
            {"aevix_task_scope_open", reinterpret_cast<std::uintptr_t>(&aevix_task_scope_open)},
            {"aevix_task_scope_prepare", reinterpret_cast<std::uintptr_t>(&aevix_task_scope_prepare)},
            {"aevix_task_scope_prepare_identified", reinterpret_cast<std::uintptr_t>(&aevix_task_scope_prepare_identified)},
            {"aevix_scoped_task_commit", reinterpret_cast<std::uintptr_t>(&aevix_scoped_task_commit)},
            {"aevix_task_scope_prepare_ready", reinterpret_cast<std::uintptr_t>(&aevix_task_scope_prepare_ready)},
            {"aevix_scoped_task_ready_commit", reinterpret_cast<std::uintptr_t>(&aevix_scoped_task_ready_commit)},
            {"aevix_scoped_task_abort", reinterpret_cast<std::uintptr_t>(&aevix_scoped_task_abort)},
            {"aevix_scoped_task_join", reinterpret_cast<std::uintptr_t>(&aevix_scoped_task_join)},
            {"aevix_scoped_task_try_take", reinterpret_cast<std::uintptr_t>(&aevix_scoped_task_try_take)},
            {"aevix_scoped_task_wait", reinterpret_cast<std::uintptr_t>(&aevix_scoped_task_wait)},
            {"aevix_scoped_task_wait_timeout", reinterpret_cast<std::uintptr_t>(&aevix_scoped_task_wait_timeout)},
            {"aevix_scoped_task_discard", reinterpret_cast<std::uintptr_t>(&aevix_scoped_task_discard)},
            {"aevix_scoped_task_request_cancel", reinterpret_cast<std::uintptr_t>(&aevix_scoped_task_request_cancel)},
            {"aevix_scoped_task_snapshot", reinterpret_cast<std::uintptr_t>(&aevix_scoped_task_snapshot)},
            {"aevix_task_scope_request_cancel", reinterpret_cast<std::uintptr_t>(&aevix_task_scope_request_cancel)},
            {"aevix_task_scope_snapshot", reinterpret_cast<std::uintptr_t>(&aevix_task_scope_snapshot)},
            {"aevix_task_scope_close_admission", reinterpret_cast<std::uintptr_t>(&aevix_task_scope_close_admission)},
            {"aevix_task_scope_active_count", reinterpret_cast<std::uintptr_t>(&aevix_task_scope_active_count)},
            {"aevix_task_scope_status", reinterpret_cast<std::uintptr_t>(&aevix_task_scope_status)},
            {"aevix_task_scope_close", reinterpret_cast<std::uintptr_t>(&aevix_task_scope_close)},
            {"aevix_task_scope_cleanup", reinterpret_cast<std::uintptr_t>(&aevix_task_scope_cleanup)},
            {"aevix_task_scope_runtime_drain", reinterpret_cast<std::uintptr_t>(&aevix_task_scope_runtime_drain)},
            {"aevix_task_prepare", reinterpret_cast<std::uintptr_t>(&aevix_task_prepare)},
            {"aevix_task_prepare_identified", reinterpret_cast<std::uintptr_t>(&aevix_task_prepare_identified)},
            {"aevix_task_commit", reinterpret_cast<std::uintptr_t>(&aevix_task_commit)},
            {"aevix_task_abort", reinterpret_cast<std::uintptr_t>(&aevix_task_abort)},
            {"aevix_task_join", reinterpret_cast<std::uintptr_t>(&aevix_task_join)},
            {"aevix_task_try_take", reinterpret_cast<std::uintptr_t>(&aevix_task_try_take)},
            {"aevix_task_wait", reinterpret_cast<std::uintptr_t>(&aevix_task_wait)},
            {"aevix_task_wait_timeout", reinterpret_cast<std::uintptr_t>(&aevix_task_wait_timeout)},
            {"aevix_task_cleanup", reinterpret_cast<std::uintptr_t>(&aevix_task_cleanup)},
            {"aevix_task_runtime_drain", reinterpret_cast<std::uintptr_t>(&aevix_task_runtime_drain)},
            {"aevix_task_request_cancel", reinterpret_cast<std::uintptr_t>(&aevix_task_request_cancel)},
            {"aevix_task_snapshot", reinterpret_cast<std::uintptr_t>(&aevix_task_snapshot)},
            {"aevix_task_context_cancel_requested", reinterpret_cast<std::uintptr_t>(&aevix_task_context_cancel_requested)},
            {"aevix_task_last_error", reinterpret_cast<std::uintptr_t>(&aevix_task_last_error)},
            {"aevix_task_error_domain", reinterpret_cast<std::uintptr_t>(&aevix_task_error_domain)}})
            if (!session->defineSymbol(name, address, error)) return nullptr;
        return session;
    }

    bool JITEngine::addModule(std::string_view llvmIR, std::string& error)
    {
        return addModuleTracked(llvmIR, error).has_value();
    }

    std::optional<JITModuleId> JITEngine::addModuleTracked(std::string_view llvmIR, std::string& error)
    {
        error.clear();
        std::lock_guard guard(implementation_->operations);
        auto context = std::make_unique<llvm::LLVMContext>();
        llvm::SMDiagnostic diagnostic;
        auto module = llvm::parseAssemblyString(llvm::StringRef(llvmIR.data(), llvmIR.size()), diagnostic, *context);
        if (!module)
        {
            llvm::raw_string_ostream output(error);
            diagnostic.print("Aevix JIT", output);
            return std::nullopt;
        }
        const auto& native = implementation_->engine->getTargetTriple();
        const auto& requested = module->getTargetTriple();
        if (!requested.str().empty()
            && (requested.getArch() != native.getArch()
                || requested.getObjectFormat() != native.getObjectFormat()
                || (requested.getOS() != native.getOS() && !(requested.isOSDarwin() && native.isOSDarwin()))))
        {
            error = "JIT modules must target the running host (" + native.str() + ").";
            return std::nullopt;
        }
        const auto& layout = implementation_->engine->getDataLayout();
        if (!module->getDataLayoutStr().empty() && module->getDataLayout() != layout)
        {
            error = "JIT module data layout does not match the native target.";
            return std::nullopt;
        }
        module->setTargetTriple(native);
        module->setDataLayout(layout);
        llvm::raw_string_ostream verification(error);
        if (llvm::verifyModule(*module, &verification)) return std::nullopt;
        // 기계어 생성 수준뿐 아니라 실제 LLVM IR 최적화 파이프라인도 적용한다.
        // Apply a real LLVM IR optimization pipeline in addition to code-generation tuning.
        if (implementation_->optimization != 0)
        {
            llvm::LoopAnalysisManager loops;
            llvm::FunctionAnalysisManager functions;
            llvm::CGSCCAnalysisManager calls;
            llvm::ModuleAnalysisManager modules;
            llvm::PassBuilder builder;
            builder.registerModuleAnalyses(modules);
            builder.registerCGSCCAnalyses(calls);
            builder.registerFunctionAnalyses(functions);
            builder.registerLoopAnalyses(loops);
            builder.crossRegisterProxies(loops, functions, calls, modules);
            const auto level = implementation_->optimization == 1 ? llvm::OptimizationLevel::O1
                : implementation_->optimization == 2 ? llvm::OptimizationLevel::O2 : llvm::OptimizationLevel::O3;
            auto pipeline = builder.buildPerModuleDefaultPipeline(level);
            pipeline.run(*module, modules);
            if (llvm::verifyModule(*module, &verification)) return std::nullopt;
        }
        std::map<std::string, CallableSignature, std::less<>> added;
        Impl::Module tracked;
        for (const auto& symbol : module->global_values())
        {
            const auto name = symbol.getName().str();
            if (name.starts_with("llvm."))
            {
                if (name == "llvm.global_ctors" || name == "llvm.global_dtors") tracked.hasExitCallbacks = true;
                continue;
            }
            if (symbol.hasLocalLinkage()) continue;
            if (symbol.isDeclarationForLinker())
            {
                tracked.references.insert(name);
                if (name == "atexit" || name == "__cxa_atexit" || name == "__cxa_thread_atexit")
                    tracked.hasExitCallbacks = true;
                continue;
            }
            if (implementation_->definitions.contains(name))
            {
                error = "JIT symbol is already defined in this session: " + name;
                return std::nullopt;
            }
            tracked.definitions.insert(name);
            if (const auto* function = llvm::dyn_cast<llvm::Function>(&symbol)) added.emplace(name, signature(*function));
        }
        tracked.resources = implementation_->engine->getMainJITDylib().createResourceTracker();
        if (auto failure = implementation_->engine->addIRModule(tracked.resources,
            llvm::orc::ThreadSafeModule(std::move(module), std::move(context))))
        {
            error = implementation_->errorMessage(std::move(failure));
            if (auto cleanup = tracked.resources->remove()) error += "\n" + implementation_->errorMessage(std::move(cleanup));
            return std::nullopt;
        }
        const auto id = implementation_->nextModule++;
        for (const auto& name : tracked.definitions) implementation_->definitions.emplace(name, id);
        implementation_->signatures.insert(added.begin(), added.end());
        implementation_->modules.emplace(id, std::move(tracked));
        return id;
    }

    bool JITEngine::removeModule(JITModuleId id, std::string& error)
    {
        error.clear();
        std::lock_guard guard(implementation_->operations);
        const auto found = implementation_->modules.find(id);
        if (id == InvalidJITModuleId || found == implementation_->modules.end())
        {
            error = "JIT module is not loaded in this session.";
            return false;
        }
        if (found->second.hasExitCallbacks)
        {
            error = "JIT module owns initialization or exit callbacks and must remain until session teardown.";
            return false;
        }
        for (const auto& [other, module] : implementation_->modules)
        {
            if (other == id) continue;
            for (const auto& name : found->second.definitions)
                if (module.references.contains(name))
                {
                    error = "JIT module is still referenced by module " + std::to_string(other) + " through symbol '" + name + "'.";
                    return false;
                }
        }
        if (auto failure = found->second.resources->remove())
        {
            error = implementation_->errorMessage(std::move(failure));
            return false;
        }
        for (const auto& name : found->second.definitions)
        {
            implementation_->definitions.erase(name);
            implementation_->signatures.erase(name);
            implementation_->addresses.erase(name);
        }
        implementation_->modules.erase(found);
        return true;
    }

    JITInvocationResult JITEngine::invokeI32(std::string_view function)
    {
        return implementation_->invoke(function, CallableSignature::I32);
    }
    bool JITEngine::defineSymbol(std::string_view name, std::uintptr_t address, std::string& error)
    {
        error.clear();
        if (name.empty() || name.find('\0') != std::string_view::npos || address == 0)
        {
            error = "A JIT host symbol requires a nonempty name and a nonzero address.";
            return false;
        }
        std::lock_guard guard(implementation_->operations);
        if (implementation_->definitions.contains(name))
        {
            error = "JIT symbol is already defined in this session: " + std::string(name);
            return false;
        }
        auto& engine = *implementation_->engine;
        llvm::orc::MangleAndInterner mangle(engine.getExecutionSession(), engine.getDataLayout());
        llvm::orc::SymbolMap symbols;
        symbols[mangle(llvm::StringRef(name.data(), name.size()))] = llvm::orc::ExecutorSymbolDef(
            llvm::orc::ExecutorAddr(address), llvm::JITSymbolFlags::Exported);
        if (auto failure = engine.getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(symbols))))
        {
            error = implementation_->errorMessage(std::move(failure));
            return false;
        }
        implementation_->definitions.emplace(std::string(name), InvalidJITModuleId);
        return true;
    }
    JITInvocationResult JITEngine::invokeVoid(std::string_view function)
    {
        return implementation_->invoke(function, CallableSignature::Void);
    }
    std::string JITEngine::targetTriple() const
    {
        return implementation_->engine->getTargetTriple().str();
    }
#else
    struct JITEngine::Impl final {};
    bool JITEngine::available() noexcept { return false; }
    std::unique_ptr<JITEngine> JITEngine::create(const JITOptions&, std::string& error)
    {
        error = "LLVM ORC JIT is unavailable in this build; configure with LLVM development libraries to enable it.";
        return nullptr;
    }
    bool JITEngine::addModule(std::string_view, std::string& error)
    {
        error = "LLVM ORC JIT is unavailable in this build.";
        return false;
    }
    std::optional<JITModuleId> JITEngine::addModuleTracked(std::string_view, std::string& error)
    {
        error = "LLVM ORC JIT is unavailable in this build.";
        return std::nullopt;
    }
    bool JITEngine::removeModule(JITModuleId, std::string& error)
    {
        error = "LLVM ORC JIT is unavailable in this build.";
        return false;
    }
    bool JITEngine::defineSymbol(std::string_view, std::uintptr_t, std::string& error)
    {
        error = "LLVM ORC JIT is unavailable in this build.";
        return false;
    }
    JITInvocationResult JITEngine::invokeI32(std::string_view)
    {
        return {false, 0, "LLVM ORC JIT is unavailable in this build."};
    }
    JITInvocationResult JITEngine::invokeVoid(std::string_view)
    {
        return {false, 0, "LLVM ORC JIT is unavailable in this build."};
    }
    std::string JITEngine::targetTriple() const { return {}; }
#endif

    JITEngine::JITEngine(std::unique_ptr<Impl> implementation) : implementation_(std::move(implementation)) {}
    JITEngine::~JITEngine() = default;
}
