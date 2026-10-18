// ========================
//
// Created By : miruku_sora2026
//
// File name : SharedRuntime.hpp
//
// Description :
//     비공개 세대 토큰으로 Shared와 Weak의 페이로드 및 참조 수명 계약을 정의한다.
//     Define Shared/Weak payload and reference lifetimes using private generational tokens.
//
// ========================
#pragma once

#include <cstdint>

namespace aevix
{
    /* 원시 상태는 컴파일러 연결층에서만 공개 오류·Optional로 변환한다.
     * Translate raw statuses to public errors/Optional only in the compiler bridge.
     * @since 0.3 @deprecated_at none @removed_at none */
    enum class SharedStatus : std::int32_t
    {
        Success = 0,
        AllocationFailure = 1,
        SizeOverflow = 2,
        InvalidInput = 3,
        InvalidToken = 4,
        InvalidState = 5,
        CountOverflow = 6,
        Expired = 7,
        NotUnique = 8
    };
    /* 컴파일러가 만든 비투척 정리 함수만 보관하며, 마지막 강한 소유자에서 잠금 밖에 호출한다.
     * Retain only a compiler-generated nonthrowing destructor and call it outside locks at last strong release.
     * @since 0.3 @deprecated_at none @removed_at none */
    using SharedDrop = void (*)(void *) noexcept;
}

