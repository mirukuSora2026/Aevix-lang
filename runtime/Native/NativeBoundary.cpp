// ========================
//
// Created By : miruku_sora2026
//
// File name : NativeBoundary.cpp
//
// Description :
//     네이티브 버퍼의 표현 범위와 동기식 어댑터의 완료 바이트 수를 검사한다.
//     Check native buffer representability and synchronous adapter byte-progress reports.
//
// ========================

#include "aevix/Runtime/NativeBoundary.hpp"

#include <limits>

namespace aevix
{
    namespace
    {
        NativeBufferStatus validateRaw(const void* data, std::uint64_t length) noexcept
        {
            if (length == 0) return NativeBufferStatus::Success;
            if (!data) return NativeBufferStatus::NullWithNonzeroLength;
            if (length > std::numeric_limits<std::size_t>::max() ||
                length > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) ||
                length > std::numeric_limits<std::uintptr_t>::max())
                return NativeBufferStatus::LengthOutOfRange;

            const auto address = reinterpret_cast<std::uintptr_t>(data);
            // 시작과 끝 주소의 정수 표현만 확인하며, 할당 객체의 경계는 알 수 없다.
            // Check integer representations of start and end only; allocation bounds remain unknown.
            if (address > std::numeric_limits<std::uintptr_t>::max() - length)
                return NativeBufferStatus::AddressOverflow;
            return NativeBufferStatus::Success;
        }
    }

    NativeBufferResult<NativeInputBytes>
    NativeInputBytes::checkedRaw(const void* data, std::uint64_t length) noexcept
    {
        const auto status = validateRaw(data, length);
        if (status != NativeBufferStatus::Success || length == 0) return {status, {}};
        return {status, NativeInputBytes(std::span(static_cast<const std::byte*>(data),
                                                  static_cast<std::size_t>(length)))};
    }

    NativeBufferResult<NativeOutputBytes>
    NativeOutputBytes::checkedRaw(void* data, std::uint64_t length) noexcept
    {
        const auto status = validateRaw(data, length);
        if (status != NativeBufferStatus::Success || length == 0) return {status, {}};
        return {status, NativeOutputBytes(std::span(static_cast<std::byte*>(data),
                                                   static_cast<std::size_t>(length)))};
    }

    NativeIoOutcome native_detail::validateOutcome(NativeAdapterOutcome outcome,
                                                   std::size_t requestedBytes) noexcept
    {
        if (outcome.completedBytes > requestedBytes ||
            (outcome.status != NativeAdapterStatus::Success &&
             outcome.status != NativeAdapterStatus::PlatformError) ||
            (outcome.status == NativeAdapterStatus::Success &&
             outcome.platformError != NativePlatformError{}))
        {
            // 잘못된 보고에서 진행량을 추측하지 않는다. 이미 일어난 OS 부작용은 취소할 수 없다.
            // Do not infer progress from an invalid report; existing OS side effects cannot be undone.
            return {NativeIoStatus::AdapterContractViolation, 0, {}};
        }
        return {outcome.status == NativeAdapterStatus::Success ? NativeIoStatus::Success
                                                               : NativeIoStatus::PlatformError,
                outcome.completedBytes, outcome.platformError};
    }
}
