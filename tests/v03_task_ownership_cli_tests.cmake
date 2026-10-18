# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_task_ownership_cli_tests.cmake
#
# Description :
#     스칼라 결과의 작업도 보관하는 스코프 대여와 소유 작업의 스레드 경계를 검사한다.
#     Check retained Scope loans even for scalar-result tasks and owned-worker thread boundaries.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(header [=[
import task { Task, TaskScope, Executor, ScopeOptions, ScopeError, TaskError };
import memory { Box };
import sync { Mutex, MutexGuard };
import error { AnyError, Error };
T success<T, E>(Result<T, E> input) {
    switch (move input) { case Result<T, E>.Ok { T value = value }: return value; break; default: Never.unreachable(); break; }
}
TaskScope open() { Executor executor = Executor.native_threads(); return success(Task.open_scope(&executor, ScopeOptions.defaults())); }
Task<i32> start(&TaskScope scope) { return success(Task.spawn_owned(scope, []() -> i32 { return 7; })); }
Task<i32> identity(Task<i32> input) { return input; }
]=])
function(task_ownership_run name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 120)
    if(NOT "${status}" STREQUAL "${expected}" OR
       (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(task_ownership_accept name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${header}\n${source}\n")
    task_ownership_run("${name} check" 0 "ownership checked" --check "${TEST_DIR}/${name}.avx")
    foreach(opt -O0 -O2)
        task_ownership_run("${name} AOT ${opt}" 0 "" --run "${opt}" "${TEST_DIR}/${name}.avx")
        if(version MATCHES "ORC JIT enabled")
            task_ownership_run("${name} JIT ${opt}" 0 "" --jit "${opt}" "${TEST_DIR}/${name}.avx")
        endif()
    endforeach()
endfunction()
function(task_ownership_reject name pattern source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${header}\n${source}\n")
    task_ownership_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()
task_ownership_accept(helper_join [=[
i32 main() { TaskScope scope = open(); Task<i32> worker = identity(start(&scope)); i32 result = success(Task.join(worker)); success(Task.close_scope(scope)); return result == 7 ? 0 : 1; }
]=])
task_ownership_accept(optional_join [=[
i32 main() {
    TaskScope scope = open(); Optional<Task<i32>> wrapped = Optional.some(identity(start(&scope)));
    switch (move wrapped) {
        case Optional<Task<i32>>.Some { Task<i32> worker = value }: if (success(Task.join(worker)) != 7) return 1; break;
        case Optional<Task<i32>>.None: return 2; break;
    }
    success(Task.close_scope(scope)); return 0;
}
]=])
task_ownership_accept(box_discard [=[
i32 main() { TaskScope scope = open(); { Box<Task<i32>> owner = success(Box.new(start(&scope))); } success(Task.close_scope(scope)); return 0; }
]=])
task_ownership_accept(tuple_drop [=[
i32 main() { TaskScope scope = open(); { Tuple<Task<i32>, i32> holder = Tuple.make(identity(start(&scope)), 9); } success(Task.close_scope(scope)); return 0; }
]=])
task_ownership_accept(vector_drop [=[
i32 main() {
    TaskScope scope = open();
    { Result<Vector<Task<i32>>, CollectionError> made = []; mut Vector<Task<i32>> owners = success(made); success(Vector.push(&mut owners, start(&scope))); }
    success(Task.close_scope(scope)); return 0;
}
]=])
task_ownership_accept(local_erased_scope_cleanup [=[
i32 main() {
    ScopeError pending = ScopeError.Retry { scope = open(), reason = TaskError.Cancelled };
    AnyError erased = success(AnyError.pack(pending));
    return string.is_empty(erased.code()) ? 1 : 0;
}
]=])
task_ownership_reject(no_global_erased_scope "global|persist|owned-worker" [=[
AnyError create_error() {
    ScopeError pending = ScopeError.Retry { scope = open(), reason = TaskError.Cancelled };
    return success(AnyError.pack(pending));
}
persist AnyError stored = create_error();
i32 main() { return 0; }
]=])
task_ownership_reject(no_global_boxed_erased_scope "global|persist|owned-worker" [=[
Box<dyn Error> create_error() {
    ScopeError pending = ScopeError.Retry { scope = open(), reason = TaskError.Cancelled };
    Box<ScopeError> concrete = success(Box.new(pending));
    return Box.into_dyn(concrete);
}
persist Box<dyn Error> stored = create_error();
i32 main() { return 0; }
]=])
task_ownership_reject(local_scope_escape "reference.*local|outlive|escape|scope.*borrow" [=[
Task<i32> escape() { TaskScope scope = open(); return identity(start(&scope)); }
i32 main() { return 0; }
]=])
task_ownership_reject(wrapped_scope_escape "reference.*local|outlive|escape|scope.*borrow" [=[
Optional<Task<i32>> escape() { TaskScope scope = open(); return Optional.some(identity(start(&scope))); }
i32 main() { return 0; }
]=])
task_ownership_reject(unused_handle_keeps_scope "Cannot modify, move, or exclusively borrow|borrowed" [=[
i32 main() { TaskScope scope = open(); Task<i32> unused = start(&scope); success(Task.close_scope(scope)); return 0; }
]=])
task_ownership_reject(box_keeps_scope "Cannot modify, move, or exclusively borrow|borrowed" [=[
i32 main() { TaskScope scope = open(); Box<Task<i32>> unused = success(Box.new(identity(start(&scope)))); success(Task.close_scope(scope)); return 0; }
]=])
task_ownership_reject(tuple_keeps_scope "Cannot modify, move, or exclusively borrow|borrowed" [=[
i32 main() { TaskScope scope = open(); Tuple<Task<i32>, i32> unused = Tuple.make(identity(start(&scope)), 0); success(Task.close_scope(scope)); return 0; }
]=])
task_ownership_reject(vector_keeps_scope "Cannot modify, move, or exclusively borrow|borrowed" [=[
i32 main() { TaskScope scope = open(); Result<Vector<Task<i32>>, CollectionError> made = []; mut Vector<Task<i32>> owners = success(made); success(Vector.push(&mut owners, start(&scope))); success(Task.close_scope(scope)); return 0; }
]=])
task_ownership_reject(task_move_only "moved|unavailable|initialized" [=[
i32 main() { TaskScope scope = open(); Task<i32> first = start(&scope); Task<i32> second = identity(first); Task.status(&first); success(Task.join(second)); return 0; }
]=])
task_ownership_reject(scope_move_only "moved|unavailable|initialized" [=[
i32 main() { TaskScope first = open(); TaskScope second = first; Task<i32> worker = start(&first); success(Task.join(worker)); success(Task.close_scope(second)); return 0; }
]=])
task_ownership_reject(no_borrowed_capture "borrowed views|compiler-derived Send|Worker.*captur" [=[
i32 main() { TaskScope scope = open(); i32 value = 7; Task<i32> worker = success(Task.spawn_owned(&scope, [&value]() -> i32 { return value; })); return success(Task.join(worker)); }
]=])
task_ownership_reject(no_borrowed_result "borrowed views|compiler-derived Send|Worker.*result" [=[
i32 main() { TaskScope scope = open(); Task<string_view> worker = success(Task.spawn_owned(&scope, []() -> string_view { return "static"; })); success(Task.join(worker)); return 0; }
]=])
task_ownership_reject(no_unknown_worker_ffi "unapproved transitive effect|external or unresolved|worker-effect contract" [=[
extern "C" i32 unknown_effect();
i32 main() { TaskScope scope = open(); Task<i32> worker = success(Task.spawn_owned(&scope, []() -> i32 { return unknown_effect(); })); return success(Task.join(worker)); }
]=])
task_ownership_reject(no_hidden_drop_effect "compiler-derived Send|unapproved transitive effect|worker.*cleanup" [=[
extern "C" i32 unknown_effect();
struct Capture implements Drop { public i32 value; public void drop(&mut Self self) { unknown_effect(); return; } };
i32 main() { TaskScope scope = open(); Capture captured = Capture { value = 7 }; Task<i32> worker = success(Task.spawn_owned(&scope, [captured]() -> i32 { return captured.value; })); return success(Task.join(worker)); }
]=])
task_ownership_reject(no_user_send_bypass "compiler-derived Send|Worker.*captur|borrowed views" [=[
interface Send {};
struct Pretend implements Send { public MutexGuard<i32> guard; };
i32 main() { Mutex<i32> owner = Mutex.new(7); Pretend captured = Pretend { guard = success(Mutex.lock(&owner)) }; TaskScope scope = open(); Task<i32> worker = success(Task.spawn_owned(&scope, [captured]() -> i32 { return 7; })); return success(Task.join(worker)); }
]=])
message(STATUS "Task Scope loans survive helpers, wrappers, unused handles and Drop; owned-worker boundaries reject unsafe transfers")
