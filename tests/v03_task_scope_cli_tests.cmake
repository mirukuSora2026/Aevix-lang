# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_task_scope_cli_tests.cmake
#
# Description :
#     실제 소스에서 스코프 입장 중단·미완료 수·상태와 남은 정리 및 대여 의무를 검사한다.
#     Exercise source scope admission, unfinished counts, state and retained cleanup/loan obligations.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(prelude [=[
import task { Task, TaskScope, Executor, ScopeOptions, ScopeStatus, ScopeReport, ScopeError, SpawnError, TaskError };
import memory { Shared };
import atomic { Atomic, MemoryOrder };
T success<T,E>(Result<T,E> input) {
    switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; }
}
TaskScope open() { Executor executor = Executor.native_threads(); return success(Task.open_scope(&executor, ScopeOptions.defaults())); }
bool closing(ScopeStatus state) { switch (state) { case ScopeStatus.Open: return false; break; case ScopeStatus.Closing: return true; break; } }
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
accept(empty_admission_and_rejected_capture [=[
struct Capture implements Drop {
    public Shared<Atomic<i32>> count;
    public void drop(&mut Self self) { success(Atomic.fetch_add(Shared.as_ref(&self.count), 1, MemoryOrder.SeqCst)); return; }
};
i32 main() {
    TaskScope scope = open();
    if (Task.active_count(&scope) != 0usize || closing(Task.scope_status(&scope))) return 1;
    success(Task.close_admission(&scope));
    success(Task.close_admission(&scope));
    if (Task.active_count(&scope) != 0usize || !closing(Task.scope_status(&scope))) return 2;
    Shared<Atomic<i32>> count = success(Shared.new(Atomic.new(0)));
    Capture owned = Capture { count = success(Shared.clone(&count)) };
    Result<Task<i32>,SpawnError> rejected = Task.spawn_owned(&scope, [owned]() -> i32 { return success(Atomic.load(Shared.as_ref(&owned.count), MemoryOrder.SeqCst)); });
    switch (move rejected) {
        case Result<Task<i32>,SpawnError>.Ok: return 3; break;
        case Result<Task<i32>,SpawnError>.Err { SpawnError error = error }: switch (error) { case SpawnError.ScopeClosed: break; default: return 4; break; } break;
    }
    ScopeReport report = success(Task.close_scope(scope));
    return report.accepted == 0u64 && success(Atomic.load(Shared.as_ref(&count), MemoryOrder.SeqCst)) == 1 && Shared.strong_count(&count) == 1usize ? 0 : 5;
}
]=])
accept(discarded_running_work_is_active [=[
i32 main() {
    TaskScope scope = open();
    Shared<Atomic<i32>> gate = success(Shared.new(Atomic.new(0)));
    Shared<Atomic<i32>> captured = success(Shared.clone(&gate));
    Task<i32> work = success(Task.spawn_owned(&scope, [captured]() -> i32 {
        success(Atomic.store(Shared.as_ref(&captured), 1, MemoryOrder.Release));
        while (success(Atomic.load(Shared.as_ref(&captured), MemoryOrder.Acquire)) != 2) {}
        return 67;
    }));
    while (success(Atomic.load(Shared.as_ref(&gate), MemoryOrder.Acquire)) != 1) {}
    usize before = Task.active_count(&scope);
    success(Task.discard_result(work));
    success(Task.close_admission(&scope));
    usize after = Task.active_count(&scope);
    bool stopped = closing(Task.scope_status(&scope));
    success(Atomic.store(Shared.as_ref(&gate), 2, MemoryOrder.Release));
    ScopeReport report = success(Task.close_scope(scope));
    return before == 1usize && after == 1usize && stopped && report.accepted == 1u64 && report.discarded == 1u64 && report.completed == 1u64 && Shared.strong_count(&gate) == 1usize ? 0 : 1;
}
]=])
accept(terminal_unclaimed_is_not_drained [=[
i32 main() {
    TaskScope scope = open();
    Task<string> work = success(Task.spawn_owned(&scope, []() -> string { return success(string.from_view("unclaimed owner")); }));
    success(Task.wait(&work));
    usize count = Task.active_count(&scope);
    bool was_open = !closing(Task.scope_status(&scope));
    success(Task.close_admission(&scope));
    bool is_closing = closing(Task.scope_status(&scope));
    string result = success(Task.join(work));
    ScopeReport report = success(Task.close_scope(scope));
    return count == 0usize && was_open && is_closing && string.equals(&result, "unclaimed owner") && report.completed == 1u64 && report.observed == 1u64 ? 0 : 1;
}
]=])
accept(nonowning_failure_and_helpers [=[
T identity<T>(T value) { return value; }
usize inspect(&mut TaskScope scope) { ScopeStatus state = identity(Task.scope_status(scope)); return identity(Task.active_count(scope)); }
i32 main() {
    mut TaskScope scope = open();
    if (inspect(&mut scope) != 0usize) return 1;
    ScopeError error = ScopeError.Failure { reason = TaskError.Overflow };
    TaskError overflow = TaskError.Overflow;
    bool message = string.equals(error.code(), overflow.code()) && string.byte_len(error.message()) > 0usize;
    success(Task.close_admission(&mut scope));
    ScopeReport report = success(Task.close_scope(scope));
    return message && report.accepted == 0u64 ? 0 : 2;
}
]=])
set(loan "borrow|Borrow|loan|Loan|live|scope|Scope")
foreach(operation active_count scope_status close_admission)
    reject("${operation}_keeps_task_scope_loan" "i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope, []() -> i32 { return 1; })); Task.${operation}(&scope); Task.close_scope(scope); return 0; }" "${loan}")
    reject("${operation}_rejects_task_receiver" "i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope, []() -> i32 { return 1; })); Task.${operation}(&work); return 0; }" "scope|Scope|owner")
    reject("${operation}_rejects_namespace_types" "i32 main() { TaskScope scope = open(); Task<i32>.${operation}(&scope); return 0; }" "scope operations|result type")
