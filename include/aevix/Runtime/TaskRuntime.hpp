// ========================
//
// Created By : miruku_sora2026
//
// File name : TaskRuntime.hpp
//
// Description :
//     준비·확정·취소와 정확한 캡처·결과 정리를 위한 컴파일러 전용 네이티브 작업 계약을 정의한다.
//     Define compiler-only native task preparation, commit, cancellation, and exact capture/result cleanup.
//
// ========================
#pragma once

#include <cstdint>

namespace aevix
{
    /* 네이티브 작업 상태이며 애플리케이션 Result<T,E>와 합치지 않는다.
     * Native task status, separate from application Result<T,E>.
     * @since 0.3 @deprecated_at none @removed_at none */
    enum class TaskRuntimeStatus : std::int32_t
    {
        Success = 0,
        AllocationFailure = 1,
        SizeOverflow = 2,
        InvalidInput = 3,
        InvalidToken = 4,
        InvalidState = 5,
        ThreadFailure = 6,
        WouldDeadlock = 7,
        NotReady = 8,
        Cancelled = 9,
        Failed = 10,
        TimedOut = 11
    };
    /* 완료 단계는 호출 thunk와 캡처 정리가 끝났다는 뜻이며 OS 스레드 회수는 별도 join 경계이다.
     * Terminal phase means the invoke thunk and capture cleanup finished; OS reaping is a separate join boundary.
     * @since 0.3 @deprecated_at none @removed_at none */
    enum class TaskPhase : std::uint32_t
    {
        Prepared = 0,
        Queued = 1,
        Running = 2,
        Completed = 3,
        Cancelled = 4,
        Failed = 5,
        Aborted = 6
    };
    /* Completed에서만 결과 슬롯이 초기화되어 있다. 모든 반환 경로는 캡처를 정확히 한 번 소비·정리한다.
     * Only Completed initializes the result slot; every return path consumes/cleans captures exactly once.
     * @since 0.3 @deprecated_at none @removed_at none */
    enum class TaskInvokeOutcome : std::int32_t
    {
        Completed = 0,
        Cancelled = 1,
        Failed = 2
    };
    struct TaskContext;
    using TaskInvoke = std::int32_t (*)(void *, void *, const TaskContext *) noexcept;
    using TaskDrop = void (*)(void *) noexcept;
    /* 관측값은 수명이나 결과 소유권을 취득하지 않는 순간값이다.
     * Observation is a snapshot, not ownership of result storage or a lifetime guarantee.
     * @since 0.3 @deprecated_at none @removed_at none */
    struct TaskSnapshot final
    {
        std::uint32_t phase = 0;
        std::uint32_t cancelRequested = 0;
        std::int32_t invokeOutcome = 0;
    };
}

