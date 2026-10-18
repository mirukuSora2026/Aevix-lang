# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_task_resources_cli_tests.cmake
#
# Description :
#     작업자의 동기 시간·파일 호출과 자원 이동·오류 전달을 실제 실행으로 검사한다.
#     Check synchronous worker time/file calls, resource transfer, and error propagation through execution.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(header [=[
import task { Task, TaskScope, Executor, ScopeOptions };
import io { File, FileError, IoError };
import time;
T success<T,E>(Result<T,E> input) {
    switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; default: Never.unreachable(); break; }
}
TaskScope open() { Executor executor = Executor.native_threads(); return success(Task.open_scope(&executor,ScopeOptions.defaults())); }
]=])
function(resource_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(resource_accept name source)
    string(REPLACE "@INPUT@" "${TEST_DIR}/input.bin" source "${source}")
    string(REPLACE "@OUTPUT@" "${TEST_DIR}/output.bin" source "${source}")
    string(REPLACE "@MISSING@" "${TEST_DIR}/missing.bin" source "${source}")
    file(WRITE "${TEST_DIR}/${name}.avx" "${header}\n${source}\n")
    resource_run("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    set(modes --run)
    if(version MATCHES "ORC JIT enabled")
        list(APPEND modes --jit)
    endif()
    foreach(mode IN LISTS modes)
        foreach(opt -O0 -O2)
            file(WRITE "${TEST_DIR}/input.bin" "ABC")
            file(REMOVE "${TEST_DIR}/output.bin" "${TEST_DIR}/missing.bin")
            resource_run("${name} ${mode} ${opt}" 0 "" "${mode}" "${opt}" "${TEST_DIR}/${name}.avx")
            if(name STREQUAL "worker_io_and_owned_result")
                file(READ "${TEST_DIR}/output.bin" content HEX)
                if(NOT content STREQUAL "4142")
                    message(FATAL_ERROR "${name}: expected actual two-byte file 4142, got ${content}")
                endif()
            endif()
        endforeach()
    endforeach()
endfunction()
function(resource_reject name imports source pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${imports}\n${header}\n${source}\n")
    resource_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()

# 이 sleep은 네이티브 작업 스레드를 막는다. async 중단·재개 기능을 검사하지 않는다.
# This sleep blocks the native worker; it does not test async suspension/resumption.
resource_accept(blocking_time [=[
Tuple<u64,i64> clock_worker() {
    u64 before = success(time.monotonic_ns());
    success(time.sleep_ns(0u64));
    success(time.sleep(time.Duration.from_nanos(1000000u64)));
    u64 after = success(time.monotonic_ns());
    i64 wall = success(time.unix_time_ns());
    return Tuple.make(after >= before ? after - before : 0u64, wall);
}
i32 main() {
    TaskScope scope = open();
    Task<Tuple<u64,i64>> work = success(Task.spawn_owned(&scope, []() -> Tuple<u64,i64> { return clock_worker(); }));
    Tuple<u64,i64> result = success(Task.join(work));
    success(Task.close_scope(scope));
    return *Tuple.get<0>(&result) >= 1000000u64 && *Tuple.get<1>(&result) > 0i64 ? 0 : 1;
}
]=])

resource_accept(owned_file_capture [=[
i32 main() {
    TaskScope scope = open();
    File input = success(File.open_read("@INPUT@"));
    Task<usize> work = success(Task.spawn_owned(&scope, [input]() -> usize { return success(input.length()); }));
    usize result = success(Task.join(work));
    success(Task.close_scope(scope));
    return result == 3usize ? 0 : 1;
}
]=])

resource_accept(worker_io_and_owned_result [=[
File prepare(string_view path) {
    mut File output = success(File.open(path,15u32));
    byte bytes[3] = [Byte.from_u8(65u8),Byte.from_u8(66u8),Byte.from_u8(67u8)];
    success(output.write_all(Slice.from_array(&bytes)));
    if (success(output.position()) != 3usize || success(output.length()) != 3usize) Never.unreachable();
    success(output.seek(0));
    mut byte copied[3] = [Byte.from_u8(0u8),Byte.from_u8(0u8),Byte.from_u8(0u8)];
    success(output.read_exact(Slice.from_array_mut(&mut copied)));
    if (copied[0] != bytes[0] || copied[1] != bytes[1] || copied[2] != bytes[2]) Never.unreachable();
    success(output.set_len(2));
    success(output.flush());
    success(output.close());
    success(output.close());
    return success(File.open_read_write(path));
}
i32 main() {
    TaskScope scope = open();
    string path = success(string.from_view("@OUTPUT@"));
    Task<File> work = success(Task.spawn_owned(&scope, [path]() -> File { return prepare(string.view(&path)); }));
    mut File result = success(Task.join(work));
    success(Task.close_scope(scope));
    usize length = success(result.length());
    success(result.close());
    return length == 2usize && !result.is_open() ? 0 : 1;
}
]=])

resource_accept(worker_file_error [=[
i32 main() {
    TaskScope scope = open();
    Task<Result<File,FileError>> work = success(Task.spawn_owned(&scope, []() -> Result<File,FileError> { return File.open_read("@MISSING@"); }));
    Result<File,FileError> result = success(Task.join(work));
    success(Task.close_scope(scope));
    switch (move result) {
        case Result<File,FileError>.Ok: return 1; break;
        case Result<File,FileError>.Err { FileError error = error }:
            switch (error) { case FileError.NotFound { i32 native = os_code }: return native != 0 ? 0 : 2; break; default: return 3; break; }
            break;
    }
}
]=])

resource_accept(worker_partial_read_error [=[
Result<unit,IoError> read_worker() {
    mut File input = success(File.open_read("@INPUT@"));
    mut byte output[5] = [Byte.from_u8(0u8),Byte.from_u8(0u8),Byte.from_u8(0u8),Byte.from_u8(0u8),Byte.from_u8(0u8)];
    return input.read_exact(Slice.from_array_mut(&mut output));
}
i32 main() {
    TaskScope scope = open();
    Task<Result<unit,IoError>> work = success(Task.spawn_owned(&scope, []() -> Result<unit,IoError> { return read_worker(); }));
    Result<unit,IoError> result = success(Task.join(work));
    success(Task.close_scope(scope));
    switch (move result) {
        case Result<unit,IoError>.Ok: return 1; break;
        case Result<unit,IoError>.Err { IoError error = error }:
            switch (error) { case IoError.ReadFailed { i32 native = os_code, usize completed = progress }: return native == 0 && completed == 3usize ? 0 : 2; break; default: return 3; break; }
            break;
    }
}
]=])

set(effect "native OS operation has no admitted worker-effect contract")
resource_reject(environment_not_admitted "import env;" [=[i32 main() { TaskScope scope = open(); rsn invalid = Task.spawn_owned(&scope, []() -> bool { return success(env.contains("AEVIX_TASK_TEST")); }); return 0; }]=] "${effect}")
resource_reject(process_not_admitted "import process;" [=[i32 main() { TaskScope scope = open(); rsn invalid = Task.spawn_owned(&scope, []() -> i32 { string_view arguments[1] = ["ignored"]; return success(process.run(Slice.from_array(&arguments))).code; }); return 0; }]=] "${effect}|unapproved transitive effect")
resource_reject(path_not_admitted "import path;" [=[i32 main() { TaskScope scope = open(); rsn invalid = Task.spawn_owned(&scope, []() -> string { return success(path.normalize("a/../b")); }); return 0; }]=] "${effect}")
resource_reject(fs_not_admitted "import fs;" [=[i32 main() { TaskScope scope = open(); rsn invalid = Task.spawn_owned(&scope, []() -> bool { return success(fs.exists("ignored")); }); return 0; }]=] "${effect}")
resource_reject(unknown_ffi_not_admitted "" [=[extern "C" i32 unknown_os_worker(); i32 main() { TaskScope scope = open(); rsn invalid = Task.spawn_owned(&scope, []() -> i32 { return unknown_os_worker(); }); return 0; }]=] "external or unresolved implementations")
resource_reject(file_capture_moves_owner "" [=[i32 main() { TaskScope scope = open(); File input = success(File.open_read("ignored")); rsn work = Task.spawn_owned(&scope, [input]() -> usize { return success(input.length()); }); input.length(); return 0; }]=] "moved|unavailable")
resource_reject(file_borrow_capture_rejected "" [=[i32 main() { TaskScope scope = open(); File input = success(File.open_read("ignored")); rsn work = Task.spawn_owned(&scope, [&input]() -> usize { return success(input.length()); }); return 0; }]=] "borrowed views|captures and results")
message(STATUS "Closed synchronous worker time/File calls, owned handles and IO failures passed")
