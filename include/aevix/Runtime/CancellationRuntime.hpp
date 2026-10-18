// ========================
//
// Created By : miruku_sora2026
//
// File name : CancellationRuntime.hpp
//
// Description :
//     작업 수명과 독립적인 취소 요청 소유자와 읽기 전용 관측자의 닫힌 네이티브 계약을 정의한다.
//     Define closed native request owners and readonly cancellation observers independent of task lifetime.
//
// ========================
#pragma once
#include <cstdint>

/*
 * Source는 요청 권한, Token은 읽기 전용 관측 권한이다. 복제는 해당 역할만 유지한다.
 * Source owns request authority; Token owns readonly observation. Cloning preserves only the same role.
 * 각각 별도 세대 토큰으로 작은 취소 상태를 참조하며 Task Control·캡처·결과를 보존하지 않는다.
 * Separate generational owners retain small cancellation state, never Task controls, captures, or results.
 * new·clone·관측자 생성은 할당 실패1 또는 참조 수·메타데이터 범위 초과2를 명시적으로 반환한다.
 * New, clone, and token derivation explicitly return allocation failure1 or owner/metadata overflow2.
 * 이 개발 단계의 실패 가능한 ABI는 설계 카탈로그의 비실패 clone 시그니처를 구현했다고 주장하지 않는다.
 * This fallible development ABI does not claim implementation of the catalog's nonfallible clone signatures.
 * 0은 성공,3은 잘못된 입력,4는 잘못된 역할·소비된 토큰이다. 실패 시 새 출력은0이고 원래 소유자는 유지된다.
 * Status0=success,3=invalid input,4=wrong-role/stale owner. Failure clears new output and preserves original owners.
 * 요청은 완료·강제 종료·메모리 안전·애플리케이션 롤백을 보장하지 않는다.
 * A request guarantees neither completion, forced termination, memory safety, nor application rollback.
 * 작업과 스코프를 회수한 뒤에도 관측자는 유효하며 전역 작업 drain은 독립적인 관측 소유자를 기다리지 않는다.
 * Observers remain valid after Task/Scope cleanup; global task drain does not wait for independent notification owners.
 * 계층 전파·대기·깨우기·async와 소스 Send/Sync 바인딩은 이 계약에 포함되지 않는다.
 * Hierarchical propagation, waits, notifications, async, and source Send/Sync bindings are outside this contract.
 * ABI 출력은 실제 쓰기 가능한 저장소여야 하며 포인터 값 검사만으로 할당·생존을 증명하지 않는다.
 * ABI outputs require real writable storage; pointer-value checks alone cannot prove allocation or lifetime.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
extern "C"
{
    /* 독립적인 요청 상태와 Source 소유자를 만든다.
     * Create independent request state and its Source owner.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_cancel_source_new(std::uint64_t *) noexcept;
    /* 요청 권한을 명시적으로 복제하며 실패해도 입력 Source는 유지된다.
     * Explicitly clone request authority, preserving the input Source on failure.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_cancel_source_clone(std::uint64_t source, std::uint64_t *) noexcept;
    /* Source에서 읽기 전용 소유 관측자를 만든다. 반대 방향 권한 변환은 없다.
     * Derive an owned readonly observer from Source; authority cannot be converted in reverse.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_cancel_token_from_source(std::uint64_t source, std::uint64_t *) noexcept;
    /* 읽기 전용 관측 소유권만 복제한다.
     * Clone readonly observation ownership only.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_cancel_token_clone(std::uint64_t token, std::uint64_t *) noexcept;
    /* already_requested는0=새 요청,1=기존 요청이다. 요청은 작업 완료를 뜻하지 않는다.
     * already_requested is0=new request,1=already requested; a request is not task completion.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_cancel_source_request(std::uint64_t source, std::uint32_t *already_requested) noexcept;
    /* Token 역할만 요청 플래그의 순간값을 읽는다. Source 권한은 반환하지 않는다.
     * Only the Token role reads a request snapshot; no Source authority is returned.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_cancel_token_requested(std::uint64_t token, std::uint32_t *) noexcept;
    /* Source 소유자를 한 번 소비한다. 0은 무동작이며 마지막 소유자에서 작은 상태를 해제한다.
     * Consume one Source; zero is a no-op and the final owner frees the small state.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_cancel_source_release(std::uint64_t source) noexcept;
    /* 관측 소유자를 한 번 소비하며 다른 관측자·작업의 상태는 보존한다.
     * Consume one observer while preserving state owned by other observers/tasks.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_cancel_token_release(std::uint64_t token) noexcept;
    /* 작업 상태를 pin한 동안 읽기 전용 소유자를 생성한다. TaskContext 포인터를 반환하지 않는다.
     * Derive a readonly owner while pinning task state, without returning a TaskContext pointer.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_task_cancel_token(std::uint64_t task, std::uint64_t *) noexcept;
    /* 스코프뿐 아니라 기저 작업을 pin하여 관측 소유권을 생성한다.
     * Pin the underlying task, not only Scope, while deriving observation ownership.
     * @since 0.3 @deprecated_at none @removed_at none */
    std::int32_t aevix_scoped_task_cancel_token(std::uint64_t task, std::uint64_t *) noexcept;
#if defined(AEVIX_TASK_RUNTIME_TESTING)
    /* 전용 테스트에서만 소유 수 상한과 실제 생존 상태를 검사한다.
     * Inspect ownership limits and live state only in dedicated tests.
     * @since 0.3 @deprecated_at none @removed_at none */
    void aevix_cancel_test_owner_limit(std::uint64_t) noexcept;
    std::uint64_t aevix_cancel_test_live_states() noexcept;
    std::uint64_t aevix_cancel_test_owner_count(std::uint64_t) noexcept;
#endif
}
