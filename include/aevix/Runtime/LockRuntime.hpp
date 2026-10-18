// ========================
//
// Created By : miruku_sora2026
//
// File name : LockRuntime.hpp
//
// Description :
//     힙 할당 없는 인라인 Mutex·RwLock 상태 및 컴파일러 전용 보호자 계약을 정의한다.
//     Define allocation-free inline Mutex/RwLock states and compiler-only guard contracts.
//
// ========================
#pragma once

#include <cstdint>

namespace aevix
{
    /* 상태는 소스 Mutex<T>/RwLock<T> 안에 포함되며 잠금이 없을 때 이동할 수 있다.
     * State lives inside source Mutex<T>/RwLock<T> and can relocate only without active lock borrowers.
     * @since 0.3 @deprecated_at none @removed_at none */
    struct alignas(8) InlineMutexState final { std::uint64_t bits = 0; };
    /* 63번 비트는 쓰기, 62번은 권고성 오염, 하위 62개는 읽기 보호자 수이다.
     * bit63=writer, bit62=advisory poison, low62=reader count.
     * @since 0.3 @deprecated_at none @removed_at none */
    struct alignas(8) InlineRwLockState final { std::uint64_t bits = 0; };
    /* Busy와 TimedOut은 보호자를 얻지 않은 결과이며 ReaderOverflow는 수를 바꾸지 않는다.
     * Busy/TimedOut do not acquire a guard; ReaderOverflow preserves the reader count.
     * @since 0.3 @deprecated_at none @removed_at none */
    enum class LockStatus : std::int32_t
    {
        Success = 0,
        Busy = 1,
        TimedOut = 2,
        InvalidInput = 3,
        InvalidState = 4,
        ReaderOverflow = 5
    };
    static_assert(sizeof(InlineMutexState) == 8 && alignof(InlineMutexState) == 8);
    static_assert(sizeof(InlineRwLockState) == 8 && alignof(InlineRwLockState) == 8);
}

