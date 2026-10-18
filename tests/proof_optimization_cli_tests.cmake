# ========================
#
# Created By : miruku_sora2026
#
# File name : proof_optimization_cli_tests.cmake
#
# Description :
#     증명 패스의 기본 비활성 경로·검사 제거·실패 경로를 실제 AOT/JIT 실행으로 대조한다.
#     Compare disabled emission, proven check removal, and failure paths through real AOT/JIT execution.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
file(WRITE "${TEST_DIR}/bounded.avx" [==[
i32 bounded(u32 input) {
    u32 value = input % 100u32;
    u32 result = value * 3u32 + 1u32;
    return result >= 1u32 && result <= 298u32 ? 0 : 1;
}
i32 main() {
    mut i32 values[16] = [0; 16];
    for (mut usize index = 0usize; index < 16usize; index++) {
        values[index] = 7;
    }
    if (values[15usize] != 7) return 2;
    return bounded(29u32);
}
]==])
file(WRITE "${TEST_DIR}/overflow.avx" [==[
u8 increase(u8 input) { return input + 1u8; }
i32 main() { u8 value = increase(255u8); return value == 0u8 ? 0 : 3; }
]==])
file(WRITE "${TEST_DIR}/bounds.avx" [==[
i32 get(usize index) { i32 values[4] = [1, 2, 3, 4]; return values[index]; }
i32 main() { return get(4usize); }
]==])
file(WRITE "${TEST_DIR}/loop_overflow.avx" [==[
i32 main() {
    for (mut u8 index = 250u8; index <= 255u8; index++) {
        if (index < 250u8) return 4;
    }
    return 0;
}
]==])
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Cannot query compiler version")
endif()
foreach(option default --no-proof-opt)
    set(flags)
    if(NOT option STREQUAL "default")
        list(APPEND flags "${option}")
    endif()
    execute_process(COMMAND "${AEVIXC}" --emit-llvm ${flags} "${TEST_DIR}/bounded.avx"
        OUTPUT_VARIABLE llvm_${option} ERROR_VARIABLE error RESULT_VARIABLE status TIMEOUT 60)
    if(NOT "${status}" STREQUAL "0")
        message(FATAL_ERROR "Disabled emission failed: ${error}")
    endif()
endforeach()
if(NOT "${llvm_default}" STREQUAL "${llvm_--no-proof-opt}")
    message(FATAL_ERROR "Default and explicitly disabled IR differ")
endif()
file(WRITE "${TEST_DIR}/disabled.ll" "${llvm_default}")
execute_process(COMMAND "${AEVIXC}" --emit-llvm --proof-opt --proof-opt-stats "${TEST_DIR}/bounded.avx"
    OUTPUT_FILE "${TEST_DIR}/enabled.ll" ERROR_VARIABLE stats RESULT_VARIABLE status TIMEOUT 60)
file(WRITE "${TEST_DIR}/stats.json" "${stats}")
if(NOT "${status}" STREQUAL "0" OR NOT stats MATCHES "\"arithmetic_checks_removed\":[1-9][0-9]*"
    OR NOT stats MATCHES "\"bounds_checks_removed\":[1-9][0-9]*")
    message(FATAL_ERROR "Expected both proven arithmetic and fixed-array guard elimination: ${status}\n${stats}")
endif()
file(READ "${TEST_DIR}/enabled.ll" enabled)
string(REGEX MATCHALL "call \\{ i[0-9]+, i1 \\} @llvm\\.[su](add|sub|mul)\\.with\\.overflow" disabled_arithmetic "${llvm_default}")
string(REGEX MATCHALL "call \\{ i[0-9]+, i1 \\} @llvm\\.[su](add|sub|mul)\\.with\\.overflow" enabled_arithmetic "${enabled}")
string(REGEX MATCHALL "icmp uge i[0-9]+" disabled_bounds "${llvm_default}")
string(REGEX MATCHALL "icmp uge i[0-9]+" enabled_bounds "${enabled}")
list(LENGTH disabled_arithmetic disabled_arithmetic_count)
list(LENGTH enabled_arithmetic enabled_arithmetic_count)
list(LENGTH disabled_bounds disabled_bounds_count)
list(LENGTH enabled_bounds enabled_bounds_count)
if(NOT enabled_arithmetic_count LESS disabled_arithmetic_count OR NOT enabled_bounds_count LESS disabled_bounds_count)
    message(FATAL_ERROR "Emitted IR did not independently confirm fewer arithmetic and bounds guards")
endif()
if(enabled MATCHES " (nsw|nuw|inbounds) ")
    message(FATAL_ERROR "The proof emitter must not add no-wrap or inbounds promises")
