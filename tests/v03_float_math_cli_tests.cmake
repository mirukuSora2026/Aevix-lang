# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_float_math_cli_tests.cmake
#
# Description :
#     실제 스칼라 수학 호출·반올림·분류·오류 값과 C 수학 연결을 검사한다.
#     Check real scalar math calls, rounding, classification, error values, and C math linking.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
configure_file("${CMAKE_CURRENT_LIST_DIR}/v03_library_float_math.avx"
    "${TEST_DIR}/float_math_usage.avx" COPYONLY)

function(math_float_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR
        (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()

math_float_run("scalar math check" 0 "ownership checked" --check "${TEST_DIR}/float_math_usage.avx")
foreach(optimization -O0 -O2)
    math_float_run("scalar math AOT ${optimization}" 0 "" --run "${optimization}" "${TEST_DIR}/float_math_usage.avx")
    if(version MATCHES "ORC JIT enabled")
        math_float_run("scalar math JIT ${optimization}" 0 "" --jit "${optimization}" "${TEST_DIR}/float_math_usage.avx")
    endif()
endforeach()

# 표준 라이브러리를 가져오지 않은 C 수학 호출도 Linux에서 libm을 연결해야 한다.
# A C math call without a standard-module import must also link libm on Linux.
file(WRITE "${TEST_DIR}/standalone_math.avx" [=[
extern "C" f64 hypot(f64 left, f64 right);
i32 main() { return hypot(3.0, 4.0) == 5.0 ? 0 : 1; }
]=])
math_float_run("standalone C math AOT" 0 "" --run -O0 "${TEST_DIR}/standalone_math.avx")

function(math_float_reject name pattern source)
    file(WRITE "${TEST_DIR}/${name}.avx" "import math;\n${source}\n")
    math_float_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()
math_float_reject(private_c_helper "private" "i32 main() { math.sinf(0.0f32); return 0; }")
math_float_reject(result_must_be_handled "requires f64.*received Result" "i32 main() { f64 value = math.checked_sqrt(4.0); return 0; }")
math_float_reject(integer_is_not_float "matching|match|requires|overload" "i32 main() { i32 value = 1; math.sin(value); return 0; }")
math_float_reject(low_precision_requires_conversion "matching|match|requires|overload" "i32 main() { f16 value = 4.0; math.sqrt(value); return 0; }")

execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/float_math_usage.avx"
    RESULT_VARIABLE emitted OUTPUT_VARIABLE llvm ERROR_VARIABLE error TIMEOUT 90)
if(NOT "${emitted}" STREQUAL "0")
    message(FATAL_ERROR "Scalar math LLVM emission failed: ${error}")
endif()
file(WRITE "${TEST_DIR}/float_math.ll" "${llvm}")
if(llvm MATCHES "call[^\n]*@(malloc|calloc|realloc|aevix_shared_prepare)\\(")
    message(FATAL_ERROR "Scalar math wrappers introduced an Aevix heap allocation call")
endif()
message(STATUS "Scalar f32/f64 math, error policies, rounding, classification and direct C math linking passed")
