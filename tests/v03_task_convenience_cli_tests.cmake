# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_task_convenience_cli_tests.cmake
#
# Description :
#     작업 취소 관측·독립 토큰·실제 단조 시계 마감값과 실패 보존을 검증한다.
#     Verify task cancellation snapshots, independent observers, real monotonic deadlines, and failure preservation.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(prelude [=[
import task { Task, TaskScope, Executor, ScopeOptions, TaskError, CancellationToken, Deadline, TimerError };
import time { Duration, monotonic_ns };
import memory { Shared };
import atomic { Atomic, MemoryOrder };
import error { Error, TimeError };
T success<T,E>(Result<T,E> input) {
    switch (move input) {
        case Result<T,E>.Ok { T value = value }: return value; break;
        case Result<T,E>.Err: Never.unreachable(); break;
    }
}
TaskScope open() {
    Executor executor = Executor.native_threads();
    return success(Task.open_scope(&executor,ScopeOptions.defaults()));
}
]=])
function(run name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(accept name body)
    file(WRITE "${TEST_DIR}/${name}.avx" "${prelude}\n${body}\n")
    run("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    foreach(opt 0 2)
        run("${name} AOT O${opt}" 0 "" --run "-O${opt}" "${TEST_DIR}/${name}.avx")
        if(version MATCHES "ORC JIT enabled")
            run("${name} JIT O${opt}" 0 "" --jit "-O${opt}" "${TEST_DIR}/${name}.avx")
        endif()
    endforeach()
endfunction()
function(reject name body pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${prelude}\n${body}\n")
    run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()

accept(gated_request_and_independent_observer [=[
CancellationToken retain_requested() {
    Shared<Atomic<i32>> gate = success(Shared.new(Atomic.new(0)));
    Shared<Atomic<i32>> capture = success(Shared.clone(&gate));
    TaskScope scope = open();
    Task<i32> work = success(Task.spawn_owned(&scope,[capture]() -> i32 {
        success(Atomic.store(Shared.as_ref(&capture),1,MemoryOrder.Release));
        while (success(Atomic.load(Shared.as_ref(&capture),MemoryOrder.Acquire)) != 2) {}
        return 42;
    }));
    while (success(Atomic.load(Shared.as_ref(&gate),MemoryOrder.Acquire)) != 1) {}
    if (Task.is_cancel_requested(&work)) Never.unreachable();
    CancellationToken observer = success(Task.cancellation(&work));
    if (CancellationToken.requested(&observer)) Never.unreachable();
    success(Task.request_cancel(&work));
    if (!Task.is_cancel_requested(&work) || !CancellationToken.requested(&observer)) Never.unreachable();
    success(Atomic.store(Shared.as_ref(&gate),2,MemoryOrder.Release));
    if (success(Task.join(work)) != 42) Never.unreachable();
    success(Task.close_scope(scope));
    return observer;
}
i32 main() {
    CancellationToken retained = retain_requested();
    if (!CancellationToken.requested(&retained)) return 1;
    TaskScope scope = open();
    Task<bool> observer_work = success(Task.spawn_owned(&scope,[retained]() -> bool { return CancellationToken.requested(&retained); }));
    return success(Task.join(observer_work)) ? 0 : 2;
}
]=])

accept(consumed_observation_and_retained_token [=[
bool requested<T>(&Task<T> input) { return Task.is_cancel_requested(input); }
Result<CancellationToken,TaskError> observe<T>(&Task<T> input) { return Task.cancellation(input); }
i32 main() {
    TaskScope scope = open();
    mut Task<i32> work = success(Task.spawn_owned(&scope,[]() -> i32 { return 23; }));
    CancellationToken observer = success(observe(&work));
    success(Task.wait(&work));
    Optional<i32> result = success(Task.try_take(&mut work));
    switch (move result) { case Optional<i32>.Some { i32 value = value }: if (value != 23) return 1; break; default: return 2; break; }
    if (requested(&work)) return 3;
    Result<CancellationToken,TaskError> consumed = Task.cancellation(&work);
    switch (move consumed) {
        case Result<CancellationToken,TaskError>.Ok: return 4; break;
        case Result<CancellationToken,TaskError>.Err { TaskError reason = error }:
            switch (reason) { case TaskError.AlreadyJoined: break; default: return 5; break; }
            break;
    }
    Task.discard_result(work);
    success(Task.close_scope(scope));
    return CancellationToken.requested(&observer) ? 6 : 0;
}
]=])

accept(checked_monotonic_deadline [=[
i32 main() {
    u64 before = success(monotonic_ns());
    Duration duration = Duration.from_nanos(1000u64);
    Deadline deadline = success(Task.deadline_after(duration));
    u64 after = success(monotonic_ns());
    if (deadline.monotonic_nanos() < before + 1000u64 || deadline.monotonic_nanos() > after + 1000u64) return 1;
    Deadline named = success(Task.deadline_after(duration = Duration.from_nanos(0u64)));
    Deadline direct = success(Deadline.after(Duration.from_nanos(0u64)));
    if (named.monotonic_nanos() < after || direct.monotonic_nanos() < named.monotonic_nanos()) return 2;
    Result<Deadline,TimerError> overflow = Task.deadline_after(Duration.from_nanos(18446744073709551615u64));
    switch (move overflow) {
        case Result<Deadline,TimerError>.Ok: return 3; break;
        case Result<Deadline,TimerError>.Err { TimerError reason = error }:
            &dyn Error view = &reason;
            if (!string.equals(view.code(),"task.timer_overflow") || string.byte_len(view.message()) == 0usize) return 4;
            switch (reason) { case TimerError.Overflow: break; default: return 5; break; }
            break;
    }
    return 0;
}
]=])

reject(snapshot_requires_borrow [=[i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope,[]() -> i32 { return 1; })); Task.is_cancel_requested(work); return 0; }]=] "borrow|Borrow|Task")
reject(observer_requires_borrow [=[i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope,[]() -> i32 { return 1; })); Task.cancellation(work); return 0; }]=] "borrow|Borrow|Task")
reject(observer_does_not_consume_task_loan [=[i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope,[]() -> i32 { return 1; })); CancellationToken observer = success(Task.cancellation(&work)); Task.close_scope(scope); return 0; }]=] "borrow|Borrow|scope|Scope")
reject(observer_namespace_result_mismatch [=[i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope,[]() -> i32 { return 1; })); Task<bool>.cancellation(&work); return 0; }]=] "result|type|Task")
reject(observer_explicit_type_arguments [=[i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope,[]() -> i32 { return 1; })); Task.cancellation<i32>(&work); return 0; }]=] "type|Task")
reject(deadline_explicit_type_arguments [=[i32 main() { Task.deadline_after<i32>(Duration.from_nanos(0u64)); return 0; }]=] "type|Task")
reject(deadline_result_namespace [=[i32 main() { Task<i32>.deadline_after(Duration.from_nanos(0u64)); return 0; }]=] "type|Task")
reject(deadline_requires_duration [=[i32 main() { Task.deadline_after(0u64); return 0; }]=] "Duration|type|argument")
reject(deadline_private_storage [=[i32 main() { Deadline deadline = Deadline { instant_nanos = 1u64 }; return 0; }]=] "private")
reject(deadline_private_read [=[i32 main() { Deadline deadline = success(Task.deadline_after(Duration.from_nanos(0u64))); return deadline.instant_nanos == 0u64 ? 1 : 0; }]=] "private")
reject(token_does_not_grant_request_authority [=[i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope,[]() -> i32 { return 1; })); CancellationToken observer = success(Task.cancellation(&work)); Task.cancel_request(&observer); return 0; }]=] "type|Source|argument")
file(WRITE "${TEST_DIR}/shadow.avx" "struct Task { public i32 deadline_after(i32 value) { return value + 1; } }; i32 main() { return Task.deadline_after(2) == 3 ? 0 : 1; }\n")
foreach(mode --check --run --jit)
    if(NOT mode STREQUAL "--jit" OR version MATCHES "ORC JIT enabled")
        run("ordinary Task shadow" 0 "" "${mode}" "${TEST_DIR}/shadow.avx")
    endif()
