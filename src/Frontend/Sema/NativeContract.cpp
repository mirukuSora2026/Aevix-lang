// ========================
//
// Created By : miruku_sora2026
//
// File name : NativeContract.cpp
//
// Description :
//     승인된 내부 네이티브 연산만 허용하고 대여 유출과 숨은 언어 실행 효과를 거부한다.
//     Permit only closed internal native operations and reject escaping loans or hidden language effects.
//
// ========================

#include "aevix/Native/Contract.hpp"

#include <array>

namespace aevix
{
    namespace
    {
        constexpr std::array operations = {NativeOperationId::Release, NativeOperationId::ReadBytes, NativeOperationId::WriteBytes};
    }

    std::optional<NativeContract> nativeContract(NativeOperationId operation) noexcept
    {
        NativeContract result;
        result.operation = operation;
        switch (operation)
        {
            case NativeOperationId::Release: return result;
            case NativeOperationId::ReadBytes:
                result.handle = NativeHandleMode::ExclusiveLoan;
                result.buffer = NativeBufferMode::ExclusiveLoan;
                result.result = NativeResultShape::ByteProgress;
                result.usableDuringCleanup = false;
                return result;
            case NativeOperationId::WriteBytes:
                // 파일 위치 같은 내부 상태를 바꿀 수 있으므로 입력 버퍼만 읽기 대여다.
                // Only the input buffer is read-only; native state such as a file cursor can change.
                result.handle = NativeHandleMode::ExclusiveLoan;
                result.buffer = NativeBufferMode::ReadOnlyLoan;
                result.result = NativeResultShape::ByteProgress;
                result.usableDuringCleanup = false;
                return result;
        }
        return std::nullopt;
    }

    NativeContractValidation validateNativeContract(const NativeContract& contract) noexcept
    {
        const auto expected = nativeContract(contract.operation);
        if (!expected) return {false, "Unknown native operation ID."};
        if (!contract.synchronous || contract.retainsLoan || contract.returnsLoan)
            return {false, "Native byte loans must be synchronous, non-retaining, and non-returning."};
        if (contract.invokesAevix || contract.accessesAevixStorage)
            return {false, "Native operations cannot invoke Aevix or access its globals and persist storage."};
        if (contract != *expected)
            return {false, "Native ownership, buffer, result, and cleanup effects must match the closed operation contract."};
        return {true, {}};
    }

    std::string_view nativeBridgeSymbol(NativeOperationId operation) noexcept
    {
        switch (operation)
        {
            case NativeOperationId::Release: return "aevix_native_release";
            case NativeOperationId::ReadBytes: return "aevix_native_read_bytes";
            case NativeOperationId::WriteBytes: return "aevix_native_write_bytes";
        }
        return {};
    }

    std::optional<NativeOperationId> nativeBridgeOperation(std::string_view symbol) noexcept
    {
        for (const auto operation : operations)
            if (symbol == nativeBridgeSymbol(operation)) return operation;
        return std::nullopt;
    }
}
