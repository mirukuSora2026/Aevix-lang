# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_system_safety_cli_tests.cmake
#
# Description :
#     시스템 내장 함수의 신뢰 경계·대여·평가 순서와 오류 뒤 입력 진행을 검사한다.
#     Check system intrinsic trust boundaries, borrowing, evaluation order, and input progress after errors.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(modes --run)
if(version MATCHES "ORC JIT enabled")
    list(APPEND modes --jit)
endif()
function(system_safe_case name source input expected_output expected_error)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    file(WRITE "${TEST_DIR}/${name}.in" "${input}")
    foreach(mode IN LISTS modes)
        execute_process(COMMAND "${AEVIXC}" "${mode}" "${TEST_DIR}/${name}.avx"
            INPUT_FILE "${TEST_DIR}/${name}.in" RESULT_VARIABLE status
            OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
        if(NOT "${status}" STREQUAL "0" OR NOT "${output}" STREQUAL "${expected_output}" OR NOT "${error}" STREQUAL "${expected_error}")
            message(FATAL_ERROR "${name} ${mode}: status ${status}\nstdout=${output}\nstderr=${error}\nexpected stdout=${expected_output}\nexpected stderr=${expected_error}")
        endif()
    endforeach()
endfunction()
function(reject_system_safe name source pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    execute_process(COMMAND "${AEVIXC}" --check "${TEST_DIR}/${name}.avx"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 45)
    if(NOT "${status}" STREQUAL "1" OR NOT "${output}${error}" MATCHES "${pattern}")
        message(FATAL_ERROR "${name}: expected rejection/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()

system_safe_case(same_named_functions [=[
import system;
i32 println(i32 value) { return value + 1; }
i32 printf(string_view format, i32 value) { return value; }
i32 main() {
    if (println(41) != 42) return 1;
    return printf("%q", 3) == 3 ? 0 : 2;
}
]=] "" "" "")

system_safe_case(evaluate_before_output [=[
import system;
i32 leaf(&mut i32 count) {
    *count += 1;
    system.print("leaf" + *count + ";");
    return *count;
}
i32 main() {
    mut i32 count = 0;
    system.println("outer=" + leaf(&mut count) + leaf(&mut count));
    system.println(1 + 2 + " apples");
    system.println("group=" + (1 + 2));
    return count == 2 ? 0 : 1;
}
]=] "" "leaf1;leaf2;outer=12\n3 apples\ngroup=3\n" "")

system_safe_case(propagate_before_write [=[
import system;
Result<i32, system.IoError> fail() { return Result.err(system.IoError.Closed); }
Result<unit, system.IoError> attempted() {
    system.println("must not appear:" + fail()?);
    return Result.ok(Unit.value());
}
i32 main() {
    system.print("before|");
    Result<unit, system.IoError> result = attempted();
    if (!Result.is_err(&result)) return 1;
    system.println("after");
    return 0;
}
]=] "" "before|after\n" "")

system_safe_case(owner_and_temporary [=[
import system;
string transfer(string value) { return value; }
Result<i32, TextError> exercise() {
    mut string owner = string.from_view("owned")?;
    system.println(owner);
    string.append(&mut owner, "!")?;
    system.println(owner);
    string temporary = string.from_view("temporary")?;
    system.println(transfer(temporary));
    return Result.ok(string.equals(&owner, "owned!") ? 0 : 1);
}
i32 main() {
    Result<i32, TextError> result = exercise();
    switch (result) {
        case Result<i32, TextError>.Ok { i32 status = value }: return status; break;
        case Result<i32, TextError>.Err: return 2; break;
    }
}
]=] "" "owned\nowned!\ntemporary\n" "")

system_safe_case(log_named_evaluation [=[
import system;
string_view message() { system.print("M"); return "message"; }
string_view code() { system.print("C"); return "E42"; }
system.LogLevel level() { system.print("L"); return system.LogLevel.error; }
i32 main() {
    system.log(code = code(), message = message(), level = level());
    return 0;
}
]=] "" "CML" "[ERROR E42] message\n")

system_safe_case(input_recovery [=[
import system;
Result<i32, system.ReadError> exercise() {
    Result<Optional<i32>, system.ReadError> rejected = system.input<i32>();
    switch (rejected) {
        case Result<Optional<i32>, system.ReadError>.Err { system.ReadError cause = error }:
            switch (cause) {
                case system.ReadError.Parse { ParseError parse = error }:
                    switch (parse) { case ParseError.InvalidSyntax: break; default: return Result.ok(1); break; }
                    break;
                case system.ReadError.Io: return Result.ok(2); break;
            }
            break;
        case Result<Optional<i32>, system.ReadError>.Ok: return Result.ok(3); break;
    }
    Optional<i32> accepted = system.input<i32>()?;
    switch (accepted) {
        case Optional<i32>.Some { i32 number = value }: if (number != 42) return Result.ok(4); break;
        case Optional<i32>.None: return Result.ok(5); break;
    }
    Optional<string> newline = system.input()?;
    switch (move newline) {
        case Optional<string>.Some { string line = value }: if (!string.is_empty(&line)) return Result.ok(6); break;
        case Optional<string>.None: return Result.ok(7); break;
    }
    Optional<string> ended = system.input()?;
    return Result.ok(Optional.is_none(&ended) ? 0 : 8);
}
i32 main() {
    Result<i32, system.ReadError> result = exercise();
    switch (result) {
        case Result<i32, system.ReadError>.Ok { i32 status = value }: return status; break;
        case Result<i32, system.ReadError>.Err: return 9; break;
    }
}
]=] "bad 42\n" "" "")

system_safe_case(nonfinite_canonical [=[
import system;
i32 main() {
    f64 nan64 = 0.0 / 0.0;
    f32 nan32 = 0.0 / 0.0;
    system.println(nan64);
    system.println(-nan64);
    system.println(nan32);
    system.println(-nan32);
    system.println(1.0 / 0.0);
    system.println(-1.0 / 0.0);
    system.println(-0.0);
    return 0;
}
]=] "" "nan\nnan\nnan\nnan\ninf\n-inf\n-0\n" "")

reject_system_safe(selective_short [=[
import system { print }; i32 main() { println("hidden"); return 0; }
]=] "Unknown|unknown|import")

reject_system_safe(selective_qualified [=[
import system { print }; i32 main() { system.println("hidden"); return 0; }
]=] "Unknown|unknown|import|member")

reject_system_safe(forged_level [=[
import system; enum LogLevel { info; }; i32 main() { system.log(LogLevel.info, "wrong"); return 0; }
]=] "requires a system.LogLevel")

reject_system_safe(reserved_runtime [=[
extern "C" i32 aevix_system_flush(u32 stream); i32 main() { return 0; }
]=] "reserved external symbol")

reject_system_safe(borrow_then_mutate [=[
import system;
i32 clear(&mut string text) { string.clear(text); return 0; }
i32 inspect(mut string owner) { system.println(owner + clear(&mut owner)); return 0; }
i32 main() { return 0; }
]=] "borrow|loan")

reject_system_safe(log_borrow_then_mutate [=[
import system;
i32 clear(&mut string text) { string.clear(text); return 0; }
i32 inspect(mut string owner) {
    system.log(code = owner, message = "changed=" + clear(&mut owner), level = system.LogLevel.warn);
    return 0;
}
i32 main() { return 0; }
]=] "borrow|loan")

# 표준 모듈 이름으로 사용자 소스가 내장 선언의 권한을 획득할 수 없다.
# User sources cannot acquire intrinsic authority by claiming a standard module name.
file(MAKE_DIRECTORY "${TEST_DIR}/collision")
file(WRITE "${TEST_DIR}/collision/system.avx" "public i32 print(i32 value) { return value; }\n")
file(WRITE "${TEST_DIR}/collision/main.avx" "import system; i32 main() { return system.print(0); }\n")
execute_process(COMMAND "${AEVIXC}" --check "${TEST_DIR}/collision/main.avx"
    RESULT_VARIABLE collision_status OUTPUT_VARIABLE collision_output ERROR_VARIABLE collision_error TIMEOUT 45)
if(NOT "${collision_status}" STREQUAL "1" OR NOT "${collision_error}" MATCHES "Ambiguous module path 'system'")
    message(FATAL_ERROR "Expected standard-module collision rejection: ${collision_status}\n${collision_output}${collision_error}")
endif()
# POSIX 파일 크기 한도로 실제 짧은 쓰기를 만들어 OS 코드와 부분 진행량을 확인한다.
# Use a POSIX file-size limit to produce a real short write and verify native code and partial progress.
if(UNIX AND DEFINED CLANG)
    file(WRITE "${TEST_DIR}/partial_output.c" [=[
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
int prepare_partial_stdout(void) {
    const char* path = getenv("AEVIX_TEST_PARTIAL_OUTPUT");
    if (!path || !freopen(path, "wb", stdout)) return 1;
    if (setvbuf(stdout, NULL, _IONBF, 0) != 0) return 2;
    struct rlimit limit;
    if (getrlimit(RLIMIT_FSIZE, &limit) != 0) return 3;
    limit.rlim_cur = 3;
    if (setrlimit(RLIMIT_FSIZE, &limit) != 0) return 4;
    if (signal(SIGXFSZ, SIG_IGN) == SIG_ERR) return 5;
    return 0;
}
]=])
    if(APPLE)
        set(partial_library "${TEST_DIR}/partial_output.dylib")
        set(partial_flags -dynamiclib -fPIC)
    else()
        set(partial_library "${TEST_DIR}/partial_output.so")
        set(partial_flags -shared -fPIC)
    endif()
    execute_process(COMMAND "${CLANG}" ${partial_flags} "${TEST_DIR}/partial_output.c" -o "${partial_library}"
        RESULT_VARIABLE partial_compile ERROR_VARIABLE partial_compile_error TIMEOUT 30)
    if(NOT partial_compile EQUAL 0)
        message(FATAL_ERROR "Cannot build partial output fixture: ${partial_compile_error}")
    endif()
    file(WRITE "${TEST_DIR}/partial_output.avx" [=[
import system;
extern "C" i32 prepare_partial_stdout();
i32 main() {
    if (prepare_partial_stdout() != 0) return 30;
    Result<unit, system.IoError> result = system.print("0123456789");
    switch (result) {
        case Result<unit, system.IoError>.Ok: return 1; break;
        case Result<unit, system.IoError>.Err { system.IoError reason = error }:
            switch (reason) {
                case system.IoError.WriteFailed { i32 native = os_code, usize accepted = progress }:
                    return native != 0 && accepted == 3 ? 0 : 2;
                    break;
                default: return 3; break;
            }
            break;
    }
}
]=])
    foreach(mode IN LISTS modes)
        set(partial_result_file "${TEST_DIR}/partial-${mode}.out")
        if(mode STREQUAL "--jit")
            set(partial_link --load "${partial_library}")
        else()
            set(partial_link --link "${partial_library}")
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E env "AEVIX_TEST_PARTIAL_OUTPUT=${partial_result_file}"
            "${AEVIXC}" "${mode}" ${partial_link} "${TEST_DIR}/partial_output.avx"
            RESULT_VARIABLE partial_status OUTPUT_VARIABLE partial_stdout ERROR_VARIABLE partial_stderr TIMEOUT 90)
        if(NOT "${partial_status}" STREQUAL "0" OR NOT "${partial_stdout}" STREQUAL "" OR NOT "${partial_stderr}" STREQUAL "")
            message(FATAL_ERROR "Partial output ${mode}: ${partial_status}\n${partial_stdout}${partial_stderr}")
        endif()
        file(READ "${partial_result_file}" partial_actual)
        if(NOT "${partial_actual}" STREQUAL "012")
            message(FATAL_ERROR "Partial output ${mode} did not retain the accepted prefix: ${partial_actual}")
        endif()
    endforeach()
    message(STATUS "System partial native output preserves OS error code and exactly three accepted bytes")
endif()
message(STATUS "System safety: seven execution fixtures and seven trust/loan rejection cases passed")
