# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_variadic_provenance_cli_tests.cmake
#
# Description :
#     가변 인수 프레임 저장소와 원소에 보관된 외부 참조의 수명을 구분한다.
#     Distinguish variadic frame storage from external references held by its elements.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
function(pack_run name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(pack_accept name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    pack_run("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    pack_run("${name} AOT" 0 "" --run "${TEST_DIR}/${name}.avx")
    pack_run("${name} AOT O2" 0 "" --run -O2 "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        pack_run("${name} JIT" 0 "" --jit "${TEST_DIR}/${name}.avx")
        pack_run("${name} JIT O2" 0 "" --jit -O2 "${TEST_DIR}/${name}.avx")
    endif()
endfunction()
function(pack_reject name pattern source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    pack_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()
pack_accept(external_element [=[
&i32 first(&i32 values[]) { return values[0]; }
i32 main() { i32 input = 42; &i32 result = first(&input); return *result == 42 ? 0 : 1; }
]=])
pack_accept(copied_descriptor [=[
&i32 first(&i32 values[]) { Slice<&i32> copied = values; return copied[0]; }
&i32 relay(&i32 input) { return first(input); }
i32 main() { i32 input = 42; &i32 result = relay(&input); return *result == 42 ? 0 : 1; }
]=])
pack_reject(frame_slice "variadic call-frame" [=[
Slice<&i32> escape(&i32 values[]) { return values; }
i32 main() { return 0; }
]=])
pack_reject(element_slot "variadic call-frame" [=[
& &i32 escape(&i32 values[]) { return &values[0]; }
i32 main() { return 0; }
]=])
pack_reject(scalar_slot "variadic call-frame" [=[
&i32 escape(i32 values[]) { return &values[0]; }
i32 main() { return 0; }
]=])
pack_reject(short_referent "local" [=[
&i32 first(&i32 values[]) { return values[0]; }
&i32 escape() { i32 input = 42; return first(&input); }
i32 main() { return 0; }
]=])
pack_reject(referent_mutation "borrowed" [=[
&i32 first(&i32 values[]) { return values[0]; }
i32 main() { mut i32 input = 42; &i32 result = first(&input); input++; return *result; }
]=])
message(STATUS "Variadic storage and external element provenance passed")
