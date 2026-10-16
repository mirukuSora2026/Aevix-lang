# ========================
#
# Created By : miruku_sora2026
#
# File name : range_variadic_tests.cmake
#
# Description :
#     범위 경계, 가변 인수의 전달과 대여 탈출을 실제 실행으로 검사한다.
#     Check range boundaries, variadic argument passing and loan escape with actual execution.
#
# ========================
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
function(program name expected source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}")
    set(modes --run)
    if(version MATCHES "ORC JIT enabled")
        list(APPEND modes --jit)
    endif()
    foreach(mode IN LISTS modes)
        set(options)
        if(mode STREQUAL "--run")
            list(APPEND options --clang "${CLANG}")
        endif()
        execute_process(COMMAND "${AEVIXC}" "${mode}" ${options} "${TEST_DIR}/${name}.avx"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
        if(NOT "${result}" STREQUAL "${expected}")
            message(FATAL_ERROR "${name} ${mode}: expected ${expected}, got ${result}: ${output}${error}")
        endif()
    endforeach()
endfunction()
function(reject name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}")
    execute_process(COMMAND "${AEVIXC}" --check "${TEST_DIR}/${name}.avx"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
    if(result EQUAL 0 OR NOT error MATCHES "error:")
        message(FATAL_ERROR "${name}: expected a diagnostic, got ${result}: ${output}${error}")
    endif()
endfunction()
program(range_sum 45 [=[i32 main() { mut i32 sum = 0; for (i32 i in 0..10) sum += i; return sum; }]=])
program(range_closed 6 [=[i32 main() { mut i32 sum = 0; for (i32 i in 1..=3) sum += i; return sum; }]=])
program(range_descending 6 [=[i32 main() { mut i32 sum = 0; for (i32 i in Range.descending_closed(3, 1)) sum += i; return sum; }]=])
program(range_reversed_empty 0 [=[i32 main() { mut i32 count = 0; for (i32 i in 5..1) count++; for (i32 j in Range.descending(1,5)) count++; return count; }]=])
program(range_boundary_i8 4 [=[i32 main() { mut i32 count = 0; Range<i8> up = Range.closed(126,127); Range<i8> down = Range.descending_closed(-127,-128); for (i8 i in up) count++; for (i8 j in down) count++; return count; }]=])
program(range_boundary_u64 2 [=[i32 main() { mut i32 count = 0; Range<u64> edge = Range.closed(18446744073709551614,18446744073709551615); for (u64 i in edge) count++; return count; }]=])
program(range_independent 6 [=[i32 main() { mut i32 count = 0; Range<i32> source = Range.new(0,3); for (i32 i in source) count++; for (i32 j in source) count++; return count; }]=])
program(range_break_continue 8 [=[i32 main() { mut i32 sum = 0; for (i32 i in 0..10) { if (i == 2) continue; if (i == 5) break; sum += i; } return sum; }]=])
program(variadic_zero 0 [=[usize count(i32 values[]) { return Slice.len(values); } i32 main() { return count() == 0 ? 0 : 1; }]=])
program(variadic_sum 42 [=[i32 sum(i32 values[]) { mut i32 result = 0; for (mut usize i = 0; i < Slice.len(values); i++) result += values[i]; return result; } i32 main() { return sum(10,20,12); }]=])
program(variadic_fixed 42 [=[i32 pick(i32 value) { return 42; } i32 pick(i32 values[]) { return 1; } i32 main() { return pick(3); }]=])
program(variadic_generic 0 [=[usize count<T>(T values[]) { return Slice.len(values); } i32 main() { i64 typed = 3; return count(1,typed,2) == 3 && count<i32>() == 0 ? 0 : 1; }]=])
reject(range_float [=[i32 main() { for (f64 i in 0.0..3.0) { } return 0; }]=])
reject(range_binding_mismatch [=[i32 main() { for (i64 i in 0..3) { } return 0; }]=])
reject(range_binding_write [=[i32 main() { for (i32 i in 0..3) i++; return 0; }]=])
reject(variadic_escape [=[&i32 first(i32 values[]) { return &values[0]; } i32 main() { return 0; }]=])
reject(variadic_mutation [=[void change(i32 values[]) { values[0] = 2; } i32 main() { return 0; }]=])
reject(variadic_named_pack [=[void emit(i32 values[]) { return; } i32 main() { emit(values = 1); return 0; }]=])
reject(variadic_empty_inference [=[usize count<T>(T values[]) { return Slice.len(values); } i32 main() { count(); return 0; }]=])
reject(variadic_wrong_type [=[void emit(i32 values[]) { return; } i32 main() { bool value = true; emit(value); return 0; }]=])
message(STATUS "Range / variadic programs and negative checks passed")
