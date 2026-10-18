// ========================
//
// Created By : miruku_sora2026
//
// File name : TaskScopeRuntime.hpp
//
// Description :
//     결과 핸들과 독립적으로 작업을 추적·회수하는 컴파일러 전용 구조적 스코프 계약을 정의한다.
//     Define compiler-only structured scopes that track and drain work independently of result handles.
//
// ========================
#pragma once
#include "aevix/Runtime/TaskRuntime.hpp"
#include <cstdint>

namespace aevix
{
    /* 스코프가 허용한 작업 전체의 최종 집계이며 애플리케이션 Result 오류를 펼치지 않는다.
     * Final accounting of admitted work without flattening application Result errors.
     * @since 0.3 @deprecated_at none @removed_at none */
    struct TaskScopeReport final
    {
        std::uint64_t accepted = 0, observed = 0, discarded = 0;
        std::uint64_t completed = 0, cancelled = 0, failed = 0, aborted = 0;
    };
    /* 살아 있는 스코프의 입장 상태이다. 성공한 close는 토큰을 소비하므로 Drained 상태를 추측해 반환하지 않는다.
     * Admission state of a live scope; successful close consumes its token, so no inferred Drained state is exposed.
     * @since 0.3 @deprecated_at none @removed_at none */
    enum class TaskScopeState : std::uint32_t { Open = 0, Closing = 1 };
    /* 스냅샷은 자식 작업 완료나 스코프 수명에 대한 권한이 아니다.
     * A snapshot grants neither child completion nor scope lifetime rights.
     * @since 0.3 @deprecated_at none @removed_at none */
    struct TaskScopeSnapshot final
    {
        std::uint64_t accepted = 0, live_handles = 0;
        std::uint32_t closing = 0;
    };
}

