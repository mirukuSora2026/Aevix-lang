// ========================
//
// Created By : miruku_sora2026
//
// File name : JITEngine.hpp
//
// Description :
//     LLVM 의존성을 숨기는 재사용 가능한 네이티브 JIT 세션과 서명 검사 호출 API를 정의한다.
//     Define a reusable native JIT session and signature-checked invocation API without exposing LLVM.
//
// ========================

#pragma once

#include "aevix/JIT/JITOptions.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace aevix
{
    using JITModuleId = std::uint64_t;
    inline constexpr JITModuleId InvalidJITModuleId = 0;

    struct JITInvocationResult final
    {
        bool succeeded = false;
        std::int32_t value = 0;
        std::string message;
    };

    class JITEngine final
    {
    public:
        static bool available() noexcept;
        static std::unique_ptr<JITEngine> create(const JITOptions& options, std::string& error);
        ~JITEngine();

        JITEngine(const JITEngine&) = delete;
        JITEngine& operator=(const JITEngine&) = delete;

        // 추가한 모듈의 전역 저장소는 세션이 끝날 때까지 여러 호출 사이에서 공유된다.
        // Added modules retain their global storage across invocations until the session ends.
        bool addModule(std::string_view llvmIR, std::string& error);
        // 추적 모듈은 실패 시 등록되지 않으며 ID로 코드와 주소 캐시를 해제한다.
        // Tracked modules commit only on success; their ID releases code and cached addresses.
        std::optional<JITModuleId> addModuleTracked(std::string_view llvmIR, std::string& error);
        // 다른 모듈이 참조하거나 종료 콜백이 있는 모듈은 세션 종료까지 유지한다.
        // Modules referenced by others or owning exit callbacks remain until session teardown.
        // 호스트로 유출된 코드 주소가 없다는 사실은 호출자가 보장해야 한다.
        // The caller must ensure no code address has escaped into retained host callbacks.
        bool removeModule(JITModuleId module, std::string& error);
        // 등록하는 주소의 실제 C ABI 서명은 LLVM 선언과 일치해야 한다.
        // A registered address must have the C ABI signature declared by the LLVM module.
        bool defineSymbol(std::string_view name, std::uintptr_t address, std::string& error);
        JITInvocationResult invokeI32(std::string_view function = "main");
        JITInvocationResult invokeVoid(std::string_view function);
        std::string targetTriple() const;

        // JIT 코드는 현재 프로세스에서 실행된다. 격리가 필요하면 호출자가 자식 프로세스를 사용한다.
        // JIT code runs in this process; callers needing isolation must invoke it in a child process.
    private:
        struct Impl;
        explicit JITEngine(std::unique_ptr<Impl> implementation);
        std::unique_ptr<Impl> implementation_;
    };
}
