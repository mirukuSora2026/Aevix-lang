# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_task_observation_cli_tests.cmake
#
# Description :
#     작업의 비소비 관측·완료 대기·취소 합류와 독립 취소 확인을 실제 호출로 검사한다.
#     Exercise non-consuming Task observations, completion waits, cancellation joins and independent checkpoints.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(prelude [=[
import task { Task, TaskScope, Executor, ExecutorId, ScopeOptions, ScopeReport, SpawnError, TaskError, TaskPhase, Cancelled, CancellationSource, CancellationToken };
import memory { Shared };
import atomic { Atomic, MemoryOrder };
T success<T,E>(Result<T,E> input) {
    switch (move input) {
        case Result<T,E>.Ok { T value = value }: return value; break;
        case Result<T,E>.Err: Never.unreachable(); break;
    }
}
TaskScope open() {
    Executor executor = Executor.native_threads();
    return success(Task.open_scope(&executor, ScopeOptions.defaults()));
}
bool native_backend(ExecutorId value) {
    switch (value) { case ExecutorId.NativeThreads: return true; break; }
}
bool checkpoint_cancelled(Result<unit,Cancelled> result) {
    switch (move result) {
        case Result<unit,Cancelled>.Ok: return false; break;
        case Result<unit,Cancelled>.Err { Cancelled reason = error }:
            switch (reason) { case Cancelled.Requested: return true; break; }
            break;
    }
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
accept(gated_completion [=[
i32 main() {
    TaskScope scope = open();
    Shared<Atomic<i32>> gate = success(Shared.new(Atomic.new(0)));
    Shared<Atomic<i32>> captured = success(Shared.clone(&gate));
    Task<i32> work = success(Task.spawn_owned(&scope, [captured]() -> i32 {
        while (success(Atomic.load(Shared.as_ref(&captured), MemoryOrder.Acquire)) == 0) {}
        return 47;
    }));
    bool early = Task.is_finished(&work);
    bool backend = native_backend(Task.executor(&work));
    success(Atomic.store(Shared.as_ref(&gate), 1, MemoryOrder.Release));
    success(Task.wait(&work));
    bool finished = Task.is_finished(&work);
    success(Task<i32>.wait(&work));
    i32 value = success(Task.join(work));
    ScopeReport report = success(Task.close_scope(scope));
    return !early && backend && finished && value == 47 && report.observed == 1u64 && report.completed == 1u64 && Shared.strong_count(&gate) == 1usize ? 0 : 1;
}
]=])
accept(consumed_observation [=[
i32 main() {
    TaskScope scope = open();
    mut Task<i32> work = success(Task.spawn_owned(&scope, []() -> i32 { return 31; }));
    success(Task.wait(&work));
    Optional<i32> found = success(Task.try_take(&mut work));
    switch (move found) { case Optional<i32>.Some { i32 value = value }: if (value != 31) return 1; break; case Optional<i32>.None: return 2; break; }
    if (!Task.is_finished(&work) || !native_backend(Task.executor(&work))) return 3;
    Result<unit,TaskError> repeated = Task.wait(&work);
    switch (move repeated) { case Result<unit,TaskError>.Ok: return 4; break; case Result<unit,TaskError>.Err { TaskError reason = error }: switch (reason) { case TaskError.AlreadyJoined: break; default: return 5; break; } break; }
    Result<i32,TaskError> ended = Task.cancel_and_join(work);
    switch (move ended) { case Result<i32,TaskError>.Ok: return 6; break; case Result<i32,TaskError>.Err { TaskError reason = error }: switch (reason) { case TaskError.AlreadyJoined: break; default: return 7; break; } break; }
    ScopeReport report = success(Task.close_scope(scope));
    return report.observed == 1u64 && report.completed == 1u64 ? 0 : 8;
}
]=])
accept(cancel_join_completed_and_nested [=[
i32 main() {
    TaskScope scope = open();
    Task<i32> completed = success(Task.spawn_owned(&scope, []() -> i32 { return 83; }));
    success(Task.wait(&completed));
    i32 value = success(Task.cancel_and_join(completed));
    Task<Result<i32,TaskError>> nested = success(Task.spawn_owned(&scope, []() -> Result<i32,TaskError> { return Result.err(TaskError.Failed); }));
    success(Task.wait(&nested));
    Result<i32,TaskError> application = success(Task.cancel_and_join(nested));
    switch (move application) { case Result<i32,TaskError>.Ok: return 1; break; case Result<i32,TaskError>.Err { TaskError reason = error }: switch (reason) { case TaskError.Failed: break; default: return 2; break; } break; }
    ScopeReport report = success(Task.close_scope(scope));
    return value == 83 && report.observed == 2u64 && report.completed == 2u64 && report.cancelled == 0u64 ? 0 : 3;
}
]=])
accept(cancel_join_race_and_drop [=[
struct Capture implements Drop {
    public Shared<Atomic<i32>> count;
    public void drop(&mut Self self) { success(Atomic.fetch_add(Shared.as_ref(&self.count), 1, MemoryOrder.SeqCst)); return; }
};
i32 main() {
    TaskScope scope = open();
    Shared<Atomic<i32>> count = success(Shared.new(Atomic.new(0)));
    Capture captured = Capture { count = success(Shared.clone(&count)) };
    Task<i32> work = success(Task.spawn_owned(&scope, [captured]() -> i32 { return success(Atomic.load(Shared.as_ref(&captured.count), MemoryOrder.SeqCst)) + 19; }));
    Result<i32,TaskError> outcome = Task.cancel_and_join(work);
    switch (move outcome) { case Result<i32,TaskError>.Ok { i32 value = value }: if (value != 19) return 1; break; case Result<i32,TaskError>.Err { TaskError reason = error }: switch (reason) { case TaskError.Cancelled: break; default: return 2; break; } break; }
    ScopeReport report = success(Task.close_scope(scope));
    return report.observed == 1u64 && report.completed + report.cancelled == 1u64 && success(Atomic.load(Shared.as_ref(&count), MemoryOrder.SeqCst)) == 1 && Shared.strong_count(&count) == 1usize ? 0 : 3;
}
]=])
accept(independent_checkpoint [=[
CancellationToken requested_token() {
    CancellationSource source = success(CancellationSource.new());
    CancellationToken observer = success(CancellationSource.token(&source));
    success(Task.check_cancel(&observer));
    success(CancellationSource.request(&source));
    return observer;
}
i32 main() {
    CancellationToken observer = requested_token();
    if (!checkpoint_cancelled(Task.check_cancel(&observer))) return 1;
    TaskScope scope = open();
    Task<Result<unit,Cancelled>> work = success(Task.spawn_owned(&scope, [observer]() -> Result<unit,Cancelled> { return Task.check_cancel(&observer); }));
    Result<unit,Cancelled> application = success(Task.join(work));
    ScopeReport report = success(Task.close_scope(scope));
    Cancelled reason = Cancelled.Requested;
    return checkpoint_cancelled(application) && report.completed == 1u64 && report.cancelled == 0u64 && string.equals(reason.code(), "task.cancelled") && string.byte_len(reason.message()) > 0usize ? 0 : 2;
}
]=])
accept(inference_and_read_borrow [=[
T identity<T>(T input) { return input; }
bool inspect(&mut Task<i32> work) { ExecutorId executor = identity(Task.executor(work)); success(Task.wait(work)); return identity(Task.is_finished(work)) && native_backend(executor); }
i32 main() { TaskScope scope = open(); mut Task<i32> work = success(Task.spawn_owned(&scope, []() -> i32 { return 12; })); bool done = inspect(&mut work); i32 value = success(Task.join(work)); return done && value == 12 ? 0 : 1; }
]=])
set(loan "borrow|Borrow|loan|Loan|live|scope|Scope")
reject(finished_does_not_release_scope [=[i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope, []() -> i32 { return 1; })); bool done = Task.is_finished(&work); Task.close_scope(scope); return 0; }]=] "${loan}")
reject(wait_does_not_release_scope [=[i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope, []() -> i32 { return 1; })); success(Task.wait(&work)); Task.close_scope(scope); return 0; }]=] "${loan}")
reject(executor_does_not_release_scope [=[i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope, []() -> i32 { return 1; })); ExecutorId backend = Task.executor(&work); Task.close_scope(scope); return 0; }]=] "${loan}")
reject(wait_then_escape [=[Task<i32> escape() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope, []() -> i32 { return 1; })); success(Task.wait(&work)); return work; } i32 main() { return 0; }]=] "${loan}")
reject(moved_after_cancel_join [=[i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope, []() -> i32 { return 1; })); Task.cancel_and_join(work); Task.is_finished(&work); return 0; }]=] "moved|move|unavailable")
reject(checkpoint_wrong_role [=[i32 main() { CancellationSource source = success(CancellationSource.new()); Task.check_cancel(&source); return 0; }]=] "CancellationToken|borrow")
reject(checkpoint_needs_borrow [=[i32 main() { CancellationSource source = success(CancellationSource.new()); CancellationToken token = success(CancellationSource.token(&source)); Task.check_cancel(token); return 0; }]=] "CancellationToken|borrow")
reject(checkpoint_no_result_namespace [=[i32 main() { CancellationSource source = success(CancellationSource.new()); CancellationToken token = success(CancellationSource.token(&source)); Task<i32>.check_cancel(&token); return 0; }]=] "scope operations|result type|checkpoint")
reject(wait_needs_task_borrow [=[i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope, []() -> i32 { return 1; })); Task.wait(work); return 0; }]=] "borrow|Task")
reject(cancel_join_needs_owned_task [=[i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope, []() -> i32 { return 1; })); Task.cancel_and_join(&work); return 0; }]=] "owner|Task|borrow")
reject(finished_not_a_task [=[i32 main() { i32 value = 3; Task.is_finished(&value); return 0; }]=] "Task|owner")
reject(no_type_args [=[i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope, []() -> i32 { return 1; })); Task.is_finished<i32>(&work); return 0; }]=] "infer|argument|Task")
reject(wrong_namespace_result [=[i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope, []() -> i32 { return 1; })); Task<i64>.executor(&work); return 0; }]=] "result type|Task")
file(MAKE_DIRECTORY "${TEST_DIR}/namespace")
file(WRITE "${TEST_DIR}/namespace/Task.avx" "public i32 wait(i32 value) { return value; } public i32 is_finished<T>(i32 value) { return value; } public i32 executor(i32 value) { return value; }\n")
file(WRITE "${TEST_DIR}/namespace/main.avx" "import Task; i32 main() { return Task.wait(7) + Task.is_finished<i32>(11) + Task.executor(13) == 31 ? 0 : 1; }\n")
run("user Task namespace check" 0 "" --module-root "${TEST_DIR}/namespace" --check "${TEST_DIR}/namespace/main.avx")
foreach(opt 0 2)
    run("user Task namespace AOT O${opt}" 0 "" --module-root "${TEST_DIR}/namespace" --run "-O${opt}" "${TEST_DIR}/namespace/main.avx")
    if(version MATCHES "ORC JIT enabled")
        run("user Task namespace JIT O${opt}" 0 "" --module-root "${TEST_DIR}/namespace" --jit "-O${opt}" "${TEST_DIR}/namespace/main.avx")
    endif()
endforeach()
message(STATUS "Task completion, cancellation checkpoints, backend observations and preserved scope loans passed")
