# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_math_cli_tests.cmake
#
# Description :
#     모든 정수 폭의 수학 경계와 8비트 전수 검사를 AOT·JIT로 검증한다.
#     Verify every integer width's math boundaries and exhaustive 8-bit cases through AOT/JIT.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED SOURCE_DIR OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC, SOURCE_DIR, and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
function(math_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR
        (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
foreach(name math math_exhaustive)
    set(source "${SOURCE_DIR}/tests/v03_library_${name}.avx")
    math_run("${name} check" 0 "" --check "${source}")
    math_run("${name} AOT O0" 0 "" --run -O0 "${source}")
    math_run("${name} AOT O2" 0 "" --run -O2 "${source}")
    if(version MATCHES "ORC JIT enabled")
        math_run("${name} JIT O0" 0 "" --jit -O0 "${source}")
        math_run("${name} JIT O2" 0 "" --jit -O2 "${source}")
    endif()
    execute_process(COMMAND "${AEVIXC}" --emit-llvm "${source}" RESULT_VARIABLE status OUTPUT_VARIABLE llvm ERROR_VARIABLE error)
    if(NOT "${status}" STREQUAL "0" OR llvm MATCHES "call[^\n]*@(malloc|calloc|realloc)\\(")
        message(FATAL_ERROR "${name} should contain no heap-allocation calls: ${status}\n${error}")
    endif()
endforeach()
function(math_reject name contents pattern)
    set(path "${TEST_DIR}/${name}.avx")
    file(WRITE "${path}" "${contents}\n")
    math_run("reject ${name}" 1 "${pattern}" --check "${path}")
endfunction()
math_reject(no_import "i32 main() { return math.min(1,2); }" "Unknown|import")
math_reject(float_gcd_input "import math; i32 main() { math.gcd(1.0, 2.0); return 0; }" "matching|overload|type")
math_reject(mixed_width "import math; i32 main() { math.gcd(1i8, 2i16); return 0; }" "matching|overload|type")
math_reject(signed_exponent "import math; i32 main() { math.checked_pow(2i8, -1i32); return 0; }" "matching|overload|type")
math_reject(signed_gcd_result "import math; i32 main() { i8 wrong = math.gcd(-128i8, 0i8); return 0; }" "requires i8.*received u8")
message(STATUS "Integer math: all-width boundaries, 131072 byte pairs, 8192 powers, no-allocation IR, and type rejections passed")
