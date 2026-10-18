# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_task_generic_cli_tests.cmake
#
# Description :
#     구체 클로저·결과·범위가 봉인된 Task 호출로만 연결되는지 검사한다.
#     Check exact concrete closure/result/scope binding for sealed Task calls.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
function(task_generic_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(task_generic_accept name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    task_generic_run("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    task_generic_run("${name} AOT" 0 "" --run "${TEST_DIR}/${name}.avx")
    task_generic_run("${name} AOT O2" 0 "" --run -O2 "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        task_generic_run("${name} JIT" 0 "" --jit "${TEST_DIR}/${name}.avx")
        task_generic_run("${name} JIT O2" 0 "" --jit -O2 "${TEST_DIR}/${name}.avx")
    endif()
endfunction()
function(task_generic_reject name pattern source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    task_generic_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()

file(WRITE "${TEST_DIR}/schedule.avx" [=[
import task { Task, TaskScope, SpawnError };
public Result<Task<i32>,SpawnError> owned<F>(&TaskScope scope,F work) where F implements FnOnce<Tuple<>,i32> {
    return Task.spawn_owned(scope,work);
}
]=])
set(task_generic_prelude [=[
import task { Task, TaskScope, Executor, ScopeOptions, ScopeReport, SpawnError };
T success<T,E>(Result<T,E> result) {
    switch (move result) {
        case Result<T,E>.Ok { T value = value }: return value; break;
        case Result<T,E>.Err: Never.unreachable(); break;
    }
}
]=])
string(CONCAT inferred "${task_generic_prelude}" [=[
i32 main() {
    Executor executor = Executor.native_threads();
    TaskScope scope = success(Task.open_scope(&executor,ScopeOptions.defaults()));
    i32 base = 40;
    Task<i32> first = success(Task.spawn_owned(&scope,[base]() -> i32 { return base + 2; }));
    i32 found = success(Task.join(first));
    Task<i32> second = success(Task<i32>.spawn(&scope,[]() -> i32 { return 7; }));
    i32 other = success(Task<i32>.join(second));
    ScopeReport report = success(Task.close_scope(scope));
    return found == 42 && other == 7 && report.accepted == 2u64 && report.observed == 2u64 ? 0 : 1;
}
]=])
task_generic_accept(inferred_result "${inferred}")

string(CONCAT nested "import error { TaskError };\n" "${task_generic_prelude}" [=[
struct Item { public i32 value; };
i32 main() {
    Executor executor = Executor.native_threads();
    TaskScope scope = success(Task.open_scope(&executor,ScopeOptions.defaults()));
    Task<Result<i32,TaskError>> failed = success(Task.spawn_owned(&scope,[]() -> Result<i32,TaskError> { return Result.err(TaskError.Failed); }));
    Result<i32,TaskError> application = success(Task.join(failed));
    mut bool found = false;
    switch (move application) {
        case Result<i32,TaskError>.Ok: break;
        case Result<i32,TaskError>.Err { TaskError error = error }: switch (error) { case TaskError.Failed: found = true; break; default: break; } break;
    }
    Task<Result<Optional<Item>,TaskError>> ready = success(Task<Result<Optional<Item>,TaskError>>.spawn_owned(&scope,[]() -> Result<Optional<Item>,TaskError> { return Result.ok(Optional.some(Item { value = 42 })); }));
    Optional<Item> optional = success(success(Task.join(ready)));
    mut i32 result = 0;
    switch (move optional) {
        case Optional<Item>.Some { Item item = value }: result = item.value; break;
        case Optional<Item>.None: break;
    }
    ScopeReport report = success(Task.close_scope(scope));
    return found && result == 42 && report.completed == 2u64 && report.failed == 0u64 && report.observed == 2u64 ? 0 : 1;
}
]=])
task_generic_accept(nested_application_result "${nested}")

string(CONCAT imported "import schedule;\n" "${task_generic_prelude}" [=[
i32 main() {
    Executor executor = Executor.native_threads();
    TaskScope scope = success(Task.open_scope(&executor,ScopeOptions.defaults()));
    i32 input = 38;
    Task<i32> work = success(schedule.owned(&scope,[input]() -> i32 { return input + 4; }));
    i32 found = success(Task.join(work));
    ScopeReport report = success(Task.close_scope(scope));
    return found == 42 && report.accepted == 1u64 ? 0 : 1;
}
]=])
task_generic_accept(cross_module_closure "${imported}")

string(CONCAT aliases "${task_generic_prelude}" [=[
i32 main() {
    rsn source = success(Task.cancel_source());
    rsn token = success(Task.cancel_token(&source));
    rsn copy = success(Task.cancel_clone(&token));
    rsn request = success(Task.cancel_request(&source));
    return Task.cancel_requested(&token) && Task.cancel_requested(&copy) ? 0 : 1;
}
]=])
task_generic_accept(cancellation_aliases "${aliases}")

foreach(contract Fn FnMut FnOnce)
    string(CONCAT source "${task_generic_prelude}" "Result<Task<i32>,SpawnError> schedule<F>(&TaskScope scope,F work) where F implements ${contract}<Tuple<>,i32> {return Task.spawn_owned(scope,work);}
" [=[
i32 main(){Executor executor=Executor.native_threads();TaskScope scope=success(Task.open_scope(&executor,ScopeOptions.defaults()));Task<i32> work=success(schedule(&scope,[]() -> i32{return 42;}));i32 found=success(Task.join(work));ScopeReport report=success(Task.close_scope(scope));return found==42?0:1;}
]=])
    task_generic_accept("bound_${contract}" "${source}")
endforeach()
string(CONCAT unbounded "${task_generic_prelude}" [=[
void schedule<F>(&TaskScope scope,F work){rsn result=Task.spawn_owned(scope,work);return;}
i32 main(){return 0;}
]=])
task_generic_reject(unused_unbounded "Generic Task workers require.*Fn" "${unbounded}")
string(CONCAT wrong_callable "${task_generic_prelude}" [=[
void schedule<F>(&TaskScope scope,F work) where F implements FnOnce<Tuple<i32>,i32>{rsn result=Task.spawn_owned(scope,work);return;}
i32 main(){return 0;}
]=])
task_generic_reject(unused_argument_callable "Generic Task workers require.*Fn" "${wrong_callable}")
string(CONCAT ordinary_interface "${task_generic_prelude}" [=[
interface Pretend {public i32 __invoke(Self self);};
void schedule<F>(&TaskScope scope,F work) where F implements Pretend{rsn result=Task.spawn_owned(scope,work);return;}
i32 main(){return 0;}
]=])
task_generic_reject(unused_ordinary_interface "Generic Task workers require.*Fn" "${ordinary_interface}")
string(CONCAT bad_namespace "${task_generic_prelude}" [=[
i32 main(){Executor executor=Executor.native_threads();TaskScope scope=success(Task.open_scope(&executor,ScopeOptions.defaults()));rsn result=Task<i32,bool>.spawn_owned(&scope,[]() -> i32{return 42;});return 0;}
]=])
task_generic_reject(namespace_arity "exactly one result type" "${bad_namespace}")

foreach(name bad_result parameterized_worker borrowed_worker non_closure explicit_function_types)
    set(work "[]() -> i32 { return 7; }")
    set(extra "")
    set(callee "Task.spawn_owned")
    set(pattern "Task|closure|worker|result|type")
    if(name STREQUAL "bad_result")
        set(callee "Task<bool>.spawn_owned")
    elseif(name STREQUAL "parameterized_worker")
        set(work "[](i32 input) -> i32 { return input; }")
    elseif(name STREQUAL "borrowed_worker")
        set(extra "rsn callback=[]() -> i32 { return 7; };")
        set(work "&callback")
    elseif(name STREQUAL "non_closure")
        set(work "7")
    elseif(name STREQUAL "explicit_function_types")
        set(callee "Task.spawn_owned<i32>")
    endif()
    string(CONCAT source "${task_generic_prelude}" "i32 main(){Executor executor=Executor.native_threads();TaskScope scope=success(Task.open_scope(&executor,ScopeOptions.defaults()));${extra}rsn invalid=${callee}(&scope,${work});return 0;}\n")
    task_generic_reject("${name}" "${pattern}" "${source}")
endforeach()
string(CONCAT wrong_owner "${task_generic_prelude}" "i32 main(){rsn invalid=Task.join(7);return 0;}\n")
task_generic_reject(wrong_owner "Task owner|Task operation" "${wrong_owner}")

# 같은 철자의 사용자 모듈과 사용자 제네릭 타입에는 컴파일러 작업 권한이 없다.
# Same-spelled user modules and generic types do not receive compiler task authority.
set(shadow "${TEST_DIR}/shadow")
file(MAKE_DIRECTORY "${shadow}")
file(WRITE "${shadow}/Task.avx" "public i32 spawn_owned(i32 first,i32 second){return first+second;}\n")
file(WRITE "${shadow}/main.avx" "import Task;T identity<T>(T value){return value;}i32 main(){return identity(Task.spawn_owned(40,2))==42?0:1;}\n")
task_generic_run("shadow module check" 0 "" --check "${shadow}/main.avx")
task_generic_run("shadow module AOT" 0 "" --run "${shadow}/main.avx")
file(WRITE "${shadow}/nominal.avx" "struct Task<T>{public i32 spawn_owned(i32 first,i32 second){return first+second;}};T identity<T>(T value){return value;}i32 main(){return identity(Task<i32>.spawn_owned(40,2))==42?0:1;}\n")
task_generic_run("shadow nominal check" 0 "" --check "${shadow}/nominal.avx")
task_generic_run("shadow nominal AOT" 0 "" --run "${shadow}/nominal.avx")
message(STATUS "Closed Task generic binding and namespace identity passed")
