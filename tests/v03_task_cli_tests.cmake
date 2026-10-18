# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_task_cli_tests.cmake
#
# Description :
#     소유 캡처 작업의 실제 실행·정리와 스코프 수명 및 작업자 안전 경계를 검사한다.
#     Check real owned-capture work, cleanup, scope lifetimes, and worker safety boundaries.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(task_prelude [=[
import task { Task, TaskScope, Executor, ScopeOptions, ScopeReport, ScopeError, SpawnError, TaskStatus, TaskPhase, WaitStatus, TaskCancelRequest, TaskError };
import time { Duration };
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
]=])
function(task_run name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(task_accept name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${task_prelude}\n${source}\n")
    task_run("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    foreach(optimization 0 2)
        task_run("${name} AOT O${optimization}" 0 "" --run "-O${optimization}" "${TEST_DIR}/${name}.avx")
        if(version MATCHES "ORC JIT enabled")
            task_run("${name} JIT O${optimization}" 0 "" --jit "-O${optimization}" "${TEST_DIR}/${name}.avx")
        endif()
    endforeach()
endfunction()
function(task_reject name source pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${task_prelude}\n${source}\n")
    task_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()

task_accept(join_and_report [=[
i32 main() {
    TaskScope scope = open();
    i32 input = 40;
    Task<i32> work = success(Task.spawn_owned(&scope, [input]() -> i32 { return input + 2; }));
    i32 answer = success(Task.join(work));
    ScopeReport report = success(Task.close_scope(scope));
    return answer == 42 && report.accepted == 1u64 && report.observed == 1u64 && report.completed == 1u64 && report.discarded == 0u64 ? 0 : 1;
}
]=])

task_accept(try_take_and_consumed [=[
i32 main() {
    TaskScope scope = open();
    mut Task<i32> work = success(Task.spawn(&scope, []() -> i32 { return 37; }));
    WaitStatus waited = success(Task.wait_timeout(&work, success(Duration.from_secs(10))));
    switch (waited) { case WaitStatus.Ready: break; case WaitStatus.TimedOut: return 1; break; }
    Optional<i32> found = success(Task.try_take(&mut work));
    switch (move found) {
        case Optional<i32>.Some { i32 value = value }: if (value != 37) return 2; break;
        case Optional<i32>.None: return 3; break;
    }
    TaskStatus state = Task.status(&work);
    switch (state.phase) { case TaskPhase.Consumed: break; default: return 4; break; }
    Result<Optional<i32>,TaskError> repeated = Task.try_take(&mut work);
    switch (move repeated) {
        case Result<Optional<i32>,TaskError>.Ok: return 5; break;
        case Result<Optional<i32>,TaskError>.Err { TaskError error = error }:
            switch (error) { case TaskError.AlreadyJoined: break; default: return 6; break; }
            break;
    }
    return 0;
}
]=])

task_accept(timeout_and_nonconsuming_observation [=[
i32 main() {
    TaskScope scope = open();
    Task<i32> work = success(Task.spawn_owned(&scope, []() -> i32 {
        mut i32 sum = 0;
        for (mut i32 index = 0; index < 10000; index++) sum = (sum + index) % 100003;
        return sum;
    }));
    WaitStatus snapshot = success(Task.wait_timeout(&work, Duration.from_nanos(0u64)));
    switch (snapshot) { case WaitStatus.Ready: break; case WaitStatus.TimedOut: break; }
    Result<WaitStatus,TaskError> excessive = Task.wait_timeout(&work, Duration.from_nanos(18446744073709551615u64));
    switch (move excessive) {
        case Result<WaitStatus,TaskError>.Ok: return 1; break;
        case Result<WaitStatus,TaskError>.Err { TaskError error = error }:
            switch (error) { case TaskError.Overflow: break; default: return 2; break; }
            break;
    }
    i32 found = success(Task.join(work));
    return found >= 0 && found < 100003 ? 0 : 3;
}
]=])

task_accept(owned_strings_and_wide_values [=[
i32 main() {
    TaskScope scope = open();
    string message = success(string.from_view("owned capture"));
    Task<string> text = success(Task.spawn_owned(&scope, [message]() -> string { return success(string.from_view(string.view(&message))); }));
    Task<i128> wide = success(Task.spawn_owned(&scope, []() -> i128 { return -170141183460469231731687303715884105728i128; }));
    string result = success(Task.join(text));
    i128 value = success(Task.join(wide));
    return string.equals(&result, "owned capture") && value == -170141183460469231731687303715884105728i128 ? 0 : 1;
}
]=])

task_accept(discard_and_scope_drop [=[
i32 main() {
    {
        TaskScope scope = open();
        Task<string> work = success(Task.spawn_owned(&scope, []() -> string { return success(string.from_view("unclaimed")); }));
        success(Task.discard_result(work));
        ScopeReport report = success(Task.close_scope(scope));
        if (report.accepted != 1u64 || report.discarded != 1u64 || report.observed != 0u64 || report.completed != 1u64) return 1;
    }
    {
        TaskScope scope = open();
        Task<string> work = success(Task.spawn_owned(&scope, []() -> string { return success(string.from_view("implicit")); }));
    }
    TaskScope reopened = open();
    ScopeReport empty = success(Task.close_scope(reopened));
    return empty.accepted == 0u64 ? 0 : 2;
}
]=])

task_accept(cancellation_race_is_a_request [=[
i32 main() {
    TaskScope scope = open();
    Task<i32> work = success(Task.spawn_owned(&scope, []() -> i32 { return 91; }));
    TaskCancelRequest requested = success(Task.request_cancel(&work));
    switch (requested) { case TaskCancelRequest.NewlyRequested: break; case TaskCancelRequest.AlreadyRequested: break; case TaskCancelRequest.AlreadyTerminal: break; }
    Result<i32,TaskError> outcome = Task.join(work);
    mut bool completed = false;
    switch (move outcome) {
        case Result<i32,TaskError>.Ok { i32 value = value }: if (value != 91) return 1; completed = true; break;
        case Result<i32,TaskError>.Err { TaskError error = error }:
            switch (error) { case TaskError.Cancelled: break; default: return 2; break; }
            break;
    }
    ScopeReport report = success(Task.close_scope(scope));
    return report.accepted == 1u64 && report.observed == 1u64 && report.completed + report.cancelled == 1u64 && report.failed == 0u64 && (completed ? report.completed == 1u64 : report.cancelled == 1u64) ? 0 : 3;
}
]=])

task_accept(helper_scope_loan [=[
Task<i32> relay<T>(T input) { return input; }
Task<i32> make(&TaskScope scope) { return success(Task.spawn_owned(scope, []() -> i32 { return 12; })); }
i32 main() {
    TaskScope scope = open();
    Task<i32> work = relay(make(&scope));
    i32 answer = success(Task.join(work));
    ScopeReport report = success(Task.close_scope(scope));
    return answer == 12 && report.observed == 1u64 ? 0 : 1;
}
]=])

set(loan_pattern "borrow|Borrow|loan|Loan|live|scope|Scope")
task_reject(unused_task_holds_scope [=[i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.spawn_owned(&scope, []() -> i32 { return 1; })); Task.close_scope(scope); return 0; }]=] "${loan_pattern}")
task_reject(optional_task_holds_scope [=[i32 main() { TaskScope scope = open(); Optional<Task<i32>> held = Optional.some(success(Task.spawn_owned(&scope, []() -> i32 { return 1; }))); Task.close_scope(scope); return 0; }]=] "${loan_pattern}")
task_reject(tuple_task_holds_scope [=[i32 main() { TaskScope scope = open(); Tuple<Task<i32>,i32> held = Tuple.make(success(Task.spawn_owned(&scope, []() -> i32 { return 1; })), 7); Task.close_scope(scope); return 0; }]=] "${loan_pattern}")
task_reject(helper_task_holds_scope [=[T identity<T>(T value) { return value; } i32 main() { TaskScope scope = open(); Task<i32> held = identity(success(Task.spawn_owned(&scope, []() -> i32 { return 1; }))); Task.close_scope(scope); return 0; }]=] "${loan_pattern}")
task_reject(local_scope_cannot_escape [=[Task<i32> escape() { TaskScope scope = open(); return success(Task.spawn_owned(&scope, []() -> i32 { return 1; })); } i32 main() { return 0; }]=] "${loan_pattern}")
task_reject(optional_local_scope_cannot_escape [=[Optional<Task<i32>> escape() { TaskScope scope = open(); return Optional.some(success(Task.spawn_owned(&scope, []() -> i32 { return 1; }))); } i32 main() { return 0; }]=] "${loan_pattern}")
task_reject(no_borrowed_capture [=[i32 main() { TaskScope scope = open(); i32 local = 7; rsn invalid = Task.spawn_owned(&scope, [&local]() -> i32 { return local; }); return 0; }]=] "borrow|Borrow|Send|worker|Worker")
task_reject(no_borrowed_result [=[i32 main() { TaskScope scope = open(); rsn invalid = Task.spawn_owned(&scope, []() -> string_view { return "static text"; }); return 0; }]=] "borrow|Borrow|Send|worker|Worker")
task_reject(no_unknown_ffi_worker [=[extern "C" i32 task_unknown_effect(); i32 helper() { return task_unknown_effect(); } i32 main() { TaskScope scope = open(); rsn invalid = Task.spawn_owned(&scope, []() -> i32 { return helper(); }); return 0; }]=] "foreign|Foreign|FFI|effect|worker|Worker")
task_reject(no_mutable_global_worker [=[persist mut i32 global = 0; i32 helper() { global++; return global; } i32 main() { TaskScope scope = open(); rsn invalid = Task.spawn_owned(&scope, []() -> i32 { return helper(); }); return 0; }]=] "global|Global|persist|effect|worker|Worker")
task_reject(no_scope_private_identity [=[i32 main() { TaskScope scope = open(); return scope.state_id == 0u64 ? 0 : 1; }]=] "private")
task_reject(no_scope_forgery [=[i32 main() { TaskScope scope = TaskScope { state_id = 1u64 }; return 0; }]=] "constructed|private|compiler")
task_reject(no_task_forgery [=[i32 main() { Task<i32> task = Task<i32> { result_type = 7 }; return 0; }]=] "constructed|private|compiler")
task_reject(no_task_copy [=[i32 main() { TaskScope scope = open(); Task<i32> task = success(Task.spawn_owned(&scope, []() -> i32 { return 1; })); Task<i32> moved = task; Task.status(&task); return 0; }]=] "moved|move|unavailable")
task_reject(no_scope_copy [=[i32 main() { TaskScope scope = open(); TaskScope moved = scope; Task.close_scope(scope); return 0; }]=] "moved|move|unavailable")
task_reject(no_copy_container [=[struct Bad implements Copy { public TaskScope scope; }; i32 main() { return 0; }]=] "Copy|copy")
task_reject(no_shared_try_take [=[i32 main() { TaskScope scope = open(); Task<i32> task = success(Task.spawn_owned(&scope, []() -> i32 { return 1; })); Task.try_take(&task); return 0; }]=] "mutable|Mutable|borrow|Task")
task_reject(no_global_scope [=[TaskScope global = open(); i32 main() { return 0; }]=] "global|persist|owned-worker")
task_reject(no_persist_scope [=[i32 main() { persist TaskScope scope = open(); return 0; }]=] "global|persist|owned-worker")
task_reject(no_nested_persist_scope [=[struct Holder { public TaskScope scope; }; i32 main() { persist Holder holder = Holder { scope = open() }; return 0; }]=] "global|persist|owned-worker")
task_reject(no_raw_runtime_extern [=[extern "C" i32 aevix_scoped_task_discard(u64 handle); i32 main() { return 0; }]=] "reserved|compiler|aevix_")
file(WRITE "${TEST_DIR}/import_required.avx" "i32 main() { TaskScope scope; return 0; }\n")
task_run("Task requires import" 1 "Unknown|unknown|undeclared" --check "${TEST_DIR}/import_required.avx")
message(STATUS "Owned Task execution, lifecycle, scope loans, and closed worker safety passed")
