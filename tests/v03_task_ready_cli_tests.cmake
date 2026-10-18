# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_task_ready_cli_tests.cmake
#
# Description :
#     작업자 없는 Ready의 소유 결과, 진단 정체성, 정리와 대여 거부를 실제 소스로 검증한다.
#     Verify workerless Ready owning results, diagnostic identity, cleanup and rejected borrows in real source.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(header [=[
import task { Task, TaskId, TaskScope, Executor, ScopeOptions, ScopeReport, SpawnError, TaskError, CancellationToken, WaitStatus, TaskCancelRequest };
import memory { Shared };
import atomic { Atomic, MemoryOrder };
import time { Duration };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; default: Never.unreachable(); break; } }
TaskScope open() { Executor executor = Executor.native_threads(); return success(Task.open_scope(&executor,ScopeOptions.defaults())); }
struct Owned implements Drop {
    public Shared<Atomic<i32>> count;
    public i32 value;
    public void drop(&mut Self self) { success(Atomic.fetch_add(Shared.as_ref(&self.count),1,MemoryOrder.SeqCst)); return; }
};
Owned owned(&Shared<Atomic<i32>> count) { return Owned { count = success(Shared.clone(count)), value = 42 }; }
i32 count(&Shared<Atomic<i32>> value) { return success(Atomic.load(Shared.as_ref(value),MemoryOrder.SeqCst)); }
]=])
function(run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(accept name body)
    file(WRITE "${TEST_DIR}/${name}.avx" "${header}\n${body}\n")
    run("${name} check" 0 "ownership checked" --check "${TEST_DIR}/${name}.avx")
    foreach(opt 0 2)
        run("${name} AOT O${opt}" 0 "" --run "-O${opt}" "${TEST_DIR}/${name}.avx")
        if(version MATCHES "ORC JIT enabled")
            run("${name} JIT O${opt}" 0 "" --jit "-O${opt}" "${TEST_DIR}/${name}.avx")
        endif()
    endforeach()
endfunction()
function(reject name body pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${header}\n${body}\n")
    run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()
accept(terminal_scalar_and_unit [=[
i32 main() {
    TaskScope scope = open();
    Task<i64> work = success(Task<i64>.ready(&scope,42));
    if (!Task.is_finished(&work) || Task.active_count(&scope) != 0usize) return 1;
    switch (success(Task.wait_timeout(&work,Duration.from_nanos(0u64)))) { case WaitStatus.Ready: break; default: return 2; break; }
    switch (success(Task.request_cancel(&work))) { case TaskCancelRequest.AlreadyTerminal: break; default: return 3; break; }
    i64 result = success(Task.join(work));
    Task<unit> empty = success(Task.ready(&scope,Unit.value())); success(Task.join(empty));
    ScopeReport report = success(Task.close_scope(scope));
    return result == 42i64 && report.accepted == 2u64 && report.completed == 2u64 && report.observed == 2u64 ? 0 : 4;
}
]=])
accept(owning_result_cleanup [=[
i32 main() {
    Shared<Atomic<i32>> counter = success(Shared.new(Atomic.new(0)));
    TaskScope scope = open();
    {
        Task<Owned> work = success(Task.ready(&scope,owned(&counter)));
        Owned result = success(Task.join(work));
        if (result.value != 42 || count(&counter) != 0) return 1;
    }
    if (count(&counter) != 1) return 2;
    {
        Task<Owned> unclaimed = success(Task.ready(&scope,owned(&counter)));
        Task<Owned> discarded = success(Task.ready(&scope,owned(&counter)));
        success(Task.discard_result(discarded));
    }
    ScopeReport report = success(Task.close_scope(scope));
    return count(&counter) == 3 && report.accepted == 3u64 && report.completed == 3u64 && report.observed == 1u64 && report.discarded == 2u64 ? 0 : 3;
}
]=])
accept(consumed_identity_and_token [=[
i32 main() {
    TaskScope scope = open();
    mut Task<i32> work = success(Task.ready(&scope,42));
    TaskId first = Task.id(&work);
    CancellationToken observer = success(Task.cancellation(&work));
    Optional<i32> result = success(Task.try_take(&mut work));
    switch (move result) { case Optional<i32>.Some { i32 value = value }: if (value != 42) return 1; break; default: return 2; break; }
    TaskId second = Task.id(&work);
    Task.discard_result(work);
    success(Task.close_scope(scope));
    return first.equals(second) && !CancellationToken.requested(&observer) ? 0 : 3;
}
]=])
accept(owning_string_and_application_error [=[
i32 main() {
    TaskScope scope = open();
    string original = success(string.from_view("ready owns this value"));
    Task<string> work = success(Task.ready(&scope,original));
    string result = success(Task.join(work));
    Result<i32,TaskError> application = Result.err(TaskError.Failed);
    Task<Result<i32,TaskError>> error_value = success(Task.ready(&scope,application));
    Result<i32,TaskError> returned = success(Task.join(error_value));
    switch (move returned) { case Result<i32,TaskError>.Err { TaskError reason = error }: switch (reason) { case TaskError.Failed: break; default: return 1; break; } break; default: return 2; break; }
    ScopeReport report = success(Task.close_scope(scope));
    return string.equals(&result,"ready owns this value") && report.completed == 2u64 && report.failed == 0u64 ? 0 : 3;
}
]=])
accept(closure_is_a_value [=[
i32 main() {
    Shared<Atomic<i32>> counter = success(Shared.new(Atomic.new(0)));
    Shared<Atomic<i32>> captured = success(Shared.clone(&counter));
    TaskScope scope = open();
    rsn original = [captured]() -> i32 { return success(Atomic.fetch_add(Shared.as_ref(&captured),1,MemoryOrder.SeqCst)); };
    rsn work = success(Task.ready(&scope,original));
    if (count(&counter) != 0) return 1;
    rsn returned = success(Task.join(work));
    if (count(&counter) != 0 || returned() != 0 || count(&counter) != 1) return 2;
    success(Task.close_scope(scope));
    return 0;
}
]=])
accept(nested_owning_data [=[
enum Data { Value { public Vector<i32> values; public string label; }; };
i32 main() {
    TaskScope scope = open();
    Result<Vector<i32>,CollectionError> made = [1,2,3];
    Vector<i32> values = success(made);
    Data original = Data.Value { values = values, label = success(string.from_view("nested")) };
    Task<Data> work = success(Task.ready(&scope,original));
    Data result = success(Task.join(work));
    switch (move result) {
        case Data.Value { Vector<i32> items = values, string label = label }:
            if (Vector.len(&items) != 3usize || !string.equals(&label,"nested")) return 1;
            break;
    }
    success(Task.close_scope(scope));
    return 0;
}
]=])
set(borrowed "owned data|borrowed views|hidden Task loans|borrow|Borrow|scope|Scope")
reject(no_view [=[i32 main() { TaskScope scope = open(); Task.ready(&scope,"borrowed"); return 0; }]=] "${borrowed}")
reject(no_reference [=[i32 main() { TaskScope scope = open(); i32 value = 42; Task.ready(&scope,&value); return 0; }]=] "${borrowed}")
reject(no_hidden_borrow [=[struct Holder { public &i32 view; }; i32 main() { TaskScope scope = open(); i32 value = 42; Holder holder = Holder { view = &value }; Task.ready(&scope,holder); return 0; }]=] "${borrowed}")
reject(no_hidden_task [=[i32 main() { TaskScope scope = open(); Task<i32> child = success(Task.ready(&scope,42)); Task.ready(&scope,child); return 0; }]=] "${borrowed}")
reject(no_scope_value [=[i32 main() { TaskScope parent = open(); TaskScope child = open(); Task.ready(&parent,child); return 0; }]=] "${borrowed}")
reject(no_scope_escape [=[Task<i32> escape() { TaskScope scope = open(); return success(Task.ready(&scope,42)); } i32 main() { return 0; }]=] "${borrowed}|local|escape")
reject(keeps_scope_loan [=[i32 main() { TaskScope scope = open(); Task<i32> work = success(Task.ready(&scope,42)); Task.close_scope(scope); return 0; }]=] "${borrowed}")
reject(no_value_reuse [=[i32 main() { TaskScope scope = open(); Shared<Atomic<i32>> counter = success(Shared.new(Atomic.new(0))); Owned value = owned(&counter); Task<Owned> work = success(Task.ready(&scope,value)); return value.value; }]=] "moved|unavailable|initialized")
reject(no_unknown_cleanup [=[extern "C" i32 unknown_effect(); struct Unsafe implements Drop { public void drop(&mut Self self) { unknown_effect(); return; } }; i32 main() { TaskScope scope = open(); Unsafe value = Unsafe {}; Task.ready(&scope,value); return 0; }]=] "${borrowed}|cleanup")
reject(no_borrowless_scope_arg [=[i32 main() { TaskScope scope = open(); Task.ready(scope,42); return 0; }]=] "borrow|owner")
reject(no_wrong_t [=[i32 main() { TaskScope scope = open(); i32 value = 42; Task<i64>.ready(&scope,value); return 0; }]=] "requires|received|type|Type")
reject(no_function_type_args [=[i32 main() { TaskScope scope = open(); Task.ready<i32>(&scope,42); return 0; }]=] "infer|type|Type")
if(DEFINED CLANG AND EXISTS "${CLANG}" AND DEFINED RUNTIME AND EXISTS "${RUNTIME}" AND NOT WIN32)
    set(extra_libraries -pthread)
    if(APPLE)
        list(APPEND extra_libraries -lc++)
    else()
        list(APPEND extra_libraries -lstdc++)
    endif()
    foreach(name terminal_scalar_and_unit owning_result_cleanup consumed_identity_and_token owning_string_and_application_error closure_is_a_value nested_owning_data)
        execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/${name}.avx" OUTPUT_FILE "${TEST_DIR}/${name}.ll" RESULT_VARIABLE emitted ERROR_VARIABLE error TIMEOUT 90)
        if(NOT "${emitted}" STREQUAL "0")
            message(FATAL_ERROR "sanitizer emit ${name}: ${error}")
        endif()
        # LLVM 입력에는 C 프런트엔드가 없으므로 ASan 함수 속성을 직접 붙여 실제 계측을 보장한다.
        # LLVM input bypasses the C frontend; add ASan function attributes so generated functions are actually instrumented.
        file(READ "${TEST_DIR}/${name}.ll" sanitizer_ir)
        string(REPLACE ") {" ") sanitize_address {" sanitizer_ir "${sanitizer_ir}")
        string(REPLACE ") noreturn {" ") noreturn sanitize_address {" sanitizer_ir "${sanitizer_ir}")
        file(WRITE "${TEST_DIR}/${name}-sanitized.ll" "${sanitizer_ir}")
        execute_process(COMMAND "${CLANG}" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "${TEST_DIR}/${name}-sanitized.ll" "${RUNTIME}" ${extra_libraries} -o "${TEST_DIR}/${name}-sanitized" RESULT_VARIABLE linked OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
        if(NOT "${linked}" STREQUAL "0")
            message(FATAL_ERROR "sanitizer link ${name}: ${output}${error}")
        endif()
        execute_process(COMMAND "${TEST_DIR}/${name}-sanitized" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
        if(NOT "${status}" STREQUAL "0")
            message(FATAL_ERROR "sanitizer run ${name}: ${status}\n${output}${error}")
        endif()
    endforeach()
endif()
message(STATUS "Workerless Ready: 30 positive mode executions and 12 rejected source programs")
