# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_task_identity_cli_tests.cmake
#
# Description :
#     관측 소비 뒤 진단 정체성과 중첩 Task 설명자 및 불법 생성·대여를 검사한다.
#     Check diagnostic identity after observation consumption, nested Task descriptors, and illegal construction/loans.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(prelude [=[
import task { Task, TaskId, TaskScope, Executor, ScopeOptions, ScopeReport, ScopeError, TaskError };
import memory { Box };
import error { AnyError };
T success<T,E>(Result<T,E> input) {
    switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; }
}
TaskScope open() { Executor executor = Executor.native_threads(); return success(Task.open_scope(&executor,ScopeOptions.defaults())); }
Task<i32> start(&TaskScope scope) { return success(Task.spawn_owned(scope,[]() -> i32 { return 42; })); }
]=])
function(run name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(accept name body)
    file(WRITE "${TEST_DIR}/${name}.avx" "${prelude}\n${body}\n")
    run("${name} check" 0 "ownership checked" --check "${TEST_DIR}/${name}.avx")
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
accept(consumed_observation [=[
TaskId identity<T>(&Task<T> input) { return Task.id(input); }
TaskId retained() {
    TaskScope scope = open();
    mut Task<i32> work = start(&scope);
    TaskId first = identity(&work);
    success(Task.wait(&work));
    Optional<i32> result = success(Task.try_take(&mut work));
    switch (move result) { case Optional<i32>.Some { i32 value = value }: if (value != 42) Never.unreachable(); break; default: Never.unreachable(); break; }
    TaskId second = Task.id(&work);
    if (!first.equals(second)) Never.unreachable();
    Task.discard_result(work);
    success(Task.close_scope(scope));
    return second;
}
i32 main() { TaskId one = retained(); TaskId copied = one; TaskId two = retained(); return one.equals(copied) && !one.equals(two) ? 0 : 1; }
]=])
accept(owning_closure_return [=[
i32 main() {
    TaskScope scope = open();
    Task<i32> original = start(&scope);
    TaskId expected = Task.id(&original);
    rsn forward = [original]() -> Task<i32> { return original; };
    Task<i32> forwarded = forward();
    TaskId actual = Task.id(&forwarded);
    i32 result = success(Task.join(forwarded));
    ScopeReport report = success(Task.close_scope(scope));
    return expected.equals(actual) && result == 42 && report.observed == 1u64 ? 0 : 1;
}
]=])
accept(tagged_enum_roundtrip [=[
enum Held { Work { public Task<i32> owner; }; };
i32 main() {
    TaskScope scope = open();
    Task<i32> original = start(&scope);
    TaskId expected = Task.id(&original);
    Held wrapped = Held.Work { owner = original };
    mut i32 result = 0;
    switch (move wrapped) {
        case Held.Work { Task<i32> unwrapped = owner }:
            TaskId actual = Task.id(&unwrapped);
            if (!expected.equals(actual)) return 1;
            result = success(Task.join(unwrapped));
            break;
    }
    ScopeReport report = success(Task.close_scope(scope));
    return result == 42 && report.observed == 1u64 ? 0 : 2;
}
]=])
accept(nested_tuple_vector_drop [=[
i32 main() {
    TaskScope scope = open();
    {
        Tuple<bool,Task<i32>,i32> tuple = Tuple.make(true,start(&scope),9);
        Result<Vector<Task<i32>>,CollectionError> empty = [];
        mut Vector<Task<i32>> owners = success(empty);
        success(Vector.push(&mut owners,start(&scope)));
        success(Vector.push(&mut owners,start(&scope)));
        success(Vector.push(&mut owners,start(&scope)));
        Box<Task<i32>> boxed = success(Box.new(start(&scope)));
    }
    ScopeReport report = success(Task.close_scope(scope));
    return report.accepted == 5u64 && report.discarded == 5u64 ? 0 : 1;
}
]=])
accept(owning_struct_drop [=[
struct Holder { public bool before; public Task<i32> owner; public u64 after; };
i32 main() {
    TaskScope scope = open();
    {
        Task<i32> original = start(&scope);
        TaskId expected = Task.id(&original);
        Holder wrapped = Holder { before = true, owner = original, after = 99u64 };
        TaskId actual = Task.id(&wrapped.owner);
        if (!expected.equals(actual) || !wrapped.before || wrapped.after != 99u64) return 1;
    }
    ScopeReport report = success(Task.close_scope(scope));
    return report.accepted == 1u64 && report.discarded == 1u64 ? 0 : 2;
}
]=])
accept(qualified_and_explicit_generic [=[
TaskId identify<T>(&Task<T> work) { return Task<T>.id(work); }
i32 main() {
    TaskScope scope = open();
    Task<i32> work = start(&scope);
    TaskId first = task.Task.id(&work);
    TaskId second = identify<i32>(&work);
    i32 result = success(Task.join(work));
    success(Task.close_scope(scope));
    return TaskId.equals(&first,second) && result == 42 ? 0 : 1;
}
]=])
set(moved "moved|unavailable|initialized|move")
set(borrowed "borrow|Borrow|reference|Reference|scope|Scope|loan|Loan")
set(private "private|Private|cannot be constructed|Compiler-owned")
reject(no_empty_id [=[i32 main() { TaskId forged = TaskId {}; return 0; }]=] "${private}")
reject(no_numeric_id_field [=[i32 main() { TaskId forged = TaskId { serial = 1u64 }; return 0; }]=] "${private}")
reject(no_numeric_id_cast [=[i32 main() { TaskId forged = 1u64; return 0; }]=] "requires|received|type|Type|convert|compatible")
reject(no_private_id_read [=[i32 main() { TaskScope scope = open(); Task<i32> work = start(&scope); TaskId found = Task.id(&work); u64 stolen = found.serial; return 0; }]=] "private|Private")
reject(no_id_as_u64 [=[i32 main() { TaskScope scope = open(); Task<i32> work = start(&scope); u64 stolen = Task.id(&work); return 0; }]=] "requires|received|type|Type|convert|compatible")
reject(id_needs_borrow [=[i32 main() { TaskScope scope = open(); Task<i32> work = start(&scope); Task.id(work); return 0; }]=] "${borrowed}|owner")
reject(id_rejects_scope [=[i32 main() { TaskScope scope = open(); Task.id(&scope); return 0; }]=] "Task|owner")
reject(id_rejects_wrong_payload [=[i32 main() { TaskScope scope = open(); Task<i32> work = start(&scope); Task<i64>.id(&work); return 0; }]=] "result|type|Type")
reject(id_rejects_function_types [=[i32 main() { TaskScope scope = open(); Task<i32> work = start(&scope); Task.id<i32>(&work); return 0; }]=] "infer|type|Type")
reject(id_rejects_moved_task [=[i32 main() { TaskScope scope = open(); Task<i32> original = start(&scope); Task<i32> moved = original; Task.id(&original); return 0; }]=] "${moved}")
reject(id_keeps_original_scope [=[i32 main() { TaskScope scope = open(); Task<i32> original = start(&scope); TaskId copy = Task.id(&original); Task.close_scope(scope); return 0; }]=] "${borrowed}")
reject(closure_keeps_original_scope [=[i32 main() { TaskScope scope = open(); Task<i32> original = start(&scope); rsn unused = [original]() -> i32 { return 7; }; Task.close_scope(scope); return 0; }]=] "${borrowed}")
reject(id_no_direct_foreign [=[extern "C" i32 inspect(TaskId identity); i32 main() { return 0; }]=] "FFI|ABI|extern|foreign|C function")
reject(id_no_foreign_return [=[extern "C" TaskId create(); i32 main() { return 0; }]=] "FFI|ABI|extern|foreign|C function")
reject(id_no_foreign_nested [=[struct Holder { public TaskId identity; }; extern "C" i32 inspect(Holder input); i32 main() { return 0; }]=] "FFI|ABI|extern|foreign|C function")
reject(task_no_empty_descriptor [=[i32 main() { Task<i32> forged = Task<i32> {}; return 0; }]=] "${private}")
reject(task_no_metadata_descriptor [=[i32 main() { Task<i32> forged = Task<i32> { result_type = 1 }; return 0; }]=] "${private}")
reject(no_global_erased_scope [=[
AnyError create_error() { ScopeError pending = ScopeError.Retry { scope = open(), reason = TaskError.Cancelled }; return success(AnyError.pack(pending)); }
persist AnyError stored = create_error();
i32 main() { return 0; }
]=] "global|persist|owned-worker")
if(DEFINED CLANG AND EXISTS "${CLANG}" AND DEFINED RUNTIME AND EXISTS "${RUNTIME}" AND NOT WIN32)
    set(extra_libraries -pthread)
    if(APPLE)
        list(APPEND extra_libraries -lc++)
    else()
        list(APPEND extra_libraries -lstdc++)
    endif()
    foreach(name consumed_observation owning_closure_return tagged_enum_roundtrip nested_tuple_vector_drop owning_struct_drop qualified_and_explicit_generic)
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
message(STATUS "Task identity: 30 positive mode executions, 18 rejection programs")
