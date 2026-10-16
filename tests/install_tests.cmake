# ========================
#
# Created By : miruku_sora2026
#
# File name : install_tests.cmake
#
# Description :
#     별도 설치 경로에서 컴파일러와 예제 실행 및 재배치된 런타임을 확인한다.
#     Check the compiler, examples, and relocated runtime from an isolated install prefix.
#
# ========================

if(NOT DEFINED BUILD_DIR OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "BUILD_DIR and TEST_DIR are required")
endif()
set(configuration)
if(DEFINED CONFIG AND NOT "${CONFIG}" STREQUAL "")
    list(APPEND configuration --config "${CONFIG}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" ${configuration} --prefix "${TEST_DIR}"
    RESULT_VARIABLE installed OUTPUT_VARIABLE install_output ERROR_VARIABLE install_error TIMEOUT 30)
if(NOT installed EQUAL 0)
    message(FATAL_ERROR "Install failed: ${install_output} ${install_error}")
endif()
if(WIN32)
    set(compiler "${TEST_DIR}/bin/aevixc.exe")
else()
    set(compiler "${TEST_DIR}/bin/aevixc")
endif()
execute_process(COMMAND "${compiler}" --version RESULT_VARIABLE code OUTPUT_VARIABLE version ERROR_VARIABLE error TIMEOUT 20)
if(NOT DEFINED AEVIX_VERSION)
    set(AEVIX_VERSION "0.2.0")
endif()
if(NOT code EQUAL 0 OR NOT "${version}" MATCHES "Aevix ${AEVIX_VERSION}")
    message(FATAL_ERROR "Installed compiler failed: ${version} ${error}")
endif()
set(basic_example "${TEST_DIR}/share/aevix/examples/Basic/stage7.avx")
if(EXISTS "${basic_example}")
    execute_process(COMMAND "${compiler}" --run "${basic_example}"
        RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "Installed example failed: ${output} ${error}")
    endif()
endif()
set(expanded_example "${TEST_DIR}/share/aevix/examples/Collections/expanded_0_1.avx")
if(EXISTS "${expanded_example}")
    execute_process(COMMAND "${compiler}" --run "${expanded_example}"
        RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "Installed dynamic arrays, Range, packs, dyn and generics failed: ${output} ${error}")
    endif()
    if(version MATCHES "ORC JIT enabled")
        execute_process(COMMAND "${compiler}" --jit "${expanded_example}"
            RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "Installed expanded JIT profile failed: ${output} ${error}")
        endif()
    endif()
endif()
file(WRITE "${TEST_DIR}/persist.avx" "persist i32 answer = 42; i32 main() { return answer == 42 ? 0 : 1; }\n")
execute_process(COMMAND "${compiler}" --run "${TEST_DIR}/persist.avx"
    RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "Installed runtime failed: ${output} ${error}")
endif()
if(UNIX)
    if(NOT DEFINED CLANG OR NOT EXISTS "${CLANG}")
        message(FATAL_ERROR "CLANG is required for the installed symlink regression")
    endif()
    set(link_directory "${TEST_DIR}-symlink/bin")
    file(MAKE_DIRECTORY "${link_directory}")
    set(link_compiler "${link_directory}/aevixc")
    file(REMOVE "${link_compiler}")
    file(CREATE_LINK "${compiler}" "${link_compiler}" SYMBOLIC RESULT link_error)
    if(NOT "${link_error}" STREQUAL "0")
        message(FATAL_ERROR "Cannot create installed compiler symlink: ${link_error}")
    endif()
    set(wrapper "${link_directory}/record-clang.sh")
    set(arguments_file "${link_directory}/clang-arguments.txt")
    string(REPLACE "'" "'\\''" quoted_clang "${CLANG}")
    string(REPLACE "'" "'\\''" quoted_arguments "${arguments_file}")
    # build-tree fallback이 설치 경로 탐색 오류를 숨기지 않도록 실제 연결 인수를 검사한다.
    # Inspect actual link arguments so build-tree fallback cannot hide installed-path errors.
    file(WRITE "${wrapper}" "#!/bin/sh\nprintf '%s\\n' \"$@\" > '${quoted_arguments}'\nexec '${quoted_clang}' \"$@\"\n")
    file(CHMOD "${wrapper}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    execute_process(COMMAND "${link_compiler}" --run --clang "${wrapper}" "${TEST_DIR}/persist.avx"
        RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "Installed symlink runtime failed: ${output} ${error}")
    endif()
    file(READ "${arguments_file}" link_arguments)
    file(GLOB_RECURSE installed_archives "${TEST_DIR}/*aevix_runtime*")
    set(used_installed_runtime FALSE)
    foreach(archive IN LISTS installed_archives)
        string(FIND "${link_arguments}" "${archive}" archive_position)
        if(NOT archive_position EQUAL -1)
            set(used_installed_runtime TRUE)
        endif()
    endforeach()
    if(NOT used_installed_runtime)
        message(FATAL_ERROR "Installed compiler symlink did not link the installed runtime: ${link_arguments}")
    endif()
endif()
message(STATUS "Installed compiler, runtime, and available example execution passed")