/*
 * 이 ABI는 신뢰된 컴파일러 연결층 전용이며 소스에서 포인터·토큰 변환을 제공하지 않는다.
 * This ABI is only for the trusted compiler bridge; source code receives no pointer/token conversions.
 * 토큰은 주소가 아니라 슬롯·세대·강한/약한 종류를 담는다. 0은 항상 무효이다.
 * Tokens contain a slot, generation, and strong/weak kind, never an address. Zero is always invalid.
 * prepare 성공 후 초기화 전에만 abort할 수 있다. T를 정렬된 소유 저장소에 이동한 뒤에는 commit해야 한다.
 * Abort is allowed only before initialization after prepare; after moving T into aligned storage, commit is required.
 * commit은 할당하지 않는다. 올바른 준비 토큰의 commit은 실패하지 않는다.
 * Commit does not allocate and cannot fail for a valid prepared token following this protocol.
 * 일반 버퍼 대여를 보관하지 않는다. 유일한 보관 함수는 컴파일러의 정리 thunk이다.
 * No ordinary borrowed buffer is retained. The only retained function is the compiler's destructor thunk.
 * 원시 포인터 검사는 실제 할당 범위·생존·별칭 조건을 증명하지 않는다.
 * Raw-pointer checks do not prove allocation extent, liveness, or aliasing conditions.
 * 참조 수 조작은 동기화되지만 T의 스레드 안전성·소스 대여 수명은 컴파일러가 검사해야 한다.
 * Counts are synchronized, but the compiler must enforce T's thread safety and source borrow lifetimes.
 * 스레드 전송은 T:Send+Sync와 정리 권한을 검증하기 전까지 허용하지 않는다.
 * Thread transfer remains disabled until T:Send+Sync and destructor-thread permissions are verified.
 * 프로그램 종료 전에 호출을 멈추고 모든 소유자를 정리해야 한다. 순환 강한 참조는 자동 수집하지 않는다.
 * Shutdown requires quiescence and released owners; strong reference cycles are not automatically collected.
 * 빈 슬롯 메타데이터는 최대 동시 사용량만큼 재사용하며, 제어 블록은 마지막 약한 소유자 뒤에 해제한다.
 * Empty-slot metadata is reused up to its high-water mark; control storage is freed after the final weak owner.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
extern "C"
{
    /* 실패하면 입력 T를 읽거나 이동하지 않고 출력은 0/null이다. 크기 0과 큰 정렬도 지원한다.
     * Failure never reads or moves input T and clears outputs; zero-sized and over-aligned payloads are supported.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_shared_prepare(std::uint64_t, std::uint64_t, aevix::SharedDrop,
                                      std::uint64_t *, void **) noexcept;
    /* 준비된 저장소에 정확히 하나의 초기화된 T가 있음을 신뢰된 호출자가 확인한다.
     * The trusted caller confirms prepared storage contains exactly one initialized T.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_shared_commit(std::uint64_t) noexcept;
    /* 초기화되지 않은 준비 저장소만 해제하며 정리 thunk는 호출하지 않는다.
     * Release only uninitialized prepared storage, without calling its destructor thunk.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_shared_abort(std::uint64_t) noexcept;
    /* 새 강한 소유자 하나를 만들며 실패 시 참조 수를 유지한다.
     * Create one strong owner, preserving counts on failure.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_shared_clone(std::uint64_t, std::uint64_t *) noexcept;
    /* 강한 소유자 하나를 소비한다. 마지막 소유자는 T를 한 번 정리한다.
     * Consume one strong owner; the final owner destroys T exactly once.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_shared_release(std::uint64_t) noexcept;
    /* 강한 소유권을 유지하면서 새 약한 소유자 하나를 만든다.
     * Create one weak owner while preserving strong ownership.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_shared_downgrade(std::uint64_t, std::uint64_t *) noexcept;
    /* 새 약한 소유자 하나를 만들며 만료된 페이로드도 허용한다.
     * Create one weak owner, including after payload expiration.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_weak_clone(std::uint64_t, std::uint64_t *) noexcept;
    /* 약한 소유자 하나를 소비하며 마지막 메타데이터 소유자는 제어 블록을 해제한다.
     * Consume one weak owner; the final metadata owner releases control storage.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_weak_release(std::uint64_t) noexcept;
    /* 강한 수가 0이면 Expired이며, 0에서 다시 강한 소유권을 만들지 않는다.
     * Return Expired at zero strong count; never resurrect strong ownership from zero.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_weak_upgrade(std::uint64_t, std::uint64_t *) noexcept;
    /* 강한 소유자가 보장하는 저장소만 빌린다. 실제 대여 수명은 컴파일러가 제한한다.
     * Borrow storage protected by a strong owner; the compiler limits the actual loan lifetime.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_shared_payload(std::uint64_t, void **) noexcept;
    /* 강한 수 1, 명시적 약한 수 0일 때만 배타적 저장소를 빌린다.
     * Borrow exclusive storage only with one strong and zero explicit weak owners.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_shared_unique_payload(std::uint64_t, void **) noexcept;
    /* 강한 수 1일 때 정확한 타입 크기의 목적지로 T를 이동하며 Drop을 호출하지 않는다.
     * At one strong owner, relocate T into an exact-size destination without invoking Drop.
     * 목적지 별칭·범위·초기화 상태와 실패 시 소유자 보존은 컴파일러가 보장해야 한다.
     * The compiler must guarantee destination aliasing/extent/uninitialized state and preserve the owner on failure.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_shared_take(std::uint64_t, void *, std::uint64_t) noexcept;
    /* 명시적 참조 수의 순간값이며 약한 수에 내부 정리 보호 수는 포함하지 않는다.
     * Snapshot explicit counts, excluding the internal destruction-lifetime guard from weak count.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_shared_counts(std::uint64_t, std::uint64_t *, std::uint64_t *) noexcept;
    /* 두 유효한 소유자가 같은 제어 블록인지 확인한다. 페이로드를 읽지 않는다.
     * Compare valid owners' control identity without reading payload storage.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_shared_same_allocation(std::uint64_t, std::uint64_t, std::uint32_t *) noexcept;
#if defined(AEVIX_SHARED_RUNTIME_TESTING)
    /* 전용 테스트에서만 할당 실패와 참조 수 오버플로를 주입한다.
     * Inject allocation failure and count overflow in dedicated tests only.
     * @since 0.3 @deprecated_at none @removed_at none */
    void aevix_shared_test_fail_after(std::uint64_t) noexcept;
    std::uint64_t aevix_shared_test_live_controls() noexcept;
    std::int32_t aevix_shared_test_set_counts(std::uint64_t, std::uint64_t, std::uint64_t) noexcept;
#endif
}
