# ========================
#
# Created By : miruku_sora2026
#
# File name : module_cli_tests.cmake
#
# Description :
#     실제 파일과 실행 파일로 모듈 검사, 검색 루트, 외부 패키지 및 진단 경로를 검사한다.
#     Test module checks, search roots, external packages, and diagnostic paths using real files and the executable.
#
# ========================

# 모듈 검사 모드를 실제 실행 파일과 파일 시스템으로 확인한다.
# Check module-resolution mode using the actual executable and filesystem.
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()

file(MAKE_DIRECTORY "${TEST_DIR}/src/math" "${TEST_DIR}/package")
file(WRITE "${TEST_DIR}/src/math/vector.avx"
    "public struct Point { public i32 x; };\n")
file(WRITE "${TEST_DIR}/src/main.avx"
    "import math.vector { Point }; Point point; i32 main() { return 0; }\n")
file(WRITE "${TEST_DIR}/src/unknown.avx"
    "i32 main() { Tensor value; return 0; }\n")
file(WRITE "${TEST_DIR}/src/imported_error.avx"
    "import broken; i32 main() { return 0; }\n")
file(WRITE "${TEST_DIR}/src/broken.avx"
    "public i32 get() { return missing; }\n")
file(WRITE "${TEST_DIR}/package/filters.avx"
    "public struct Filter {};\n")
file(WRITE "${TEST_DIR}/src/external.avx"
    "import pixel.filters { Filter }; Filter filter; i32 main() { return 0; }\n")
file(WRITE "${TEST_DIR}/src/rooted.avx"
    "import filters { Filter }; Filter filter; i32 main() { return 0; }\n")

function(check_module_cli name expected_code output_pattern error_pattern)
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

file(MAKE_DIRECTORY "${TEST_DIR}/src/app")
file(WRITE "${TEST_DIR}/src/app/entry.avx"
    "import math.vector { Point }; Point point; i32 main() { return 0; }\n")

check_module_cli(check_modules 0 "Type checking succeeded" "<empty>"
    --check --source-root "${TEST_DIR}/src" "${TEST_DIR}/src/main.avx")
check_module_cli(infer_nearest_source_root 0 "Type checking succeeded" "<empty>"
    --check "${TEST_DIR}/src/app/entry.avx")
check_module_cli(unknown_type 1 "<empty>" "unknown.avx:[0-9]+:[0-9]+: error:"
    --check --source-root "${TEST_DIR}/src" "${TEST_DIR}/src/unknown.avx")
check_module_cli(imported_diagnostic_path 1 "<empty>" "broken.avx:[0-9]+:[0-9]+: error:"
    --check --source-root "${TEST_DIR}/src" "${TEST_DIR}/src/imported_error.avx")
check_module_cli(external_package 0 "Type checking succeeded" "<empty>"
    --package "pixel=${TEST_DIR}/package" --check --source-root "${TEST_DIR}/src" "${TEST_DIR}/src/external.avx")
check_module_cli(extra_module_root 0 "Type checking succeeded" "<empty>"
    --check --module-root "${TEST_DIR}/package" --source-root "${TEST_DIR}/src" "${TEST_DIR}/src/rooted.avx")
check_module_cli(ast_remains_syntax_only 0 "Module" "<empty>"
    --ast "${TEST_DIR}/src/unknown.avx")
check_module_cli(reject_roots_in_ast_mode 2 "<empty>" ".+"
    --ast --source-root "${TEST_DIR}/src" "${TEST_DIR}/src/main.avx")
check_module_cli(reject_roots_in_tokens_mode 2 "<empty>" ".+"
    --tokens --module-root "${TEST_DIR}/package" "${TEST_DIR}/src/main.avx")
check_module_cli(package_missing_separator 2 "<empty>" ".+"
    --check --package "pixel" "${TEST_DIR}/src/main.avx")
check_module_cli(missing_root_argument 2 "<empty>" ".+" --check --source-root)
check_module_cli(missing_entry 2 "<empty>" ".+"
    --check --source-root "${TEST_DIR}/src" "${TEST_DIR}/src/absent.avx")

# 소스와 무관한 끊어진 링크는 무시하지만 끊어진 소스 링크는 진단한다.
# Ignore unrelated dangling links, but diagnose dangling source links.
set(module_cli_checks 12)
if(UNIX)
    file(CREATE_LINK "${TEST_DIR}/missing-data" "${TEST_DIR}/src/dangling.bin" SYMBOLIC
        RESULT data_link_result)
    if(NOT data_link_result STREQUAL "0")
        message(FATAL_ERROR "Cannot create dangling data link: ${data_link_result}")
    endif()
    check_module_cli(ignore_dangling_data_link 0 "Type checking succeeded" "<empty>"
        --check --source-root "${TEST_DIR}/src" "${TEST_DIR}/src/main.avx")
    file(CREATE_LINK "${TEST_DIR}/missing-source" "${TEST_DIR}/src/dangling.avx" SYMBOLIC
        RESULT source_link_result)
    if(NOT source_link_result STREQUAL "0")
        message(FATAL_ERROR "Cannot create dangling source link: ${source_link_result}")
    endif()
    check_module_cli(diagnose_dangling_source_link 1 "<empty>" "Cannot inspect module path:.*dangling.avx"
        --check --source-root "${TEST_DIR}/src" "${TEST_DIR}/src/main.avx")
    file(REMOVE "${TEST_DIR}/src/dangling.bin" "${TEST_DIR}/src/dangling.avx")
    math(EXPR module_cli_checks "${module_cli_checks} + 2")
endif()
message(STATUS "Module CLI checks=${module_cli_checks} passed")