endforeach()
file(WRITE "${TEST_DIR}/qualified.avx" [=[
import task;
import time;
struct Task { public i32 deadline_after(i32 value) { return value + 1; } };
T success<T,E>(Result<T,E> input) {
    switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; default: Never.unreachable(); break; }
}
i32 main() {
    task.Executor executor = task.Executor.native_threads();
    task.TaskScope scope = success(task.Task.open_scope(&executor,task.ScopeOptions.defaults()));
    task.Task<i32> work = success(task.Task.spawn_owned(&scope,[]() -> i32 { return 42; }));
    task.CancellationToken token = success(task.Task<i32>.cancellation(&work));
    if (task.Task<i32>.is_cancel_requested(&work) || Task.deadline_after(2) != 3) return 1;
    if (success(task.Task.join(work)) != 42) return 2;
    success(task.Task.close_scope(scope));
    task.Deadline deadline = success(task.Task.deadline_after(time.Duration.from_nanos(0u64)));
    return deadline.monotonic_nanos() <= success(time.monotonic_ns()) && !task.CancellationToken.requested(&token) ? 0 : 3;
}
]=])
run("qualified canonical and user Task check" 0 "" --check "${TEST_DIR}/qualified.avx")
foreach(opt 0 2)
    run("qualified canonical and user Task AOT O${opt}" 0 "" --run "-O${opt}" "${TEST_DIR}/qualified.avx")
    if(version MATCHES "ORC JIT enabled")
        run("qualified canonical and user Task JIT O${opt}" 0 "" --jit "-O${opt}" "${TEST_DIR}/qualified.avx")
    endif()