endforeach()
reject(scope_state_needs_borrow [=[i32 main() { TaskScope scope = open(); Task.scope_status(scope); return 0; }]=] "borrow|scope|Scope")
reject(no_fake_drained [=[i32 main() { ScopeStatus status = ScopeStatus.Drained; return 0; }]=] "Unknown|unknown|member|Member|Drained")
reject(nonowning_failure_has_no_scope [=[i32 main() { TaskScope scope = open(); ScopeError error = ScopeError.Failure { scope = scope, reason = TaskError.Overflow }; return 0; }]=] "field|Field|scope")
reject(no_function_type_args [=[i32 main() { TaskScope scope = open(); Task.active_count<i32>(&scope); return 0; }]=] "infer|argument|Task")
reject(scope_operations_not_worker_effects [=[i32 main() { TaskScope scope = open(); Task<usize> work = success(Task.spawn_owned(&scope, []() -> usize { TaskScope inner = open(); return Task.active_count(&inner); })); return 0; }]=] "worker|Worker|effect|Task")
file(MAKE_DIRECTORY "${TEST_DIR}/namespace")
file(WRITE "${TEST_DIR}/namespace/Task.avx" "public i32 active_count(i32 value) { return value; } public i32 scope_status<T>(i32 value) { return value; } public i32 close_admission(i32 value) { return value; }\n")
file(WRITE "${TEST_DIR}/namespace/main.avx" "import Task; i32 main() { return Task.active_count(3) + Task.scope_status<i32>(5) + Task.close_admission(7) == 15 ? 0 : 1; }\n")
run("user Task scope namespace check" 0 "" --module-root "${TEST_DIR}/namespace" --check "${TEST_DIR}/namespace/main.avx")
foreach(opt 0 2)
    run("user Task scope namespace AOT O${opt}" 0 "" --module-root "${TEST_DIR}/namespace" --run "-O${opt}" "${TEST_DIR}/namespace/main.avx")
    if(version MATCHES "ORC JIT enabled")
        run("user Task scope namespace JIT O${opt}" 0 "" --module-root "${TEST_DIR}/namespace" --jit "-O${opt}" "${TEST_DIR}/namespace/main.avx")
    endif()
endforeach()
message(STATUS "Scope admission, active-work observations and retained cleanup/loan contracts passed")