/*
 * 이 비스코프 ABI는 작업당 네이티브 작업자 하나를 사용한다. 구조적 스코프는 TaskScopeRuntime.hpp의 별도 계약이다.
 * These unscoped entry points use one native worker per task; TaskScopeRuntime.hpp defines the separate structured-scope contract.
 * 토큰은 주소가 아니라 비공개 슬롯·세대이며, 원시 저장소는 컴파일러만 다룬다.
 * Tokens are private slot/generation identities, never addresses; only the compiler handles raw storage.
 * prepare가 실패하면 호출자의 원래 클로저를 읽거나 이동하지 않는다.
 * Failed prepare never reads or moves the caller's original closure.
 * prepare 후 초기화 전에는 abort할 수 있다. 캡처를 이동한 뒤에는 할당 없는 commit이 필수이다.
 * Abort is valid before initialization after prepare; after moving captures, allocation-free commit is required.
 * 실행된 invoke thunk가 캡처를 소비한다. 실행 전 취소만 런타임의 capture_drop이 처리한다.
 * An invoked thunk consumes captures; only cancellation before invocation uses runtime capture_drop.
 * Completed 반환만 결과 초기화를 허용하며, 다른 반환은 결과 슬롯을 초기화하지 않는다.
 * Only a Completed return permits an initialized result slot; other returns leave result storage uninitialized.
 * join과 cleanup은 작업자가 끝나고 정리될 때까지 기다린다. 실패 시 무효화되지 않은 토큰은 호출자가 계속 소유한다.
 * Join/cleanup wait and reap the worker; on a non-consuming failure the caller retains the token.
 * try_take는 언어 작업 완료 뒤에만 OS 스레드의 자동 회수를 설정한다. 이것은 소스 작업을 분리하는 기능이 아니다.
 * try_take enables OS auto-reaping only after language work completes; it does not detach source work.
 * 작업자 수명 보호자는 메타데이터만 사용하는 마지막 코드까지 유지되며 drain은 이 꼬리까지 기다린다.
 * A worker lifetime pin covers its metadata-only tail, and drain waits for that tail too.
 * 전역 레지스트리 잠금 중에는 사용자 코드·정리 thunk·OS 대기를 수행하지 않으며, 정리 thunk는 소비 잠금 밖에서도 호출한다.
 * No global registry lock is held across user code, destructor thunks, or OS waits; destructor thunks also run outside consume locks.
 * 취소는 요청일 뿐 실행 중인 스레드를 강제 종료하거나 캡처 대여를 즉시 끝내지 않는다.
 * Cancellation is only a request; it neither kills running threads nor immediately ends capture loans.
 * Send·정적 캡처·스코프 대여·non-Send 보호자·실행 코드 수명은 별도의 컴파일러 의무이다.
 * Send, static captures, scope loans, non-Send guards, and executable-code lifetime remain compiler obligations.
 * 원시 포인터 검사는 실제 할당 범위·초기화·별칭·생존을 증명하지 않는다.
 * Raw-pointer checks do not prove allocation extent, initialization, aliasing, or liveness.
 * 종료 전에 모든 작업을 회수해야 한다. 강제 중단·네이티브 예외·unsafe 콜백 위반을 복구하는 기능은 없다.
 * Shutdown must reap all tasks; forced termination, native exceptions, and unsafe thunk violations are not recoverable here.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
extern "C"
{
    /* 실패한 출력은 0/null이다. 크기 0과 큰 정렬을 지원하고 초기 작업자는 확정 전 실행하지 않는다.
     * Failed outputs are 0/null; zero sizes and over-alignment are supported, and a gated worker cannot invoke before commit.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_prepare(std::uint64_t capture_size, std::uint64_t capture_alignment,
                                    std::uint64_t result_size, std::uint64_t result_alignment, aevix::TaskInvoke invoke,
                                    aevix::TaskDrop capture_drop, aevix::TaskDrop result_drop,
                                    std::uint64_t *owning_token, void **uninitialized_capture_storage) noexcept;
    /* 비공개 토큰과 독립적인 진단 번호를 같은 준비 트랜잭션에서 반환한다. 출력 저장소는 겹치면 안 된다.
     * Return a token-independent diagnostic serial in the same prepare transaction; output storage must not overlap.
     * 번호는 재사용하지 않으며 소진 시 SizeOverflow를 반환하고 모든 실패 출력은 0/null이다.
     * Serials are never reused; exhaustion returns SizeOverflow and all failed outputs are zero/null.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_prepare_identified(std::uint64_t capture_size, std::uint64_t capture_alignment,
                                    std::uint64_t result_size, std::uint64_t result_alignment, aevix::TaskInvoke invoke,
                                    aevix::TaskDrop capture_drop, aevix::TaskDrop result_drop,
                                    std::uint64_t *owning_token, void **uninitialized_capture_storage,
                                    std::uint64_t *diagnostic_serial) noexcept;
    /* 초기화된 캡처 하나를 작업자에게 게시한다. 유효한 준비 토큰에서는 할당하거나 실패하지 않는다.
     * Publish one initialized capture to the worker; a valid prepared token commits without allocation or failure.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_commit(std::uint64_t) noexcept;
    /* 초기화 전 준비만 취소·회수하며 캡처 정리는 호출하지 않는다.
     * Abort/reap only uninitialized preparation without invoking capture cleanup.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_abort(std::uint64_t) noexcept;
    /* 성공 결과는 정확한 크기·정렬의 미초기화 목적지로 이동한 뒤 토큰을 소비한다.
     * Move a successful result to exact-size/aligned uninitialized output before consuming the token.
     * 완료된 취소·실패도 정리가 끝난 뒤 토큰을 소비한다. 검증·OS 회수 오류는 토큰을 보존한다.
     * Terminal cancellation/failure consumes only after cleanup; validation/OS-reap errors preserve the token.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_join(std::uint64_t, void *, std::uint64_t) noexcept;
    /* 완료 전에는 NotReady이다. 완료 후 자동 OS 회수를 설정한 뒤 결과·토큰을 한 번 소비하며 기다리지 않는다.
     * Return NotReady before completion; after completion set up OS auto-reaping and consume result/token without waiting for worker exit.
     * 회수 설정 실패는 소유권을 보존하며, 언어 작업은 이미 모든 캡처 정리를 마쳐야 한다.
     * Failed reap setup preserves ownership; all language capture cleanup must already be complete.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_try_take(std::uint64_t, void *, std::uint64_t) noexcept;
    /* 토큰을 유지하며 OS 작업자의 완료와 회수를 기다린다.
     * Wait and reap the OS worker while preserving the owning token.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_wait(std::uint64_t) noexcept;
    /* thunk 완료만 관측하며 결과·토큰을 소비하지 않는다. 만료 시 작업은 계속 추적된다.
     * Observe thunk completion without consuming result/token; timeout leaves work tracked.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_wait_timeout(std::uint64_t, std::uint64_t) noexcept;
    /* 작업을 기다려 회수하고 아직 가져가지 않은 결과를 한 번 정리한 뒤 토큰을 소비한다.
     * Wait/reap, destroy an unclaimed result once, and consume the token.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_cleanup(std::uint64_t) noexcept;
    /* 새 준비를 닫고 남은 작업과 메타데이터 꼬리를 회수한다. 준비 트랜잭션은 소유자가 확정·취소해야 한다.
     * Close new preparation and drain work/metadata tails; owners must commit/abort outstanding preparation transactions.
     * 외부 네이티브 API 호출의 정지는 별도 요구이며 협조하지 않는 작업은 종료를 지연할 수 있다.
     * External native API quiescence remains required; noncooperative work can delay shutdown indefinitely.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_runtime_drain() noexcept;
    /* disposition은0=새 요청,1=이미 완료,2=이미 요청이다. 결과 값은 취소 성공을 보장하지 않는다.
     * disposition is 0=new request, 1=already terminal, 2=already requested; none proves cancellation completion.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_request_cancel(std::uint64_t, std::uint32_t *) noexcept;
    /* 소비나 대기 없이 단계·취소 상태를 읽는다.
     * Read phase/cancellation status without consuming or waiting.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_snapshot(std::uint64_t, aevix::TaskSnapshot *) noexcept;
    /* invoke 호출 중에만 유효한 불투명 취소 문맥이다. 문맥 포인터를 보관하면 안 된다.
     * An opaque cancellation context valid only during invocation; its pointer must not be retained.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::uint32_t aevix_task_context_cancel_requested(const aevix::TaskContext *) noexcept;
    /* 오류 조회는 메타데이터를 지우지 않는다. domain0=없음,1=errno/CRT,2=Win32이다.
     * Error queries preserve metadata; domain 0=none, 1=errno/CRT, 2=Win32.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_last_error() noexcept;
    std::int32_t aevix_task_error_domain() noexcept;
#if defined(AEVIX_TASK_RUNTIME_TESTING)
    /* 전용 테스트에서만 할당·네이티브 스레드 생성 실패를 주입한다.
     * Inject allocation/native-thread-creation failure only in dedicated tests.
     * @since 0.3 @deprecated_at none @removed_at none */
    void aevix_task_test_fail_after(std::uint64_t) noexcept;
    void aevix_task_test_fail_threads(std::uint32_t) noexcept;
    void aevix_task_test_fail_reap(std::uint32_t) noexcept;
    void aevix_task_test_hold_worker_tail(std::uint32_t) noexcept;
    std::uint32_t aevix_task_test_is_reaping(std::uint64_t) noexcept;
    std::uint64_t aevix_task_test_live_controls() noexcept;
    std::uint64_t aevix_task_test_thread_creations() noexcept;
    std::uint64_t aevix_task_test_native_reaps() noexcept;
#endif
}