/*
 * 이 ABI는 컴파일러 전용이며 상태 주소·원시 비트를 소스에 공개하지 않는다.
 * This ABI is compiler-only: state addresses and raw bits are not source-visible.
 * 상태는 크기 8·정렬 8인 실제 uint64 저장소여야 한다. 포인터 검사는 할당 범위를 증명하지 않는다.
 * State requires actual uint64 storage of size/alignment 8; pointer checks do not prove allocation extent.
 * 기다리는 호출과 모든 보호자는 소유 객체를 대여한다. 그동안 이동·재초기화·해제하면 안 된다.
 * Waiting calls and all guards borrow the owner; moving, reinitializing, or destroying it then is forbidden.
 * 보호자는 해제 스레드 제약 때문에 Send가 아니며, 해제 권한·상태 수명은 컴파일러가 검사한다.
 * Guards are non-Send because of unlock-thread restrictions; the compiler checks unlock permission and state lifetime.
 * 비재귀 잠금이며 공정성·기아 방지·동일 스레드 재획득 진단을 약속하지 않는다.
 * Locks are nonrecursive and promise no fairness, starvation prevention, or same-thread reacquisition diagnostic.
 * 실패한 획득은 yield로 재시도한다. 잠금마다 동적 메모리·OS 잠금 객체·레지스트리를 만들지 않는다.
 * Failed acquisition retries yield; no per-lock heap memory, OS lock object, or registry is created.
 * acquire/release는 T의 게시를 동기화한다. 권고성 오염은 T의 유효성이나 롤백을 보장하지 않는다.
 * Acquire/release synchronize T publication. Advisory poison proves neither T validity nor rollback.
 * Success는 poisoned=1일 때도 보호자를 획득했다. 소스 오류 값은 그 보호자를 보존해야 한다.
 * Success acquires a guard even with poisoned=1; source poison errors must retain that acquired guard.
 * 출력 포인터는 상태와 겹치지 않아야 한다. 런타임은 사용자 코드·정리 thunk를 호출하지 않는다.
 * Outputs must not overlap state. The runtime invokes no user code or destructor thunk.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
extern "C"
{
    /* 유효하고 대여되지 않은 저장소의 초기화는 실패하거나 할당하지 않는다.
     * Initialization of valid unborrowed storage cannot fail or allocate.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_mutex_init(void *) noexcept;
    /* 성공할 때까지 기다리며 오염 여부와 함께 배타적 보호자를 얻는다.
     * Wait for exclusive acquisition and return its advisory poison flag.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_mutex_lock(void *, std::uint32_t *) noexcept;
    /* 한 번의 원자적 획득만 시도한다. Busy에는 보호자가 없다.
     * Attempt one atomic acquisition; Busy owns no guard.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_mutex_try_lock(void *, std::uint32_t *) noexcept;
    /* 최대 INT64_MAX 나노초이며 0은 한 번 시도한 뒤 즉시 만료한다.
     * Accept up to INT64_MAX nanoseconds; zero tries once before immediate timeout.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_mutex_lock_timeout(void *, std::uint64_t, std::uint32_t *) noexcept;
    /* mark_poison은 0/1이다. 유효한 배타적 보호자만 같은 스레드에서 해제한다.
     * mark_poison is 0/1; only a valid exclusive guard may unlock on its owning thread.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_mutex_unlock(void *, std::uint32_t) noexcept;
    /* 데이터 대여를 얻지 않는 권고성 오염 상태의 순간값이다.
     * Snapshot advisory poison without obtaining a data loan.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_mutex_is_poisoned(void *, std::uint32_t *) noexcept;
    /* 현재 배타적 보호자의 소유자만 명시적인 복구 후 호출할 수 있다.
     * Only the current exclusive guard owner may call this after explicit recovery.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_mutex_clear_poison(void *) noexcept;
    /* 유효하고 대여되지 않은 읽기/쓰기 상태를 할당 없이 초기화한다.
     * Initialize valid unborrowed reader/writer state without allocation.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_rwlock_init(void *) noexcept;
    /* 읽기 보호자 획득을 기다린다. 읽기 수 포화는 오류이다.
     * Wait to acquire a read guard; saturated reader count is an error.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_rwlock_read(void *, std::uint32_t *) noexcept;
    /* 쓰기 보호자 획득을 기다린다. 읽기에서 쓰기로 자동 승격하지 않는다.
     * Wait to acquire a write guard, without implicit read-to-write upgrade.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_rwlock_write(void *, std::uint32_t *) noexcept;
    /* 한 번의 공유 읽기 획득을 시도한다.
     * Attempt one shared read acquisition.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_rwlock_try_read(void *, std::uint32_t *) noexcept;
    /* 한 번의 배타적 쓰기 획득을 시도한다.
     * Attempt one exclusive write acquisition.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_rwlock_try_write(void *, std::uint32_t *) noexcept;
    /* 읽기 대기 요청은 최대 INT64_MAX 나노초이며 실제 재개 시각은 스케줄러에 따른다.
     * Accept requested read timeouts up to INT64_MAX nanoseconds; actual resumption depends on scheduling.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_rwlock_read_timeout(void *, std::uint64_t, std::uint32_t *) noexcept;
    /* 쓰기 대기 요청은 최대 INT64_MAX 나노초이며 실제 재개 시각은 스케줄러에 따른다.
     * Accept requested write timeouts up to INT64_MAX nanoseconds; actual resumption depends on scheduling.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_rwlock_write_timeout(void *, std::uint64_t, std::uint32_t *) noexcept;
    /* 읽기 보호자 하나만 해제하며 다른 읽기 수·오염 비트를 보존한다.
     * Release one read guard, preserving other readers and the poison bit.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_rwlock_read_unlock(void *) noexcept;
    /* 쓰기 보호자를 해제하며 mark_poison 0/1을 적용한다.
     * Release a write guard with mark_poison 0/1.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_rwlock_write_unlock(void *, std::uint32_t) noexcept;
    /* 데이터 대여 없이 오염 상태를 관측한다.
     * Observe advisory poison without a data loan.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_rwlock_is_poisoned(void *, std::uint32_t *) noexcept;
    /* 현재 쓰기 보호자의 소유자만 명시적으로 오염을 지울 수 있다.
     * Only the current write guard owner may explicitly clear poison.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_rwlock_clear_poison(void *) noexcept;
#if defined(AEVIX_LOCK_RUNTIME_TESTING)
    /* 대여자가 없는 전용 테스트 저장소에만 원시 상태를 주입한다.
     * Inject raw state only into quiescent dedicated test storage.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_lock_test_set_state(void *, std::uint64_t) noexcept;
#endif
}
