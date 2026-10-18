// ========================
//
// Created By : miruku_sora2026
//
// File name : AtomicRuntime.hpp
//
// Description :
//     CPU 인라인 원자 셀의 닫힌 연산과 메모리 순서 ABI를 선언한다.
//     Declare the closed CPU inline atomic-cell operation and memory-order ABI.
//
// ========================

#pragma once

#include <cstdint>

namespace aevix::runtime
{
    enum class AtomicKind : std::uint32_t
    {
        I8, U8, I16, U16, I32, U32, I64, U64, Boolean
    };

    enum class AtomicOrder : std::uint32_t
    {
        Relaxed, Acquire, Release, AcqRel, SeqCst
    };

    enum class AtomicOperation : std::uint32_t
    {
        Load, Store, Swap, CompareExchange, CompareExchangeWeak,
        FetchAdd, FetchSub, FetchAnd, FetchOr, FetchXor, FetchNand,
        FetchMin, FetchMax, CheckedFetchAdd, CheckedFetchSub,
        SaturatingFetchAdd, SaturatingFetchSub, NotifyOne, NotifyAll
    };

    enum class AtomicStatus : std::int32_t
    {
        Ok = 0, InvalidOrder = 1, UnsupportedType = 2,
        Overflow = 3, InvalidStorage = 4, InvalidOperation = 5
    };
}

extern "C"
{
    // 값은 해당 폭의 하위 비트를 사용하며, 결과 출력은 실패 시 0으로 초기화한다.
    // Values use the low bits of their declared width; failed calls zero result outputs.
    std::int32_t aevix_atomic_operation(void* storage, std::uint32_t kind,
        std::uint32_t operation, std::uint64_t operand, std::uint64_t expected,
        std::uint32_t success_order, std::uint32_t failure_order,
        std::uint64_t* observed, std::uint32_t* changed) noexcept;

    std::int32_t aevix_atomic_fence(std::uint32_t order,
        std::uint32_t compiler_only) noexcept;

    // 무한 대기는 atomic wait, 유한 대기는 시계 확인과 yield를 사용한다.
    // Infinite waits use atomic wait; finite waits poll a clock and yield.
    std::int32_t aevix_atomic_wait(void* storage, std::uint32_t kind,
        std::uint64_t expected, std::uint32_t order, std::uint64_t timeout_ns,
        std::uint32_t finite, std::uint64_t* observed, std::uint32_t* changed) noexcept;

    std::uint32_t aevix_atomic_alignment(std::uint32_t kind) noexcept;
    std::uint32_t aevix_atomic_lock_free(std::uint32_t kind) noexcept;
}