/*
 * 이 스코프는 네이티브 작업자와 미리 계산된 값의 수명·정리 의무를 함께 추적한다.
 * These scopes track native workers and precomputed values with the same lifetime and cleanup obligations.
 * 스코프·관측 핸들은 서로 다른 세대 토큰이다. 기저 작업 토큰과 주소는 호출자에게 공개하지 않는다.
 * Scope/observation handles use distinct generational tokens; underlying task tokens and addresses stay hidden.
 * 실패한 네이티브 준비는 호출자 소유의 캡처·값을 건드리지 않는다. 성공한 준비는 초기화 전 abort 또는 초기화 후 commit이 필수이다.
 * Failed native preparation leaves caller-owned captures/values untouched; successful preparation requires abort before initialization or commit afterward.
 * close는 새 작업을 닫고 모든 자식을 회수한 뒤 관측 핸들을 무효화하고 미수취 결과를 정리한다.
 * Close stops admission, drains every child, invalidates observation handles, and drops unclaimed results.
 * 회수 실패 시 닫힌 스코프 토큰과 남은 의무를 유지한다. 재시도 성공이나 안전한 중단 전 대여를 끝내면 안 된다.
 * Reap failure retains the closed scope token and remaining obligations; loans cannot end before safe retry or termination.
 * discard는 관측 핸들만 소비하며 기다리지 않는다. 작업·캡처·결과 정리는 여전히 스코프 의무이다.
 * Discard consumes only observation without waiting; work, captures, and unclaimed results remain scope obligations.
 * 전역 잠금 중에는 사용자 정리·OS 대기를 하지 않는다. 자기 작업을 담은 스코프 닫기는 대기 전 거부한다.
 * No global lock covers user cleanup or OS waits; closing a scope containing the calling worker is rejected before waits.
 * source Task 핸들은 스코프를 대여해야 한다. 살아 있는 핸들이 있으면 명시적 소스 close를 거부하는 것은 컴파일러 의무이다.
 * Source Task handles must borrow Scope; rejecting explicit source close while handles survive is a compiler obligation.
 * 취소 소유자·Send·스코프 캡처 대여·JIT 코드 수명은 이 네이티브 계약만으로 구현되지 않는다.
 * Retained cancellation owners, Send, scoped-capture loans, and JIT-code lifetime are not implemented by this native contract alone.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
extern "C"
{
    /* 실패 시 출력은 0이다. 성공한 스코프는 반드시 close 또는 cleanup으로 회수한다.
     * Failed output is zero; every successful scope requires close or cleanup.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_scope_open(std::uint64_t *) noexcept;
    /* 스코프가 소유할 준비 작업과 별도의 관측 핸들을 만든다. 캡처는 아직 초기화되지 않는다.
     * Create scope-owned prepared work and a separate observation handle; captures remain uninitialized.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_scope_prepare(std::uint64_t scope, std::uint64_t capture_size,
                                          std::uint64_t capture_alignment, std::uint64_t result_size,
                                          std::uint64_t result_alignment, aevix::TaskInvoke,
                                          aevix::TaskDrop capture_drop, aevix::TaskDrop result_drop,
                                          std::uint64_t *observation_handle,
                                          void **uninitialized_capture_storage) noexcept;
    /* 스코프 관측 핸들과 별도의 진단 번호를 원자적인 준비에서 함께 반환한다.
     * Return a scope observation handle and independent diagnostic serial from one atomic preparation.
     * 번호는 관측 소비·슬롯 재사용·스코프 종료로 재사용되지 않는다. 출력 저장소는 겹치면 안 된다.
     * Observation consumption, slot reuse and scope closure never recycle serials; output storage must not overlap.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_scope_prepare_identified(std::uint64_t scope, std::uint64_t capture_size,
                                          std::uint64_t capture_alignment, std::uint64_t result_size,
                                          std::uint64_t result_alignment, aevix::TaskInvoke,
                                          aevix::TaskDrop capture_drop, aevix::TaskDrop result_drop,
                                          std::uint64_t *observation_handle,
                                          void **uninitialized_capture_storage, std::uint64_t *diagnostic_serial) noexcept;
    /* 유효한 준비를 초기화 후 할당 없이 게시한다. close가 시작되어도 기존 준비는 확정할 수 있다.
     * Publish initialized valid preparation without allocation, including after close has stopped new admission.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_scoped_task_commit(std::uint64_t) noexcept;
    /* 작업자 없이 스코프가 소유할 값의 미초기화 저장소와 정체성을 한 번에 준비한다.
     * Prepare scope-owned uninitialized value storage and identity without creating a worker.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_scope_prepare_ready(std::uint64_t scope, std::uint64_t result_size,
        std::uint64_t result_alignment, aevix::TaskDrop result_drop, std::uint64_t *observation_handle,
        void **uninitialized_result_storage, std::uint64_t *diagnostic_serial) noexcept;
    /* 유효한 Ready 준비에 값을 이동한 뒤 할당 없이 Completed로 게시한다. 입장 종료는 기존 준비를 취소하지 않는다.
     * Publish an initialized valid Ready preparation as Completed without allocation; admission closure preserves existing preparation.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_scoped_task_ready_commit(std::uint64_t) noexcept;
    /* 초기화 전 준비를 취소·회수하고 관측 핸들을 소비한다.
     * Abort/reap uninitialized preparation and consume its observation handle.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_scoped_task_abort(std::uint64_t) noexcept;
    /* 결과를 한 번 이동한다. 검증·OS 오류는 핸들과 스코프의 추적 의무를 보존한다.
     * Move a result once; validation/OS failures preserve the handle and scope tracking obligation.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_scoped_task_join(std::uint64_t, void *, std::uint64_t) noexcept;
    /* 완료된 언어 작업에서만 결과를 비차단 수취한다. 진행 중이면 NotReady이며 관측을 유지한다.
     * Nonblockingly take only terminal language work; NotReady preserves observation.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_scoped_task_try_take(std::uint64_t, void *, std::uint64_t) noexcept;
    /* 완료를 기다리되 결과 핸들은 소비하지 않는다.
     * Wait for completion without consuming result observation.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_scoped_task_wait(std::uint64_t) noexcept;
    /* 유한 완료 관측이며 만료는 작업·캡처 대여를 끝내지 않는다.
     * Finite completion observation; timeout ends neither work nor capture loans.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_scoped_task_wait_timeout(std::uint64_t, std::uint64_t) noexcept;
    /* 관측만 버린다. 경쟁 중인 소비자가 있으면 NotReady이며 소유권을 소비하지 않는다.
     * Discard observation only; a competing consumer returns NotReady without consuming ownership.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_scoped_task_discard(std::uint64_t) noexcept;
    /* 취소는 요청이며 완료를 보장하지 않는다.
     * Cancellation is a request, not completion.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_scoped_task_request_cancel(std::uint64_t, std::uint32_t *) noexcept;
    /* 비소비 관측이다. 내부 작업 토큰이나 주소를 반환하지 않는다.
     * Non-consuming observation returning neither underlying task tokens nor addresses.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_scoped_task_snapshot(std::uint64_t, aevix::TaskSnapshot *) noexcept;
    /* 버린 핸들의 작업까지 취소를 요청한다. 요청 수는 완료 수가 아니다.
     * Request cancellation including discarded-handle work; request count is not completion count.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_scope_request_cancel(std::uint64_t, std::uint64_t *) noexcept;
    /* 새 입장만 멱등적으로 닫고 반환한다. 기존 준비는 commit/abort할 수 있으며 기다리거나 정리하지 않는다.
     * Idempotently stop new admission without waiting or cleanup; accepted preparations may still commit or abort.
     * 레지스트리 조회 후 O(1) 동기화이며, 이미 닫힌 살아 있는 스코프도 성공한다.
     * O(1) synchronization after registry lookup; an already-closed live scope also succeeds.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_scope_close_admission(std::uint64_t) noexcept;
    /* O(n) 자식 순회와 세대 레지스트리 조회로 Prepared/Queued/Running을 관측한다. 버린 핸들의 미완료 작업도 포함한다.
     * Use O(n) child observations plus generational registry lookups, including unfinished discarded-observation work.
     * 각 자식의 순간 관측을 합치므로 동시 실행 중 일관된 전체 시점이나 완료·대여 종료를 증명하지 않는다.
     * Per-child observations are not one coherent instant during concurrent progress and prove neither completion nor loan release.
     * 완료됐지만 회수·결과 정리가 남은 작업은 0일 수 있다. 실패하면 출력은 0이며 할당·작업 완료 대기·콜백이 없다.
     * Terminal work may still owe reaping/result cleanup despite count zero; failure clears output, with no allocation, task-completion wait, or callback.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_scope_active_count(std::uint64_t, std::uint64_t *) noexcept;
    /* 레지스트리 조회 후 O(1)으로 Open/Closing만 관측한다. 자식 수로 Drained를 추측하지 않는다.
     * Observe only Open/Closing in O(1) after registry lookup; never infer Drained from child counts.
     * 실패하면 출력은 0이다. 닫기에 성공해 소비된 토큰은 InvalidToken이며 상태가 아니다.
     * Failure clears output; a token consumed by successful close returns InvalidToken, not a state.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_scope_status(std::uint64_t, std::uint32_t *) noexcept;
    /* 스코프의 순간 상태만 읽는다.
     * Read only the current scope snapshot.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_scope_snapshot(std::uint64_t, aevix::TaskScopeSnapshot *) noexcept;
    /* 모든 자식을 회수한 경우에만 스코프를 소비하고 최종 집계를 반환한다.
     * Consume the scope and return final accounting only after every child is drained.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_scope_close(std::uint64_t, aevix::TaskScopeReport *) noexcept;
    /* 0은 무동작이다. 일반 소멸 경로도 회수 실패를 무시하면 안 된다.
     * Zero is a no-op; ordinary destruction must not ignore reaping failure.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_scope_cleanup(std::uint64_t) noexcept;
    /* 새 스코프를 닫고 스코프·기저 작업·네이티브 메타데이터 꼬리를 모두 회수한다.
     * Close new scopes and drain scopes, underlying tasks, and native metadata tails.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_scope_runtime_drain() noexcept;
#if defined(AEVIX_TASK_RUNTIME_TESTING)
    /* 전용 테스트에서만 살아 있는 스코프와 관측 핸들을 센다.
     * Count live scopes and observation handles only in dedicated tests.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::uint64_t aevix_task_scope_test_live_scopes() noexcept;
    std::uint64_t aevix_task_scope_test_live_handles() noexcept;
    std::uint32_t aevix_task_scope_test_is_reaping(std::uint64_t) noexcept;
#endif
}
