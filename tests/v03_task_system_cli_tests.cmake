# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_task_system_cli_tests.cmake
#
# Description :
#     작업 스레드의 표준 입출력·로그 잠금과 소유 입력 결과를 실제 실행으로 검사한다.
#     Check serialized worker IO, logging, and owned input results through actual execution.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(header [=[
import system;
import task { Task, TaskScope, Executor, ScopeOptions };
T success<T,E>(Result<T,E> input) {
    switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; default: Never.unreachable(); break; }
}
TaskScope open() { Executor executor = Executor.native_threads(); return success(Task.open_scope(&executor, ScopeOptions.defaults())); }
]=])
function(task_system_fixture name source input)
    file(WRITE "${TEST_DIR}/${name}.avx" "${header}\n${source}\n")
    file(WRITE "${TEST_DIR}/${name}.in" "${input}")
    execute_process(COMMAND "${AEVIXC}" --check "${TEST_DIR}/${name}.avx"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "0")
        message(FATAL_ERROR "${name} check: ${status}\n${output}${error}")
    endif()
    set(modes --run)
    if(version MATCHES "ORC JIT enabled")
        list(APPEND modes --jit)
    endif()
    foreach(mode IN LISTS modes)
        foreach(opt -O0 -O2)
            execute_process(COMMAND "${AEVIXC}" "${mode}" "${opt}" "${TEST_DIR}/${name}.avx"
                INPUT_FILE "${TEST_DIR}/${name}.in" RESULT_VARIABLE status
                OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
            if(NOT "${status}" STREQUAL "0")
                message(FATAL_ERROR "${name} ${mode} ${opt}: ${status}\n${output}${error}")
            endif()
            if(name STREQUAL "concurrent_output")
                set(expected_lines "")
                set(expected_log "")
                foreach(worker 1 2)
                    foreach(index RANGE 0 31)
                        list(APPEND expected_lines "worker=${worker}:${index}")
                        list(APPEND expected_log "[INFO task] worker=${worker}:${index}")
                    endforeach()
                endforeach()
                string(REPLACE "\n" ";" lines "${output}")
                string(REPLACE "\n" ";" logs "${error}")
                list(FILTER lines EXCLUDE REGEX "^$")
                list(FILTER logs EXCLUDE REGEX "^$")
                list(SORT lines)
                list(SORT logs)
                list(SORT expected_lines)
                list(SORT expected_log)
                if(NOT "${lines}" STREQUAL "${expected_lines}" OR NOT "${logs}" STREQUAL "${expected_log}")
                    message(FATAL_ERROR "${name} ${mode} ${opt}: lost, duplicated, or interleaved output\n${output}${error}")
                endif()
            elseif(NOT "${output}${error}" STREQUAL "")
                message(FATAL_ERROR "${name} ${mode} ${opt}: unexpected output\n${output}${error}")
            endif()
        endforeach()
    endforeach()
endfunction()
task_system_fixture(concurrent_output [=[
i32 write_worker(i32 worker) {
    for (mut i32 index = 0; index < 32; index++) {
        success(system.print(""));
        if (index % 2 == 0) success(system.println("worker=" + worker + ":" + index));
        else success(system.printf("worker=%d:%d\n", worker, index));
        success(system.log(system.info, "worker=" + worker + ":" + index, "task"));
    }
    success(system.flush()); return 32;
}
i32 main() {
    TaskScope scope = open();
    Task<i32> first = success(Task.spawn_owned(&scope, []() -> i32 { return write_worker(1); }));
    Task<i32> second = success(Task.spawn_owned(&scope, []() -> i32 { return write_worker(2); }));
    i32 left = success(Task.join(first)); i32 right = success(Task.join(second));
    success(Task.close_scope(scope)); return left + right == 64 ? 0 : 1;
}
]=] "")
task_system_fixture(concurrent_tokens [=[
i32 read_number() {
    Optional<i32> result = success(system.input<i32>());
    switch (result) { case Optional<i32>.Some { i32 value = value }: return value; break; default: return -100; break; }
}
i32 main() {
    TaskScope scope = open();
    Task<i32> first = success(Task.spawn_owned(&scope, []() -> i32 { return read_number(); }));
    Task<i32> second = success(Task.spawn_owned(&scope, []() -> i32 { return read_number(); }));
    i32 left = success(Task.join(first)); i32 right = success(Task.join(second));
    success(Task.close_scope(scope)); return left + right == 30 && left != right ? 0 : 1;
}
]=] "10 20\n")
task_system_fixture(owned_line [=[
i32 main() {
    TaskScope scope = open();
    Task<Result<Optional<string>,system.ReadError>> work = success(Task.spawn_owned(&scope,
        []() -> Result<Optional<string>,system.ReadError> { return system.input<string>(); }));
    Optional<string> result = success(success(Task.join(work)));
    switch (move result) {
        case Optional<string>.Some { string text = value }: if (!string.equals(&text, "he ll o")) return 1; break;
        default: return 2; break;
    }
    success(Task.close_scope(scope)); return 0;
}
]=] "he ll o\n")
task_system_fixture(input_errors [=[
i32 main() {
    TaskScope scope = open();
    Task<Result<Optional<i32>,system.ReadError>> work = success(Task.spawn_owned(&scope,
        []() -> Result<Optional<i32>,system.ReadError> { return system.input<i32>(); }));
    Result<Optional<i32>,system.ReadError> result = success(Task.join(work));
    switch (move result) {
        case Result<Optional<i32>,system.ReadError>.Err { system.ReadError cause = error }:
            switch (cause) { case system.ReadError.Parse: break; default: return 1; break; } break;
        default: return 2; break;
    }
    success(Task.close_scope(scope)); return 0;
}
]=] "bad\n")
message(STATUS "Worker IO: four fixtures in check/AOT O0/O2/JIT O0/O2; exact concurrent output sets and owned input")
