# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_ascii_cli_tests.cmake
#
# Description :
#     ASCII 전체 바이트·문자 분류와 진법 경계·비 ASCII 보존을 검사한다.
#     Check all ASCII byte/character categories, radix boundaries, and non-ASCII preservation.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/v03_library_ascii.avx" "${TEST_DIR}/ascii_usage.avx" COPYONLY)
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
function(ascii_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR
        (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
ascii_run("ASCII check" 0 "ownership checked" --check "${TEST_DIR}/ascii_usage.avx")
foreach(optimization -O0 -O2)
    ascii_run("ASCII AOT ${optimization}" 0 "" --run "${optimization}" "${TEST_DIR}/ascii_usage.avx")
    if(version MATCHES "ORC JIT enabled")
        ascii_run("ASCII JIT ${optimization}" 0 "" --jit "${optimization}" "${TEST_DIR}/ascii_usage.avx")
    endif()
endforeach()
file(WRITE "${TEST_DIR}/mixed_type.avx" "import ascii;\ni32 main() { u8 value = 65; ascii.to_lower(value); return 0; }\n")
ascii_run("u8 does not implicitly become char or byte" 1 "match|requires|overload" --check "${TEST_DIR}/mixed_type.avx")
file(WRITE "${TEST_DIR}/unchecked_digit.avx" "import ascii;\ni32 main() { Optional<u32> value = ascii.digit_value('A', 16u32); return 0; }\n")
ascii_run("digit Result must be handled" 1 "requires Optional.*received Result" --check "${TEST_DIR}/unchecked_digit.avx")
execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/ascii_usage.avx"
    RESULT_VARIABLE emitted OUTPUT_VARIABLE llvm ERROR_VARIABLE error TIMEOUT 90)
if(NOT "${emitted}" STREQUAL "0")
    message(FATAL_ERROR "ASCII LLVM emission failed: ${error}")
endif()
if(llvm MATCHES "call[^\n]*@(malloc|calloc|realloc|aevix_shared_prepare)\\(")
    message(FATAL_ERROR "ASCII operations introduced a heap allocation call")
endif()
message(STATUS "ASCII byte/character tables, casing, radix 2..36, errors and UTF-8 preservation passed")