endforeach()
file(WRITE "${TEST_DIR}/no_short_type_leak.avx" "import task { Task }; import time { Duration }; i32 main() { rsn deadline = Task.deadline_after(Duration.from_nanos(0u64)); Deadline unavailable; return 0; }\n")
run("alias does not import an unselected short type" 1 "Unknown|unknown|undeclared" --check "${TEST_DIR}/no_short_type_leak.avx")
file(WRITE "${TEST_DIR}/no_qualified_type_leak.avx" "import task { Task }; import time { Duration }; i32 main() { rsn deadline = Task.deadline_after(Duration.from_nanos(0u64)); task.Deadline unavailable; return 0; }\n")
run("alias does not grant an unselected qualified type" 1 "not imported|Unknown|unknown" --check "${TEST_DIR}/no_qualified_type_leak.avx")
run("deadline uses source composition" 0 "aevix_os_monotonic_ns" --emit-llvm "${TEST_DIR}/checked_monotonic_deadline.avx")

if(DEFINED CLANG AND EXISTS "${CLANG}")
    get_filename_component(repo "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    file(WRITE "${TEST_DIR}/failure.avx" "${prelude}\n")
    file(APPEND "${TEST_DIR}/failure.avx" [=[
extern "C" i32 task_convenience_stage();
i32 main() {
    i32 stage = task_convenience_stage();
    if (stage <= 2) {
        TaskScope scope = open();
        Task<i32> work = success(Task.spawn_owned(&scope,[]() -> i32 { return 23; }));
        Result<CancellationToken,TaskError> result = Task.cancellation(&work);
        switch (move result) {
            case Result<CancellationToken,TaskError>.Ok { CancellationToken observer = value }:
                if (stage != 0 || CancellationToken.requested(&observer)) return 1;
                break;
            case Result<CancellationToken,TaskError>.Err { TaskError reason = error }:
                switch (reason) {
                    case TaskError.AllocationFailure: if (stage != 1) return 2; break;
                    case TaskError.Overflow: if (stage != 2) return 3; break;
                    default: return 4; break;
                }
                break;
        }
        CancellationToken retry = success(Task.cancellation(&work));
        if (Task.is_cancel_requested(&work)) return 5;
        if (success(Task.join(work)) != 23) return 5;
        success(Task.close_scope(scope));
        return CancellationToken.requested(&retry) ? 6 : 0;
    }
    Duration duration = Duration.from_nanos(stage == 6 ? 18446744073709551615u64 : stage == 4 ? 2u64 : 1u64);
    Result<Deadline,TimerError> result = Task.deadline_after(duration);
    switch (move result) {
        case Result<Deadline,TimerError>.Ok { Deadline value = value }:
            return (stage == 5 || stage == 6) && value.monotonic_nanos() == 18446744073709551615u64 ? 0 : 7;
            break;
        case Result<Deadline,TimerError>.Err { TimerError reason = error }:
            switch (reason) {
                case TimerError.Overflow: return stage == 4 ? 0 : 8; break;
                case TimerError.Clock { error.TimeError cause = reason }:
                    switch (cause) { case error.TimeError.ClockUnavailable: return stage == 3 ? 0 : 9; break; case error.TimeError.OutOfRange: return stage == 7 ? 0 : 10; break; default: return 11; break; }
                    break;
            }
            break;
    }
}
]=])
    run("fault fixture check" 0 "" --check "${TEST_DIR}/failure.avx")
    execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/failure.avx" RESULT_VARIABLE emitted OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${emitted}" STREQUAL "0")
        message(FATAL_ERROR "fault IR: ${error}")
    endif()
    string(REPLACE "@main(" "@task_convenience_main(" ir "${ir}")
    string(REPLACE "@aevix_os_monotonic_ns(" "@task_convenience_clock(" ir "${ir}")
    string(REPLACE "@aevix_scoped_task_cancel_token(" "@task_convenience_token(" ir "${ir}")
    file(WRITE "${TEST_DIR}/failure.ll" "${ir}")
    file(WRITE "${TEST_DIR}/failure.cpp" [=[
#include "aevix/Runtime/TaskScopeRuntime.hpp"
#include "aevix/Runtime/CancellationRuntime.hpp"
#include "aevix/Runtime/OsRuntime.hpp"
#include <cstdint>
#include <limits>
static std::int32_t stage;
static unsigned token_calls;
extern "C" std::int32_t task_convenience_stage() { return stage; }
extern "C" void task_convenience_reset() {
    aevix_task_test_fail_after(UINT64_MAX); aevix_cancel_test_owner_limit(UINT64_MAX);
}
extern "C" std::int32_t task_convenience_token(std::uint64_t task,std::uint64_t* output) {
    if (token_calls++ == 0) {
        if (stage == 1) aevix_task_test_fail_after(0);
        if (stage == 2) aevix_cancel_test_owner_limit(1);
    }
    const auto status = aevix_scoped_task_cancel_token(task,output);
    task_convenience_reset(); return status;
}
extern "C" std::int32_t task_convenience_clock(std::uint64_t* output) {
    *output = 0;
    if (stage == 3) return 7;
    if (stage == 7) return 5;
    if (stage == 4 || stage == 5) { *output = UINT64_MAX - 1; return 0; }
    if (stage == 6) return 0;
    return aevix_os_monotonic_ns(output);
}
extern "C" int task_convenience_main();
int main(int argc,char** argv) {
    if (argc != 2 || argv[1][0] < '0' || argv[1][0] > '7' || argv[1][1]) return 30;
    stage = argv[1][0] - '0'; task_convenience_reset();
    const auto result = task_convenience_main(); task_convenience_reset();
    if (result) return 40 + result;
    return aevix_cancel_test_live_states() == 0 && aevix_task_scope_test_live_scopes() == 0 && aevix_task_scope_test_live_handles() == 0 ? 0 : 70;
}
]=])
    foreach(opt 0 2)
        execute_process(COMMAND "${CLANG}" --driver-mode=g++ -std=c++20 -O${opt} -fno-exceptions -fno-rtti -pthread -DAEVIX_TASK_RUNTIME_TESTING "-I${repo}/include"
            "${TEST_DIR}/failure.ll" "${TEST_DIR}/failure.cpp" "${repo}/runtime/Threading/TaskRuntime.cpp" "${repo}/runtime/Native/NativeBoundary.cpp" "${repo}/runtime/System/OsRuntime.cpp"
            -o "${TEST_DIR}/failure-O${opt}" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 180)
        if(NOT "${status}" STREQUAL "0")
            message(FATAL_ERROR "fault harness O${opt}: ${output}${error}")
        endif()
        foreach(pass RANGE 1 4)
            foreach(stage RANGE 0 7)
                execute_process(COMMAND "${TEST_DIR}/failure-O${opt}" "${stage}" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
                if(NOT "${status}" STREQUAL "0")
                    message(FATAL_ERROR "fault O${opt} pass${pass} stage${stage}: ${status}\n${output}${error}")
                endif()
            endforeach()
        endforeach()
    endforeach()
endif()
message(STATUS "Task cancellation snapshot, independent observer, and checked monotonic source helper passed")
