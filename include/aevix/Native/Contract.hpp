// ========================
//
// Created By : miruku_sora2026
//
// File name : Contract.hpp
//
// Description :
//     컴파일러 내부 네이티브 호출의 닫힌 목록과 소유권·대여 효과 계약을 정의한다.
//     Define the closed set of internal native operations and their ownership and loan effects.
//
// ========================

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace aevix
{
    // 이 ID는 사용자 소스의 이름·경로·주석으로 권한을 부여하지 않는다.
    // These IDs do not grant authority through user-source names, paths, or annotations.
    enum class NativeOperationId : std::uint8_t { Release = 1, ReadBytes = 2, WriteBytes = 3 };
    enum class NativeHandleMode : std::uint8_t { Consume, ExclusiveLoan };
    enum class NativeBufferMode : std::uint8_t { None, ReadOnlyLoan, ExclusiveLoan };
    enum class NativeResultShape : std::uint8_t { Cleanup, ByteProgress };

    struct NativeContract final
    {
        NativeOperationId operation = NativeOperationId::Release;
        NativeHandleMode handle = NativeHandleMode::Consume;
        NativeBufferMode buffer = NativeBufferMode::None;
        NativeResultShape result = NativeResultShape::Cleanup;
        bool synchronous = true;
        bool retainsLoan = false;
        bool returnsLoan = false;
        bool invokesAevix = false;
        bool accessesAevixStorage = false;
        bool usableDuringCleanup = true;
        bool operator==(const NativeContract&) const = default;
    };

    struct NativeContractValidation final
    {
        bool succeeded = false;
        std::string_view message;
    };

    [[nodiscard]] std::optional<NativeContract> nativeContract(NativeOperationId operation) noexcept;
    [[nodiscard]] NativeContractValidation validateNativeContract(const NativeContract& contract) noexcept;
    [[nodiscard]] std::string_view nativeBridgeSymbol(NativeOperationId operation) noexcept;
    [[nodiscard]] std::optional<NativeOperationId> nativeBridgeOperation(std::string_view symbol) noexcept;
}
