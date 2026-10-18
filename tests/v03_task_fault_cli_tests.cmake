# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_task_fault_cli_tests.cmake
#
# Description :
#     실제 소스 작업의 네이티브 실패 경로에서 소유권·재시도·정확한 정리를 검사한다.
#     Check ownership, retry, and exact cleanup through native faults in actual source Tasks.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED CLANG OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC, CLANG and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
get_filename_component(repo "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(WRITE "${TEST_DIR}/fault.avx" [=[
import task { Task, TaskScope, Executor, ScopeOptions, ScopeReport, ScopeError, SpawnError, TaskError, TaskStatus, TaskPhase, WaitStatus, Cancelled, CancellationSource, CancellationToken, ScopeStatus };
import memory { Shared };
import atomic { Atomic, MemoryOrder };
import time { Duration };
extern "C" i32 task_fault_stage();
extern "C" void task_fault_arm(i32 stage);
extern "C" void task_fault_reset();
T success<T,E>(Result<T,E> input) {
    switch (move input) {
        case Result<T,E>.Ok { T value = value }: return value; break;
        case Result<T,E>.Err: Never.unreachable(); break;
    }
}
struct Capture implements Drop {
    public Shared<Atomic<i32>> counter;
    public i32 amount;
    public void drop(&mut Self self) {
        success(Atomic.fetch_add(Shared.as_ref(&self.counter), 1, MemoryOrder.SeqCst));
        return;
    }
};
Capture capture(&Shared<Atomic<i32>> counter) { return Capture { counter = success(Shared.clone(counter)), amount = 42 }; }
bool spawn_failure<T>(Result<T,SpawnError> outcome, bool thread) {
    switch (move outcome) {
        case Result<T,SpawnError>.Ok: return false; break;
        case Result<T,SpawnError>.Err { SpawnError error = error }:
            switch (error) {
                case SpawnError.AllocationFailure: return !thread; break;
                case SpawnError.ThreadFailure { i32 native = os_code, i32 system = domain }: return thread && native != 0 && (system == 1 || system == 2); break;
                default: return false; break;
            }
            break;
    }
}
bool native_failure<T>(Result<T,TaskError> outcome) {
    switch (move outcome) {
        case Result<T,TaskError>.Ok: return false; break;
        case Result<T,TaskError>.Err { TaskError error = error }:
            switch (error) { case TaskError.Native { i32 native = os_code, i32 system = domain }: return native != 0 && (system == 1 || system == 2); break; default: return false; break; }
            break;
    }
}
i32 main() {
    i32 stage = task_fault_stage();
    Executor executor = Executor.native_threads();
    Shared<Atomic<i32>> counter = success(Shared.new(Atomic.new(0)));
    if (stage == 0) {
        task_fault_arm(stage);
        bool rejected = spawn_failure(Task.open_scope(&executor, ScopeOptions.defaults()), false);
        task_fault_reset();
        return rejected && Shared.strong_count(&counter) == 1usize ? 0 : 1;
    }
    TaskScope scope = success(Task.open_scope(&executor, ScopeOptions.defaults()));
    if (stage == 1 || stage == 2) {
        Capture item = capture(&counter);
        task_fault_arm(stage);
        bool rejected = spawn_failure(Task.spawn_owned(&scope, [item]() -> i32 { return item.amount; }), stage == 2);
        task_fault_reset();
        ScopeReport report = success(Task.close_scope(scope));
        return rejected && report.accepted == 0u64 && success(Atomic.load(Shared.as_ref(&counter), MemoryOrder.SeqCst)) == 1 && Shared.strong_count(&counter) == 1usize ? 0 : 2;
    }
    if (stage == 11) {
        Result<unit,ScopeError> failed = Task.close_admission(&scope);
        switch (move failed) {
            case Result<unit,ScopeError>.Ok: return 27; break;
            case Result<unit,ScopeError>.Err { ScopeError error = error }:
                switch (move error) {
                    case ScopeError.Failure { TaskError reason = reason }: switch (reason) { case TaskError.Overflow: break; default: return 28; break; } break;
                    case ScopeError.Retry: return 29; break;
                }
                break;
        }
        switch (Task.scope_status(&scope)) { case ScopeStatus.Open: break; case ScopeStatus.Closing: return 30; break; }
        if (Task.active_count(&scope) != 0usize) return 31;
        Capture item = capture(&counter);
        Task<i32> work = success(Task.spawn_owned(&scope, [item]() -> i32 { return item.amount; }));
        i32 value = success(Task.join(work));
        ScopeReport report = success(Task.close_scope(scope));
        return value == 42 && report.accepted == 1u64 && report.completed == 1u64 && report.observed == 1u64 && success(Atomic.load(Shared.as_ref(&counter), MemoryOrder.SeqCst)) == 1 && Shared.strong_count(&counter) == 1usize ? 0 : 32;
    }
    if (stage == 10) {
        Capture item = capture(&counter);
        Task<i32> work = success(Task.spawn_owned(&scope, [item]() -> i32 { return item.amount; }));
        ScopeStatus state = Task.scope_status(&scope);
        switch (state) { case ScopeStatus.Open: return 23; break; case ScopeStatus.Closing: break; }
        Result<Task<i32>,SpawnError> rejected = Task.spawn_owned(&scope, []() -> i32 { return 0; });
        switch (move rejected) {
            case Result<Task<i32>,SpawnError>.Ok: return 24; break;
            case Result<Task<i32>,SpawnError>.Err { SpawnError error = error }: switch (error) { case SpawnError.ScopeClosed: break; default: return 25; break; } break;
        }
        i32 result = success(Task.join(work));
        ScopeReport report = success(Task.close_scope(scope));
        return result == 42 && report.accepted == 1u64 && report.observed == 1u64 && report.completed == 1u64 && success(Atomic.load(Shared.as_ref(&counter), MemoryOrder.SeqCst)) == 1 && Shared.strong_count(&counter) == 1usize ? 0 : 26;
    }
    if (stage == 9) {
        CancellationSource source = success(CancellationSource.new());
        CancellationToken observer = success(CancellationSource.token(&source));
        task_fault_arm(stage);
        success(Task.check_cancel(&observer));
        success(CancellationSource.request(&source));
        Result<unit,Cancelled> checked = Task.check_cancel(&observer);
        task_fault_reset();
        switch (move checked) {
            case Result<unit,Cancelled>.Ok: return 17; break;
            case Result<unit,Cancelled>.Err { Cancelled reason = error }: switch (reason) { case Cancelled.Requested: break; } break;
        }
        ScopeReport empty = success(Task.close_scope(scope));
        return empty.accepted == 0u64 && success(Atomic.load(Shared.as_ref(&counter), MemoryOrder.SeqCst)) == 0 && Shared.strong_count(&counter) == 1usize ? 0 : 18;
    }
    if (stage == 6) {
        Task<unit> empty = success(Task.spawn_owned(&scope, []() -> unit { return Unit.value(); }));
        unit value = success(Task.join(empty));
        Task<Result<i32,TaskError>> application = success(Task.spawn_owned(&scope, []() -> Result<i32,TaskError> { return Result.err(TaskError.Failed); }));
        Result<i32,TaskError> result = success(Task.join(application));
        switch (move result) { case Result<i32,TaskError>.Ok: return 3; break; case Result<i32,TaskError>.Err { TaskError error = error }: switch (error) { case TaskError.Failed: break; default: return 4; break; } break; }
        ScopeReport report = success(Task.close_scope(scope));
        return report.completed == 2u64 && report.failed == 0u64 && report.observed == 2u64 ? 0 : 5;
    }
    Capture item = capture(&counter);
    if (stage == 3 || stage == 5 || stage == 7 || stage == 8) task_fault_arm(stage);
    mut Task<i32> work = success(Task.spawn_owned(&scope, [item]() -> i32 { return item.amount; }));
    if (stage == 7) {
        bool rejected = native_failure(Task.wait(&work));
        success(Task.wait(&work));
        if (!Task.is_finished(&work)) return 19;
        i32 result = success(Task.join(work));
        task_fault_reset();
        ScopeReport report = success(Task.close_scope(scope));
        return rejected && result == 42 && report.observed == 1u64 && report.discarded == 0u64 && report.completed == 1u64 && success(Atomic.load(Shared.as_ref(&counter), MemoryOrder.SeqCst)) == 1 && Shared.strong_count(&counter) == 1usize ? 0 : 20;
    }
    if (stage == 8) {
        bool rejected = native_failure(Task.cancel_and_join(work));
        task_fault_reset();
        ScopeReport report = success(Task.close_scope(scope));
        return rejected && report.observed == 0u64 && report.discarded == 1u64 && report.completed + report.cancelled == 1u64 && success(Atomic.load(Shared.as_ref(&counter), MemoryOrder.SeqCst)) == 1 && Shared.strong_count(&counter) == 1usize ? 0 : 21;
    }
    if (stage == 3) {
        bool rejected = native_failure(Task.join(work));
        task_fault_reset();
        ScopeReport report = success(Task.close_scope(scope));
        return rejected && report.observed == 0u64 && report.discarded == 1u64 && report.completed == 1u64 && success(Atomic.load(Shared.as_ref(&counter), MemoryOrder.SeqCst)) == 1 && Shared.strong_count(&counter) == 1usize ? 0 : 6;
    }
    if (stage == 4) {
        success(Task.discard_result(work));
        task_fault_arm(stage);
        Result<ScopeReport,ScopeError> failed = Task.close_scope(scope);
        task_fault_reset();
        switch (move failed) {
            case Result<ScopeReport,ScopeError>.Ok: return 7; break;
            case Result<ScopeReport,ScopeError>.Err { ScopeError error = error }:
                switch (move error) {
                    case ScopeError.Retry { TaskScope retained = scope, TaskError reason = reason }:
                        switch (reason) { case TaskError.Native { i32 native = os_code, i32 system = domain }: if (native == 0 || (system != 1 && system != 2)) return 8; break; default: return 9; break; }
                        Result<Task<i32>,SpawnError> closed = Task.spawn_owned(&retained, []() -> i32 { return 1; });
                        switch (move closed) { case Result<Task<i32>,SpawnError>.Ok: return 10; break; case Result<Task<i32>,SpawnError>.Err { SpawnError problem = error }: switch (problem) { case SpawnError.ScopeClosed: break; default: return 11; break; } break; }
                        ScopeReport report = success(Task.close_scope(retained));
                        if (report.discarded != 1u64 || report.completed != 1u64) return 12;
                        break;
                    case ScopeError.Failure: return 22; break;
                }
                break;
        }
        return success(Atomic.load(Shared.as_ref(&counter), MemoryOrder.SeqCst)) == 1 && Shared.strong_count(&counter) == 1usize ? 0 : 13;
    }
    WaitStatus ready = success(Task.wait_timeout(&work, success(Duration.from_secs(10))));
    switch (ready) { case WaitStatus.Ready: break; case WaitStatus.TimedOut: return 14; break; }
    bool rejected = native_failure(Task.try_take(&mut work));
    TaskStatus preserved = Task.status(&work);
    switch (preserved.phase) { case TaskPhase.Completed: break; default: return 15; break; }
    i32 result = success(Task.join(work));
    task_fault_reset();
    ScopeReport report = success(Task.close_scope(scope));
    return rejected && result == 42 && report.observed == 1u64 && report.discarded == 0u64 && success(Atomic.load(Shared.as_ref(&counter), MemoryOrder.SeqCst)) == 1 && Shared.strong_count(&counter) == 1usize ? 0 : 16;
}
]=])
execute_process(COMMAND "${AEVIXC}" --check "${TEST_DIR}/fault.avx" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Task fault source check: ${output}${error}")
endif()
execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/fault.avx" RESULT_VARIABLE status OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 90)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Task fault IR generation: ${error}")
endif()
string(REPLACE "@main(" "@task_fault_main(" ir "${ir}")
# 대기 오류 뒤에도 살아 있는 소스 대여를 유지하며, 닫힌 ABI의 테스트 래퍼가 일회성 오류만 해제한다.
# Preserve live source loans after a wait error; the closed-ABI test wrapper clears only the one-shot injected fault.
string(REPLACE "@aevix_scoped_task_wait(" "@task_fault_scoped_wait(" ir "${ir}")
string(REPLACE "@aevix_task_scope_prepare_identified(" "@task_fault_scope_prepare(" ir "${ir}")
string(REPLACE "@aevix_task_scope_close_admission(" "@task_fault_close_admission(" ir "${ir}")
file(WRITE "${TEST_DIR}/fault.ll" "${ir}")
file(WRITE "${TEST_DIR}/fault.cpp" [=[
#include "aevix/Runtime/TaskRuntime.hpp"
#include "aevix/Runtime/TaskScopeRuntime.hpp"
#include "aevix/Runtime/CancellationRuntime.hpp"
#include "aevix/Runtime/SharedRuntime.hpp"
#include <cstdint>
#include <limits>
static std::int32_t stage;
static bool preparedContractObserved = false;
extern "C" std::int32_t task_fault_stage() { return stage; }
extern "C" void task_fault_arm(std::int32_t selected) {
    if (selected <= 1 || selected == 9) aevix_task_test_fail_after(0);
    else if (selected == 2) aevix_task_test_fail_threads(1);
    else aevix_task_test_fail_reap(selected == 5 ? 2 : 1);
}
extern "C" void task_fault_reset() {
    aevix_task_test_fail_after(std::numeric_limits<std::uint64_t>::max());
    aevix_task_test_fail_threads(0);
    aevix_task_test_fail_reap(0);
}
extern "C" std::int32_t task_fault_scoped_wait(std::uint64_t token) {
    const auto outcome = aevix_scoped_task_wait(token);
    if (stage == 7) aevix_task_test_fail_reap(0);
    return outcome;
}
extern "C" std::int32_t task_fault_scope_prepare(std::uint64_t scope, std::uint64_t captureSize, std::uint64_t captureAlignment,
    std::uint64_t resultSize, std::uint64_t resultAlignment, aevix::TaskInvoke invoke, aevix::TaskDrop captureDrop,
    aevix::TaskDrop resultDrop, std::uint64_t *handle, void **storage, std::uint64_t *diagnosticSerial) {
    const auto outcome = aevix_task_scope_prepare_identified(scope, captureSize, captureAlignment, resultSize, resultAlignment,
        invoke, captureDrop, resultDrop, handle, storage, diagnosticSerial);
    if (stage == 10 && outcome == 0) {
        std::uint64_t active = 99;
        std::uint32_t state = 99;
        preparedContractObserved = aevix_task_scope_close_admission(scope) == 0
            && aevix_task_scope_active_count(scope, &active) == 0 && active == 1
            && aevix_task_scope_status(scope, &state) == 0 && state == std::uint32_t(aevix::TaskScopeState::Closing);
    }
    return outcome;
}
extern "C" std::int32_t task_fault_close_admission(std::uint64_t scope) {
    return stage == 11 ? 2 : aevix_task_scope_close_admission(scope);
}
extern "C" int task_fault_main();
int main(int argc, char **argv) {
    if (argc != 2) return 30;
    if (argv[1][0] == '1' && (argv[1][1] == '0' || argv[1][1] == '1') && !argv[1][2]) stage = 10 + argv[1][1] - '0';
    else { if (argv[1][0] < '0' || argv[1][0] > '9' || argv[1][1]) return 30; stage = argv[1][0] - '0'; }
    const auto result = task_fault_main();
    task_fault_reset();
    if (result) return 40 + result;
    if (stage == 10 && !preparedContractObserved) return 33;
    if (aevix_task_scope_runtime_drain()) return 31;
    return aevix_task_scope_test_live_scopes() == 0 && aevix_task_scope_test_live_handles() == 0 && aevix_task_test_live_controls() == 0 && aevix_cancel_test_live_states() == 0 && aevix_shared_test_live_controls() == 0 ? 0 : 32;
}
]=])
foreach(opt IN ITEMS 0 2)
    execute_process(COMMAND "${CLANG}" --driver-mode=g++ -std=c++20 -O${opt} -fno-exceptions -fno-rtti -pthread
        -DAEVIX_TASK_RUNTIME_TESTING -DAEVIX_SHARED_RUNTIME_TESTING "-I${repo}/include"
        "${TEST_DIR}/fault.ll" "${TEST_DIR}/fault.cpp" "${repo}/runtime/Threading/TaskRuntime.cpp"
        "${repo}/runtime/Memory/SharedRuntime.cpp" "${repo}/runtime/Threading/AtomicRuntime.cpp"
        "${repo}/runtime/Native/NativeBoundary.cpp" "${repo}/runtime/System/OsRuntime.cpp"
        -o "${TEST_DIR}/fault-O${opt}" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 180)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Task native fault harness O${opt}: ${output}${error}")
    endif()
    # 새로운 프로세스는 레지스트리 재사용이 의도한 할당 실패를 우회하지 못하게 한다.
    # Fresh processes prevent registry reuse from bypassing the selected allocation fault.
    foreach(pass RANGE 1 8)
        foreach(stage RANGE 0 11)
            execute_process(COMMAND "${TEST_DIR}/fault-O${opt}" "${stage}" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
            if(NOT status EQUAL 0)
                message(FATAL_ERROR "Task fault/Drop O${opt} pass${pass} stage${stage}: ${status}\n${output}${error}")
            endif()
        endforeach()
    endforeach()
endforeach()
message(STATUS "Task source failure mapping, retained ownership, scope retry, and exact cleanup passed")
