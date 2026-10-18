# ========================
#
# Created By : miruku_sora2026
#
# File name : type_cli_tests.cmake
#
# Description :
#     실행 파일로 타입 검사, 제어 흐름 진단 및 기존 파싱 모드를 검사한다.
#     Test type checking, control-flow diagnostics, and existing parsing modes through the executable.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()

file(MAKE_DIRECTORY "${TEST_DIR}/src/model")
file(WRITE "${TEST_DIR}/src/model/item.avx"
    "public struct Item { public i32 value; };\npublic i32 read(&Item item) { return (*item).value; }\n")
file(WRITE "${TEST_DIR}/src/main.avx"
    "import model.item { Item, read };\ni32 main() { Item item = Item { value = 4 }; return read(&item); }\n")
file(WRITE "${TEST_DIR}/src/wrong_type.avx"
    "i32 main() { i32 count = true; return count; }\n")
file(WRITE "${TEST_DIR}/src/uninitialized.avx"
    "i32 main() { i32 count; return count; }\n")
file(WRITE "${TEST_DIR}/src/missing_return.avx"
    "i32 main() {}\n")
file(WRITE "${TEST_DIR}/src/bad_library.avx"
    "public i32 number() { return 1.5; }\n")
file(WRITE "${TEST_DIR}/src/import_error.avx"
    "import bad_library { number };\ni32 main() { return number(); }\n")
file(WRITE "${TEST_DIR}/src/unknown_name.avx"
    "i32 main() { return absent; }\n")
file(WRITE "${TEST_DIR}/src/deferred.avx"
    "i32 main() { i32 value; value = 3; return value; }\n")
file(WRITE "${TEST_DIR}/src/generic.avx"
    "T identity<T>(T value) { return value; }\ni32 main() { return identity<i32>(1); }\n")

# 성공한 명령의 표준 오류와 실패한 검사의 표준 출력을 함께 확인한다.
# Check stderr for successful commands and stdout for failed semantic checks.
function(check_type_cli name expected_code output_pattern error_pattern)
    execute_process(
        COMMAND "${AEVIXC}" ${ARGN}
        RESULT_VARIABLE exit_code
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 10
    )
    if(NOT "${exit_code}" STREQUAL "${expected_code}")
        message(FATAL_ERROR "${name}: exit=${exit_code}, expected=${expected_code}\nstdout=${output}\nstderr=${error}")
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

check_type_cli(check_valid_module_graph 0 "Type checking succeeded" "<empty>"
    --check --source-root "${TEST_DIR}/src" "${TEST_DIR}/src/main.avx")
check_type_cli(check_inferred_source_root 0 "Type checking succeeded" "<empty>"
    --check "${TEST_DIR}/src/main.avx")
check_type_cli(type_error_has_source_location 1 "<empty>" "wrong_type.avx:[0-9]+:[0-9]+: error:"
    --check "${TEST_DIR}/src/wrong_type.avx")
check_type_cli(uninitialized_read_fails 1 "<empty>" "uninitialized.avx:[0-9]+:[0-9]+: error:"
    --check "${TEST_DIR}/src/uninitialized.avx")
check_type_cli(missing_return_fails 1 "<empty>" "missing_return.avx:[0-9]+:[0-9]+: error:"
    --check "${TEST_DIR}/src/missing_return.avx")
check_type_cli(imported_type_error_has_original_path 1 "<empty>" "bad_library.avx:[0-9]+:[0-9]+: error:"
    --check "${TEST_DIR}/src/import_error.avx")
check_type_cli(name_error_stops_semantic_pipeline 1 "<empty>" "unknown_name.avx:[0-9]+:[0-9]+: error:"
    --check "${TEST_DIR}/src/unknown_name.avx")
check_type_cli(deferred_immutable_initialization 0 "Type checking succeeded" "<empty>"
    --check "${TEST_DIR}/src/deferred.avx")
check_type_cli(generic_function_is_specialized 0 "Type checking succeeded" "<empty>"
    --check "${TEST_DIR}/src/generic.avx")
check_type_cli(ast_remains_syntax_only 0 "Module" "<empty>"
    --ast "${TEST_DIR}/src/wrong_type.avx")
check_type_cli(tokens_remain_syntax_only 0 "i32" "<empty>"
    --tokens "${TEST_DIR}/src/uninitialized.avx")
message(STATUS "Type CLI checks=11 passed")
