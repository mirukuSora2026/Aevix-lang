# ========================
#
# Created By : miruku_sora2026
#
# File name : dependency_cli_tests.cmake
#
# Description :
#     도구 누락 경고와 독립적인 의존성 검사, 실행 모드별 검사 범위를 검증한다.
#     Verify missing-tool warnings, standalone dependency probes, and mode-specific checks.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}/src")
file(WRITE "${TEST_DIR}/src/main.avx" "i32 main() { return 42; }\n")
file(WRITE "${TEST_DIR}/src/foreign.avx" "extern \"C\" i32 dependency_test_missing_symbol(); i32 main() { return dependency_test_missing_symbol(); }\n")
set(missing_driver "${TEST_DIR}/missing clang compiler")
set(missing_runtime "${TEST_DIR}/missing runtime archive.a")
set(build_output "${TEST_DIR}/blocked program")
if(CMAKE_HOST_WIN32)
    string(APPEND missing_driver ".exe")
    string(APPEND build_output ".exe")
endif()
file(REMOVE "${missing_driver}" "${missing_runtime}" "${build_output}")

function(run_case name expected required forbidden)
    execute_process(COMMAND "${AEVIXC}" ${ARGN}
        WORKING_DIRECTORY "${TEST_DIR}" RESULT_VARIABLE code
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
    if(NOT "${code}" STREQUAL "${expected}")
        message(FATAL_ERROR "${name}: exit ${code}, expected ${expected}\n${output}\n${error}")
    endif()
    if(NOT "${required}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${required}")
        message(FATAL_ERROR "${name}: missing ${required}\n${output}\n${error}")
    endif()
    if(NOT "${forbidden}" STREQUAL "" AND "${output}${error}" MATCHES "${forbidden}")
        message(FATAL_ERROR "${name}: unexpected ${forbidden}\n${output}\n${error}")
    endif()
    set(case_output "${output}" PARENT_SCOPE)
    set(case_error "${error}" PARENT_SCOPE)
endfunction()

function(expect_dependency name)
    run_case("${name}" 1 "warning: dependency:" "Dependency check passed" ${ARGN})
    if(NOT "${case_error}" MATCHES "error: dependency check failed")
        message(FATAL_ERROR "${name}: missing dependency-check failure on stderr\n${case_error}")
    endif()
    if(NOT "${case_output}" STREQUAL "")
        message(FATAL_ERROR "${name}: failed dependency check wrote stdout\n${case_output}")
    endif()
endfunction()

expect_dependency(missing_absolute_driver --check-deps --clang "${missing_driver}")
run_case(build_missing_driver 1 "warning: dependency:" "Dependency check passed"
    --build --clang "${missing_driver}" -o "${build_output}" "${TEST_DIR}/src/main.avx")
if(NOT "${case_error}" MATCHES "error: AOT compilation failed" OR EXISTS "${build_output}")
    message(FATAL_ERROR "Missing driver did not preserve build failure without an output: ${case_error}")
endif()

# PATH가 비어도 구문·타입 검사와 도움말에는 외부 컴파일러를 요구하지 않는다.
# Syntax and type inspection and informational commands do not require an external compiler on an empty PATH.
set(saved_path "$ENV{PATH}")
set(ENV{PATH} "")
expect_dependency(missing_path_driver --check-deps --clang aevix-nonexistent-clang-for-test)
run_case(frontend_without_path 0 "ownership checked" "warning: dependency:"
    --check "${TEST_DIR}/src/main.avx")
run_case(tokens_without_path 0 "main" "warning: dependency:"
    --tokens "${TEST_DIR}/src/main.avx")
run_case(ast_without_path 0 "main" "warning: dependency:"
    --ast "${TEST_DIR}/src/main.avx")
run_case(air_without_path 0 "return" "warning: dependency:"
    --emit-air "${TEST_DIR}/src/main.avx")
run_case(llvm_without_path 0 "define i32 @main" "warning: dependency:"
    --emit-llvm "${TEST_DIR}/src/main.avx")
run_case(help_without_path 0 "--check-deps" "warning: dependency:" --help)
run_case(version_without_path 0 "Aevix" "warning: dependency:" --version)
if("${case_output}" MATCHES "JIT enabled")
    run_case(jit_without_path 42 "" "warning: dependency:"
        --jit "${TEST_DIR}/src/main.avx")
endif()
set(ENV{PATH} "${saved_path}")

# 진단 전용 명령에 소스와 빌드·모듈 옵션을 섞으면 사용 오류로 처리한다.
# Reject source input and build or module options on the diagnostic-only command.
run_case(reject_source 2 "invalid command line" "warning: dependency:"
    --check-deps "${TEST_DIR}/src/main.avx")
run_case(reject_output 2 "invalid command line" "warning: dependency:"
    --check-deps -o "${build_output}")
run_case(reject_link 2 "invalid command line" "warning: dependency:"
    --check-deps --link "${missing_runtime}")
run_case(reject_load 2 "invalid command line" "warning: dependency:"
    --check-deps --load "${missing_runtime}")
run_case(reject_stdin 2 "invalid command line" "warning: dependency:" --check-deps --stdin)
run_case(reject_source_root 2 "invalid command line" "warning: dependency:"
    --check-deps --source-root "${TEST_DIR}/src")
run_case(reject_module_root 2 "invalid command line" "warning: dependency:"
    --check-deps --module-root "${TEST_DIR}/src")
run_case(reject_package 2 "invalid command line" "warning: dependency:"
    --check-deps --package "fixture=${TEST_DIR}/src")

if(CMAKE_HOST_UNIX)
    set(non_executable "${TEST_DIR}/non executable clang")
    file(WRITE "${non_executable}" "#!/bin/sh\nexit 0\n")
    file(CHMOD "${non_executable}" PERMISSIONS OWNER_READ OWNER_WRITE)
    expect_dependency(driver_without_execute_permission --check-deps --clang "${non_executable}")

    # 가짜 드라이버는 서로 다른 외부 도구 오류를 재현하며 일반 연결 오류와 구분한다.
    # Fake drivers reproduce distinct external-tool failures and distinguish ordinary link errors.
    set(linker_driver "${TEST_DIR}/missing linker driver")
    set(sdk_driver "${TEST_DIR}/missing sdk driver")
    set(undefined_driver "${TEST_DIR}/undefined symbol driver")
    set(empty_success_driver "${TEST_DIR}/success without output driver")
    file(WRITE "${linker_driver}" [=[#!/bin/sh
printf '%s\n' "clang: error: unable to execute command: Executable \"ld\" doesn't exist!" >&2
exit 1
]=])
    file(WRITE "${sdk_driver}" "#!/bin/sh\nprintf '%s\\n' 'ld: library not found for -lSystem' >&2\nexit 1\n")
    file(WRITE "${undefined_driver}" "#!/bin/sh\nprintf '%s\\n' 'ld: undefined symbol: aevix_missing_symbol' >&2\nexit 1\n")
    file(WRITE "${empty_success_driver}" "#!/bin/sh\nexit 0\n")
    file(CHMOD "${linker_driver}" "${sdk_driver}" "${undefined_driver}" "${empty_success_driver}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    expect_dependency(missing_linker --check-deps --clang "${linker_driver}")
    expect_dependency(missing_sdk --check-deps --clang "${sdk_driver}")
    expect_dependency(driver_success_without_output --check-deps --clang "${empty_success_driver}")
    # 일반 빌드에서도 경고를 확인하여 probe의 포괄적 실패 경고로 오분류가 가려지지 않게 한다.
    # Check ordinary builds too so a generic probe-failure warning cannot hide missing classification.
    foreach(failure IN ITEMS linker sdk)
        run_case("build_missing_${failure}" 1 "warning: dependency:" "Dependency check passed"
            --build --clang "${${failure}_driver}" -o "${build_output}" "${TEST_DIR}/src/main.avx")
        if(NOT "${case_error}" MATCHES "error: AOT compilation failed" OR EXISTS "${build_output}")
            message(FATAL_ERROR "Missing ${failure} did not preserve build failure without an output: ${case_error}")
        endif()
    endforeach()
    run_case(undefined_symbol_is_not_dependency 1 "undefined symbol" "warning: dependency:"
        --build --clang "${undefined_driver}" -o "${build_output}" "${TEST_DIR}/src/main.avx")
    foreach(library IN ITEMS custom myproject)
        set(custom_library_driver "${TEST_DIR}/missing ${library} library driver")
        file(WRITE "${custom_library_driver}" "#!/bin/sh\nprintf '%s\\n' 'ld: library not found for -l${library}' >&2\nexit 1\n")
        file(CHMOD "${custom_library_driver}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
        run_case("missing_${library}_is_not_system_dependency" 1 "library not found" "warning: dependency:"
            --build --clang "${custom_library_driver}" -o "${build_output}" "${TEST_DIR}/src/main.avx")
    endforeach()
endif()

if(DEFINED CLANG AND EXISTS "${CLANG}")
    if(CMAKE_HOST_WIN32)
        set(cross_target aarch64-unknown-linux-gnu)
    else()
        set(cross_target x86_64-w64-windows-gnu)
    endif()
    expect_dependency(cross_probe_requires_target_runtime
        --check-deps --clang "${CLANG}" --target "${cross_target}")
    expect_dependency(missing_runtime_probe --check-deps --clang "${CLANG}" --runtime "${missing_runtime}")
    run_case(build_missing_runtime 1 "warning: dependency:" "Dependency check passed"
        --build --clang "${CLANG}" --runtime "${missing_runtime}" -o "${build_output}" "${TEST_DIR}/src/main.avx")
    if(NOT "${case_error}" MATCHES "error: AOT compilation failed" OR EXISTS "${build_output}")
        message(FATAL_ERROR "Missing runtime did not preserve build failure without an output: ${case_error}")
    endif()
    run_case(object_probe_ignores_runtime 0 "Dependency check passed" "warning: dependency:"
        --check-deps -c --clang "${CLANG}" --runtime "${missing_runtime}")
    run_case(host_object_probe 0 "Dependency check passed" "warning: dependency:"
        --check-deps --clang "${CLANG}" -c)
    foreach(target IN ITEMS arm64-apple-macosx aarch64-unknown-linux-gnu x86_64-unknown-linux-gnu x86_64-pc-windows-msvc x86_64-w64-windows-gnu)
        run_case("object_probe_${target}" 0 "Dependency check passed" "warning: dependency:"
            --check-deps --clang "${CLANG}" -c --target "${target}")
    endforeach()
    run_case(host_executable_probe 0 "Dependency check passed" "warning: dependency:"
        --check-deps --clang "${CLANG}")
    run_case(healthy_run 42 "" "warning: dependency:"
        --clang "${CLANG}" "${TEST_DIR}/src/main.avx")
    run_case(real_undefined_symbol_is_not_dependency 1 "error: AOT compilation failed" "warning: dependency:"
        --build --clang "${CLANG}" -o "${build_output}" "${TEST_DIR}/src/foreign.avx")
    if(EXISTS "${build_output}")
        message(FATAL_ERROR "Failed foreign-symbol link left an output")
    endif()
else()
    message(STATUS "LLVM clang unavailable; successful native dependency probes skipped")
endif()

file(GLOB unexpected_outputs "${TEST_DIR}/a.out" "${TEST_DIR}/a.exe" "${TEST_DIR}/a.o" "${TEST_DIR}/a.obj")
if(unexpected_outputs)
    message(FATAL_ERROR "Dependency checks left default build outputs: ${unexpected_outputs}")
endif()
message(STATUS "Dependency CLI checks passed")
