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

#include <llvm/AsmParser/Parser.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include <map>
#include <mutex>
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
        // 엔진을 마지막에 선언하여 엔진 종료 중 오류 콜백의 저장소가 아직 살아 있게 한다.
        // Declare the engine last so error-callback storage survives engine shutdown.
        std::unique_ptr<llvm::orc::LLJIT> engine;

        ~Impl()
        {
            if (engine)
                if (auto error = engine->deinitialize(engine->getMainJITDylib()))
                    llvm::consumeError(std::move(error));
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
            auto address = engine->lookup(llvm::StringRef(function.data(), function.size()));
            if (!address) return {false, 0, errorMessage(address.takeError())};
            if (wanted == CallableSignature::I32)
                return {true, address->toPtr<std::int32_t (*)()>()(), {}};
            address->toPtr<void (*)()>()();
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
        return session;
    }

    bool JITEngine::addModule(std::string_view llvmIR, std::string& error)
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
            return false;
        }
        const auto& native = implementation_->engine->getTargetTriple();
        const auto& requested = module->getTargetTriple();
        if (!requested.str().empty()
            && (requested.getArch() != native.getArch()
                || requested.getObjectFormat() != native.getObjectFormat()
                || (requested.getOS() != native.getOS() && !(requested.isOSDarwin() && native.isOSDarwin()))))
        {
            error = "JIT modules must target the running host (" + native.str() + ").";
            return false;
        }
        const auto& layout = implementation_->engine->getDataLayout();
        if (!module->getDataLayoutStr().empty() && module->getDataLayout() != layout)
        {
            error = "JIT module data layout does not match the native target.";
            return false;
        }
        module->setTargetTriple(native);
        module->setDataLayout(layout);
        llvm::raw_string_ostream verification(error);
        if (llvm::verifyModule(*module, &verification)) return false;
        std::map<std::string, CallableSignature, std::less<>> added;
        for (const auto& function : *module)
        {
            if (function.isDeclaration() || function.hasLocalLinkage()) continue;
            const auto name = function.getName().str();
            if (implementation_->signatures.contains(name))
            {
                error = "JIT function is already defined in this session: " + name;
                return false;
            }
            added.emplace(name, signature(function));
        }
        if (auto failure = implementation_->engine->addIRModule(
            llvm::orc::ThreadSafeModule(std::move(module), std::move(context))))
        {
            error = implementation_->errorMessage(std::move(failure));
            return false;
        }
        implementation_->signatures.insert(added.begin(), added.end());
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
