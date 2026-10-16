# ========================
#
# Created By : miruku_sora2026
#
# File name : cli_tests.cmake
#
# Description :
#     실제 실행 파일로 토큰과 AST 출력, 인수 처리 및 오류 종료 코드를 검사한다.
#     Test token and AST output, argument handling, and error exit codes using the actual executable.
#
# ========================

# CLI 분기와 종료 코드를 실제 실행 파일로 검사한다.
# Check CLI branches and exit codes using the built executable.
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()

file(MAKE_DIRECTORY "${TEST_DIR}")
file(WRITE "${TEST_DIR}/valid.avx" "i32 main() { return 0; }\n")
file(WRITE "${TEST_DIR}/lexical-error.avx" "\"unterminated")
file(WRITE "${TEST_DIR}/syntax-error.avx" "i32 main() { return 0 }\n")

function(check_cli name expected_code output_pattern error_pattern)
    execute_process(
        COMMAND "${AEVIXC}" ${ARGN}
        RESULT_VARIABLE exit_code
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 10
    )
    if(NOT "${exit_code}" STREQUAL "${expected_code}")
        message(FATAL_ERROR "${name}: exit=${exit_code}, expected=${expected_code}\n${error}")
    endif()
    if(output_pattern STREQUAL "<empty>")
        if(NOT output STREQUAL "")
            message(FATAL_ERROR "${name}: unexpected stdout: ${output}")
        endif()
    elseif(NOT output MATCHES "${output_pattern}")
        message(FATAL_ERROR "${name}: stdout did not match ${output_pattern}: ${output}")
    endif()
    if(error_pattern STREQUAL "<empty>")
        if(NOT error STREQUAL "")
            message(FATAL_ERROR "${name}: unexpected stderr: ${error}")
        endif()
    elseif(NOT error MATCHES "${error_pattern}")
        message(FATAL_ERROR "${name}: stderr did not match ${error_pattern}: ${error}")
    endif()
endfunction()

check_cli(help 0 "usage:" "<empty>" --help)
check_cli(no_arguments 2 "<empty>" "usage:")
check_cli(unknown_option 2 "<empty>" "invalid command line" --unknown)
check_cli(missing_file 2 "<empty>" "cannot open:" --ast "${TEST_DIR}/missing.avx")
check_cli(tokens 0 "0:3.*i32" "<empty>" --tokens "${TEST_DIR}/valid.avx")
check_cli(ast 0 "Module" "<empty>" --ast "${TEST_DIR}/valid.avx")
check_cli(lexical_error_ast 1 "<empty>" "error:.*[Uu]nterminated" --ast "${TEST_DIR}/lexical-error.avx")
check_cli(lexical_error_tokens 1 "<empty>" "error:.*[Uu]nterminated" --tokens "${TEST_DIR}/lexical-error.avx")
check_cli(syntax_error 1 "<empty>" "error:" --ast "${TEST_DIR}/syntax-error.avx")
check_cli(tokens_skip_parsing 0 "i32" "<empty>" --tokens "${TEST_DIR}/syntax-error.avx")
message(STATUS "CLI checks=10 passed")
