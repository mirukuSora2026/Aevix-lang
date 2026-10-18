# ========================
#
# Created By : miruku_sora2026
#
# File name : runtime_cli_tests.cmake
#
# Description :
#     오류 전파, C FFI, 편집기 입력과 AOT·JIT 명령행의 실제 실행을 검사한다.
#     Check actual error propagation, C FFI, editor input, and AOT/JIT CLI execution.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR OR NOT DEFINED CLANG)
    message(FATAL_ERROR "AEVIXC, TEST_DIR, and CLANG are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
function(run_case name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE code
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
    if(NOT "${code}" STREQUAL "${expected}" OR NOT "${output}${error}" MATCHES "${pattern}")
        message(FATAL_ERROR "${name}: exit=${code}, expected=${expected}\n${output}\n${error}")
    endif()
endfunction()

file(WRITE "${TEST_DIR}/abs.avx" "extern \"C\" i32 abs(i32 value); i32 main() { return abs(-42); }\n")
file(WRITE "${TEST_DIR}/foreign.avx" "extern \"C\" i32 native_add(i32 a, i32 b); i32 main() { return native_add(19, 23); }\n")
file(WRITE "${TEST_DIR}/small.avx" "extern \"C\" i8 narrow(i8 value); i32 main() { return 0; }\n")
file(WRITE "${TEST_DIR}/pointer.avx" "extern \"C\" i32 mutate(&mut i32 value); i32 main() { return 0; }\n")
file(WRITE "${TEST_DIR}/method.avx" "struct Item { public extern \"C\" i32 get(i32 value); }; i32 main() { return 0; }\n")
run_case(c_abi_abs 42 "" --run "${TEST_DIR}/abs.avx")
run_case(reject_small_abi 1 "C FFI" --check "${TEST_DIR}/small.avx")
run_case(reject_borrow_abi 1 "C FFI" --check "${TEST_DIR}/pointer.avx")
run_case(reject_member_abi 1 "C FFI" --check "${TEST_DIR}/method.avx")

file(WRITE "${TEST_DIR}/native.c" "#if defined(_WIN32)\n#define EXPORT __declspec(dllexport)\n#else\n#define EXPORT\n#endif\nEXPORT int native_add(int a, int b) { return a + b; } EXPORT int f40(void) { return 42; } EXPORT int g40(void) { return 42; }\n")
set(shared_options)
if(APPLE)
    set(native "${TEST_DIR}/native library.dylib")
    set(shared_options -dynamiclib -fPIC)
    set(native_link "${native}")
elseif(WIN32)
    set(native "${TEST_DIR}/native library.dll")
    execute_process(COMMAND "${CLANG}" -dumpmachine RESULT_VARIABLE target_code
        OUTPUT_VARIABLE native_target OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_VARIABLE target_error TIMEOUT 10)
    if(NOT target_code EQUAL 0)
        message(FATAL_ERROR "Cannot determine Windows C ABI: ${target_error}")
    endif()
    # 실행 중에는 DLL을 찾고, AOT 연결에는 ABI에 맞는 import library를 사용한다.
    # Locate the DLL at runtime and link AOT code to its ABI-specific import library.
    if(native_target MATCHES "windows-msvc")
        set(native_link "${TEST_DIR}/native import.lib")
        set(shared_options -shared "-Wl,/implib:${native_link}")
    elseif(native_target MATCHES "windows-gnu|mingw")
        set(native_link "${TEST_DIR}/native import.dll.a")
        set(shared_options -shared "-Wl,--out-implib,${native_link}")
    else()
        message(FATAL_ERROR "Unsupported Windows C ABI fixture target: ${native_target}")
    endif()
    set(ENV{PATH} "${TEST_DIR};$ENV{PATH}")
else()
    set(native "${TEST_DIR}/native library.so")
    set(shared_options -shared -fPIC)
    set(native_link "${native}")
endif()
execute_process(COMMAND "${CLANG}" ${shared_options} "${TEST_DIR}/native.c" -o "${native}"
    RESULT_VARIABLE native_code ERROR_VARIABLE native_error TIMEOUT 30)
if(NOT native_code EQUAL 0)
    message(FATAL_ERROR "Cannot build C ABI fixture: ${native_error}")
endif()
run_case(c_abi_explicit_library 42 "" --run --link "${native_link}" "${TEST_DIR}/foreign.avx")
file(WRITE "${TEST_DIR}/symbols.avx" "extern \"C\" i32 f40(); extern \"C\" i32 g40(); const i32 fallback = 0; i32 main() { return f40() + g40() == 84 ? 42 : fallback; }\n")
run_case(c_abi_internal_names 42 "" --run --link "${native_link}" "${TEST_DIR}/symbols.avx")
run_case(c_abi_private_namespace 0 "define i32 @aevix.fn." --emit-llvm "${TEST_DIR}/symbols.avx")
file(MAKE_DIRECTORY "${TEST_DIR}/modules/src")
file(WRITE "${TEST_DIR}/modules/src/one.avx" "public extern \"C\" i32 abs(i32 value);\n")
file(WRITE "${TEST_DIR}/modules/src/two.avx" "public extern \"C\" i32 abs(i32 value);\n")
file(WRITE "${TEST_DIR}/modules/src/main.avx" "import one { abs }; import two { abs }; i32 main() { return one.abs(-20) + two.abs(-22); }\n")
run_case(c_abi_repeated_declaration 42 "" --run "${TEST_DIR}/modules/src/main.avx")

file(WRITE "${TEST_DIR}/overlay.avx" "i32 main() { return 9; }\n")
file(WRITE "${TEST_DIR}/buffer.txt" "i32 main() { return 1.5; }\n")
execute_process(COMMAND "${AEVIXC}" --check --stdin "${TEST_DIR}/overlay.avx"
    INPUT_FILE "${TEST_DIR}/buffer.txt" RESULT_VARIABLE overlay_code
    OUTPUT_VARIABLE overlay_output ERROR_VARIABLE overlay_error TIMEOUT 20)
if(NOT "${overlay_code}" STREQUAL "1" OR NOT "${overlay_error}" MATCHES "overlay.avx:[0-9]+:[0-9]+: error:")
    message(FATAL_ERROR "Editor overlay did not report a located error: ${overlay_output} ${overlay_error}")
endif()
file(READ "${TEST_DIR}/overlay.avx" disk_source)
if(NOT "${disk_source}" STREQUAL "i32 main() { return 9; }\n")
    message(FATAL_ERROR "Editor overlay modified the source file")
endif()

file(WRITE "${TEST_DIR}/propagate.avx" "Result<i32, i32> value() { return Result.err(42); } Result<i32, i32> next() { i32 result = value()?; return Result.ok(result); } i32 main() { Result<i32, i32> result = next(); switch (result) { case Result<i32, i32>.Ok { i32 value = value }: return value; break; case Result<i32, i32>.Err { i32 code = error }: return code; break; } }\n")
run_case(result_propagation 42 "" --run "${TEST_DIR}/propagate.avx")
execute_process(COMMAND "${AEVIXC}" --version RESULT_VARIABLE version_code OUTPUT_VARIABLE version)
if(NOT version_code EQUAL 0 OR NOT "${version}" MATCHES "Aevix [0-9]+\\.[0-9]+\\.[0-9]+")
    message(FATAL_ERROR "Missing compiler version: ${version}")
endif()
if("${version}" MATCHES "JIT enabled")
    run_case(jit_c_process 42 "" --jit "${TEST_DIR}/abs.avx")
    run_case(jit_c_library 42 "" --jit --load "${native}" "${TEST_DIR}/foreign.avx")
    run_case(jit_c_internal_names 42 "" --jit --load "${native}" "${TEST_DIR}/symbols.avx")
    run_case(jit_c_repeated_declaration 42 "" --jit "${TEST_DIR}/modules/src/main.avx")
    run_case(jit_result_propagation 42 "" --jit "${TEST_DIR}/propagate.avx")
endif()
message(STATUS "Runtime CLI checks passed")
