# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_range_cli_tests.cmake
#
# Description :
#     독립 Python 수열과 전 정수 폭의 극값으로 Range 보조 함수와 커서를 검사한다.
#     Check source Range helpers and cursors against independent Python sequences and all integer extrema.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
find_program(PYTHON_EXECUTABLE NAMES python3 REQUIRED)
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version TIMEOUT 10)
file(SHA256 "${AEVIXC}" compiler_sha)
message(STATUS "Range source gate uses compiler SHA256 ${compiler_sha}")
set(module_flags)
if(DEFINED TEST_MODULE_ROOT)
    list(APPEND module_flags --module-root "${TEST_MODULE_ROOT}")
endif()
function(range_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${module_flags} ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 180)
    if(NOT "${status}" STREQUAL "${expected}" OR
       (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
execute_process(COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_range_golden.py"
    --output-dir "${TEST_DIR}" --metadata "${TEST_DIR}/range_golden.json"
    RESULT_VARIABLE generated OUTPUT_VARIABLE summary ERROR_VARIABLE error TIMEOUT 60)
if(NOT "${generated}" STREQUAL "0")
    message(FATAL_ERROR "Independent range oracle failed: ${error}")
endif()
message(STATUS "${summary}")
set(fixtures exhaustive_i8 exhaustive_u8)
foreach(element i8 u8 i16 u16 i32 u32 i64 u64 i128 u128 isize usize)
    list(APPEND fixtures "operations_${element}")
endforeach()
list(APPEND fixtures exact_edges)
file(READ "${CMAKE_CURRENT_LIST_DIR}/Range/contracts.avx" contract_source)
file(WRITE "${TEST_DIR}/contracts.avx" "${contract_source}")
list(APPEND fixtures contracts)
foreach(fixture IN LISTS fixtures)
    range_run("${fixture} check" 0 "ownership checked" --check "${TEST_DIR}/${fixture}.avx")
    foreach(optimization -O0 -O2)
        range_run("${fixture} AOT ${optimization}" 0 "" --run "${optimization}" "${TEST_DIR}/${fixture}.avx")
        if(version MATCHES "ORC JIT enabled")
            range_run("${fixture} JIT ${optimization}" 0 "" --jit "${optimization}" "${TEST_DIR}/${fixture}.avx")
        endif()
    endforeach()
endforeach()
foreach(fixture private_state private_construction private_rank private_descriptor)
    range_run("reject ${fixture}" 1 "private" --check "${CMAKE_CURRENT_LIST_DIR}/Range/negative/${fixture}.avx")
endforeach()
range_run("canonical namespace remains separate" 1 "type name|Unknown|resolve|member" --check "${CMAKE_CURRENT_LIST_DIR}/Range/negative/canonical_not_claimed.avx")
range_run("unsigned magnitude required" 1 "matching overload" --check "${CMAKE_CURRENT_LIST_DIR}/Range/negative/wrong_step_type.avx")
range_run("integer element required" 1 "Invalid element type for Range|integer" --check "${CMAKE_CURRENT_LIST_DIR}/Range/negative/float_range_forbidden.avx")
range_run("mutable cursor required" 1 "matching overload|immutable|mutab" --check "${CMAKE_CURRENT_LIST_DIR}/Range/negative/immutable_cursor.avx")
file(WRITE "${TEST_DIR}/range_noallocation.avx" [=[
import range;
i32 main() {
    Range<i8> values=Range.closed(-128i8,127i8);
    mut range.Cursor<i8> cursor=range.iter(values);
    Optional<i8> first=cursor.next();
    return range.contains(&values,-128i8) ? 0 : 1;
}
]=])
execute_process(COMMAND "${AEVIXC}" ${module_flags} --emit-llvm "${TEST_DIR}/range_noallocation.avx"
    RESULT_VARIABLE emitted OUTPUT_VARIABLE llvm ERROR_VARIABLE error TIMEOUT 60)
if(NOT "${emitted}" STREQUAL "0")
    message(FATAL_ERROR "Range IR emission failed: ${error}")
endif()
if(llvm MATCHES "(call|declare)[^\n]*@(aevix_[A-Za-z0-9_]*(alloc|realloc)|malloc|calloc|realloc)")
    message(FATAL_ERROR "Pure range source unexpectedly references an allocator")
endif()
file(SHA256 "${AEVIXC}" final_compiler_sha)
if(NOT compiler_sha STREQUAL final_compiler_sha)
    message(FATAL_ERROR "The compiler changed during the Range source gate")
endif()
message(STATUS "Range: 12 integer types, 131072 endpoint pairs, 4344 independent sequence cases, 114 exact extrema assertions, descriptor/error contracts, fused independent cursors, 8 rejections, and no-allocation IR passed")
