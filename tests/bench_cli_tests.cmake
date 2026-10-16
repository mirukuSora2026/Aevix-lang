# ========================
#
# Created By : miruku_sora2026
#
# File name : bench_cli_tests.cmake
#
# Description :
#     실제 AOT와 JIT 실행으로 함수 주석의 계측·오류 전파·자동 보고를 검사한다.
#     Check annotation instrumentation, error propagation, and automatic reports through real AOT and JIT execution.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR OR NOT DEFINED CLANG)
    message(FATAL_ERROR "AEVIXC, TEST_DIR and CLANG are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version RESULT_VARIABLE version_result)
if(NOT version_result EQUAL 0)
    message(FATAL_ERROR "Cannot inspect compiler version")
endif()
set(modes --run)
if(version MATCHES "ORC JIT enabled")
    list(APPEND modes --jit)
endif()
set(measured [=[
@bench i32 leaf(bool early) { if (early) return 7; return 8; }
@bench void touch() { return; }
@bench T identity<T>(T value) { return value; }
@bench i32 recursive(i32 depth) { if (depth == 0) return 0; return recursive(depth - 1) + 1; }
@bench Result<i8, IntegerError> narrow(i64 value) { return Result.ok(Integer.convert<i8>(value)?); }
@bench i32 main() {
    touch();
    i32 first = leaf(true);
    i32 second = leaf(false);
    i32 generic = identity(9);
    i32 recursion = recursive(3);
    rsn overflow = narrow(300i64);
    if (first != 7 || second != 8 || generic != 9 || recursion != 3 || Result.is_ok(&overflow)) return 1;
    return 0;
}
]=])
file(WRITE "${TEST_DIR}/measured.avx" "${measured}")
file(WRITE "${TEST_DIR}/silent.avx" "i32 helper(i32 input) { return input + 1; } i32 main() { return helper(1) == 2 ? 0 : 1; }")
foreach(mode IN LISTS modes)
    set(options)
    if(mode STREQUAL "--run")
        list(APPEND options --clang "${CLANG}")
    endif()
    execute_process(COMMAND "${AEVIXC}" "${mode}" ${options} "${TEST_DIR}/measured.avx"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "@bench ${mode} failed (${result}): ${output}${error}")
    endif()
    foreach(expected
        "Aevix @bench: wall time"
        "sampled process RSS [(]not per-function allocations[)]"
        "function=\"leaf#[0-9]+\" calls=2"
        "function=\"touch#[0-9]+\" calls=1"
        "function=\"identity[^\"]*\" calls=1"
        "function=\"recursive#[0-9]+\" calls=4"
        "function=\"narrow#[0-9]+\" calls=1"
        "function=\"main#[0-9]+\" calls=1"
        "time_total_ms=[0-9]+[.][0-9]+"
        "time_mean_ms=[0-9]+[.][0-9]+"
        "time_max_ms=[0-9]+[.][0-9]+"
        "memory_peak_mib=([0-9]+[.][0-9]+|unavailable)"
        "memory_mean_mib=([0-9]+[.][0-9]+|unavailable)"
        "samples=[0-9]+")
        if(NOT error MATCHES "${expected}")
            message(FATAL_ERROR "@bench ${mode} lacks ${expected}: ${error}")
        endif()
    endforeach()
    if(error MATCHES "(/Users/|/home/|[A-Za-z]:\\\\Users\\\\)")
        message(FATAL_ERROR "@bench report must use logical function labels rather than home paths: ${error}")
    endif()
    if(error MATCHES "active_calls=[1-9]")
        message(FATAL_ERROR "@bench missed an early/propagating return: ${error}")
    endif()
    execute_process(COMMAND "${AEVIXC}" "${mode}" ${options} "${TEST_DIR}/silent.avx"
        RESULT_VARIABLE silent_result OUTPUT_VARIABLE silent_output ERROR_VARIABLE silent_error TIMEOUT 30)
    if(NOT silent_result EQUAL 0 OR silent_error MATCHES "Aevix @bench")
        message(FATAL_ERROR "Unannotated ${mode} unexpectedly profiles: ${silent_result} ${silent_output}${silent_error}")
    endif()
endforeach()
execute_process(COMMAND "${AEVIXC}" --check "${TEST_DIR}/measured.avx"
    RESULT_VARIABLE check_result OUTPUT_VARIABLE check_output ERROR_VARIABLE check_error TIMEOUT 15)
if(NOT check_result EQUAL 0 OR check_error MATCHES "Aevix @bench")
    message(FATAL_ERROR "@bench must not execute while checking: ${check_output}${check_error}")
endif()
execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/measured.avx"
    RESULT_VARIABLE llvm_result OUTPUT_VARIABLE llvm ERROR_VARIABLE llvm_error TIMEOUT 15)
if(NOT llvm_result EQUAL 0 OR NOT llvm MATCHES "call i64 @aevix_bench_begin" OR NOT llvm MATCHES "call void @aevix_bench_end")
    message(FATAL_ERROR "@bench LLVM instrumentation missing: ${llvm_error}")
endif()
execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/silent.avx"
    RESULT_VARIABLE silent_llvm_result OUTPUT_VARIABLE silent_llvm ERROR_VARIABLE silent_llvm_error TIMEOUT 15)
if(NOT silent_llvm_result EQUAL 0 OR silent_llvm MATCHES "aevix_bench_")
    message(FATAL_ERROR "Unannotated LLVM must not reference profiler hooks: ${silent_llvm_error}")
endif()
message(STATUS "@bench AOT/JIT instrumentation and silent unannotated execution passed")