endif()
# 의도한 런타임 정지와 전처리·타입·IR 생성 실패를 구분한다.
# Separate intended runtime stops from frontend, type, or IR generation failures.
foreach(fixture bounded overflow bounds loop_overflow)
    execute_process(COMMAND "${AEVIXC}" --check "${TEST_DIR}/${fixture}.avx"
        OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE status TIMEOUT 60)
    if(NOT "${status}" STREQUAL "0")
        message(FATAL_ERROR "Fixture did not pass frontend checks: ${fixture}\n${error}")
    endif()
    foreach(option --no-proof-opt --proof-opt)
        execute_process(COMMAND "${AEVIXC}" --emit-llvm "${option}" "${TEST_DIR}/${fixture}.avx"
            OUTPUT_FILE "${TEST_DIR}/${fixture}-${option}.ll" ERROR_VARIABLE error RESULT_VARIABLE status TIMEOUT 60)
        if(NOT "${status}" STREQUAL "0")
            message(FATAL_ERROR "Fixture IR emission failed: ${fixture} ${option}\n${error}")
        endif()
        foreach(level -O0 -O2)
            execute_process(COMMAND "${AEVIXC}" --build "${level}" "${option}" "${TEST_DIR}/${fixture}.avx"
                -o "${TEST_DIR}/${fixture}-${level}-${option}"
                OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE status TIMEOUT 90)
            if(NOT "${status}" STREQUAL "0")
                message(FATAL_ERROR "Fixture native build failed: ${fixture} ${level} ${option}\n${error}")
            endif()
        endforeach()
    endforeach()
endforeach()
set(modes --run)
if(version MATCHES "ORC JIT enabled")
    list(APPEND modes --jit)
else()
    message(STATUS "JIT execution unadvertised; no JIT pass is claimed")
endif()
foreach(mode IN LISTS modes)
    foreach(level -O0 -O2)
        foreach(fixture bounded overflow bounds loop_overflow)
            foreach(option --no-proof-opt --proof-opt)
                execute_process(COMMAND "${AEVIXC}" "${mode}" "${level}" "${option}" "${TEST_DIR}/${fixture}.avx"
                    OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result TIMEOUT 90)
                file(WRITE "${TEST_DIR}/${fixture}-${mode}-${level}-${option}.stdout" "${output}")
                file(WRITE "${TEST_DIR}/${fixture}-${mode}-${level}-${option}.stderr" "${error}")
                if("${result}" MATCHES "[Tt]imeout|[Tt]imed out")
                    message(FATAL_ERROR "Safety test timed out: ${fixture} ${mode} ${level} ${option}")
                endif()
                if(fixture STREQUAL "bounded")
                    if(NOT "${result}" STREQUAL "0")
                        message(FATAL_ERROR "Safe fixture failed: ${result}\n${output}${error}")
                    endif()
                elseif("${result}" STREQUAL "0")
                    message(FATAL_ERROR "Unsafe fixture completed normally: ${fixture} ${mode} ${level} ${option}")
                elseif(UNIX AND NOT "${result}" MATCHES "^(132|133)$")
                    message(FATAL_ERROR "Expected the existing LLVM trap signal, not a compiler failure: ${fixture} ${result}\n${error}")
                elseif(error MATCHES "error:|invalid LLVM|[Vv]erif.*fail|[Ll]ink.*fail|[Cc]ompil.*fail")
                    message(FATAL_ERROR "Compilation failure must not count as a runtime stop: ${fixture}\n${error}")
                endif()
                if(option STREQUAL "--no-proof-opt")
                    set(control_result "${result}")
                    set(control_output "${output}")
                    set(control_error "${error}")
                elseif(NOT "${result}" STREQUAL "${control_result}" OR NOT "${output}" STREQUAL "${control_output}" OR NOT "${error}" STREQUAL "${control_error}")
                    message(FATAL_ERROR "Same-backend optimized/control observations differ: ${fixture} ${mode} ${level}\n${control_result}/${result}\n${control_error}\n${error}")
                endif()
            endforeach()
        endforeach()
    endforeach()
endforeach()
foreach(mode --check --emit-air --tokens --ast)
    execute_process(COMMAND "${AEVIXC}" "${mode}" --proof-opt "${TEST_DIR}/bounded.avx"
        OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE status TIMEOUT 10)
    if(NOT "${status}" STREQUAL "2")
        message(FATAL_ERROR "Proof backend option accepted in unsupported mode ${mode}: ${status}")
    endif()
endforeach()
execute_process(COMMAND "${AEVIXC}" --check-deps --proof-opt
    OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE status TIMEOUT 10)
if(NOT "${status}" STREQUAL "2")
    message(FATAL_ERROR "Proof option accepted in unsupported dependency-only mode: ${status}")
endif()
message(STATUS "Proof opt-in, identical disabled IR, guarded arithmetic/array elimination, overflow/bounds/loop stops and paired AOT/JIT observations passed")
