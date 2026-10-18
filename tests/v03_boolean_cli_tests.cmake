# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_boolean_cli_tests.cmake
#
# Description :
#     논리값의 정수 정책·지연 콜백·소유권 정리·원자적 출력과 할당 실패를 검사한다.
#     Check logical integer policies, lazy callbacks, owned cleanup, atomic output, and allocation failures.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
file(SHA256 "${AEVIXC}" compiler_sha)
message(STATUS "Boolean source gate uses compiler SHA256 ${compiler_sha}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version TIMEOUT 10)
function(boolean_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 120)
    if(NOT "${status}" STREQUAL "${expected}" OR
       (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
foreach(fixture usage numeric worker)
    if(fixture STREQUAL "usage")
        set(original v03_library_boolean.avx)
    else()
        set(original "v03_library_boolean_${fixture}.avx")
    endif()
    configure_file("${CMAKE_CURRENT_LIST_DIR}/${original}" "${TEST_DIR}/${fixture}.avx" COPYONLY)
    boolean_run("${fixture} check" 0 "ownership checked" --check "${TEST_DIR}/${fixture}.avx")
    foreach(optimization -O0 -O2)
        boolean_run("${fixture} AOT ${optimization}" 0 "" --run "${optimization}" "${TEST_DIR}/${fixture}.avx")
        if(version MATCHES "ORC JIT enabled")
            boolean_run("${fixture} JIT ${optimization}" 0 "" --jit "${optimization}" "${TEST_DIR}/${fixture}.avx")
        endif()
    endforeach()
endforeach()
function(boolean_reject name source pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "import boolean;\n${source}\n")
    boolean_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()
boolean_reject(float_is_not_integer "i32 main() { boolean.from_integer(1.0); return 0; }" "Integer|integer|conversion")
boolean_reject(bool_is_not_integer "i32 main() { boolean.from_nonzero(true); return 0; }" "Integer|integer|conversion")
boolean_reject(byte_is_distinct "i32 main() { boolean.from_integer(Byte.from_u8(1u8)); return 0; }" "Integer|integer|conversion")
boolean_reject(char_is_distinct "i32 main() { boolean.from_integer('a'); return 0; }" "Integer|integer|conversion")
boolean_reject(float_output "i32 main() { boolean.to_integer<f32>(true); return 0; }" "Integer|integer|conversion")
boolean_reject(bool_output "i32 main() { boolean.to_integer<bool>(true); return 0; }" "Integer|integer|conversion")
boolean_reject(slice_type "i32 main() { u8 values[1] = [1u8]; boolean.all(Slice.from_array(&values)); return 0; }" "matching|requires|type")
boolean_reject(live_read_loan "i32 main() { mut byte values[5] = [Byte.from_u8(0u8),Byte.from_u8(0u8),Byte.from_u8(0u8),Byte.from_u8(0u8),Byte.from_u8(0u8)]; Slice<byte> read = Slice.from_array(&values); boolean.format_into(false,Slice.from_array_mut(&mut values)); return Byte.to_u8(read[0]); }" "borrow|reference|loan")
boolean_reject(shared_output "i32 main() { byte values[0] = []; boolean.format_into(true,Slice.from_array(&values)); return 0; }" "matching|requires|type")
boolean_reject(eager_owner_move [=[
T success<T,E>(Result<T,E> input) { switch(move input) { case Result<T,E>.Ok { T value = value }: return value; break; default: Never.unreachable(); break; } }
i32 main() { string first = success(string.from_utf8(string.as_bytes("first"))); string other = success(string.from_utf8(string.as_bytes("other"))); string selected = boolean.select(true,first,other); string.view(&first); return 0; }
]=] "moved|unavailable|initialized")
boolean_reject(shared_callback_write "i32 main() { i32 value = 1; Optional<i32> result = boolean.then(true,[&value]() -> i32 { value = 2; return value; }); return 0; }" "mutable|immutable|read.only|borrow|assign")
if(DEFINED CLANG AND EXISTS "${CLANG}" AND DEFINED RUNTIME AND EXISTS "${RUNTIME}")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/v03_library_boolean_failures.avx" "${TEST_DIR}/failures.avx" COPYONLY)
    boolean_run("allocation fixture check" 0 "" --check "${TEST_DIR}/failures.avx")
    execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/failures.avx" RESULT_VARIABLE emitted OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 120)
    if(NOT "${emitted}" STREQUAL "0")
        message(FATAL_ERROR "Boolean allocation IR emission failed: ${error}")
    endif()
    string(REPLACE "@malloc(" "@boolean_test_allocate(" ir "${ir}")
    string(REPLACE "@free(" "@boolean_test_free(" ir "${ir}")
    string(REPLACE "@aevix_text_copy(" "@boolean_test_text_copy(" ir "${ir}")
    file(WRITE "${TEST_DIR}/failures.ll" "${ir}")
    file(WRITE "${TEST_DIR}/failures.c" [=[
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
static uint32_t fail_at, attempts, outstanding;
int32_t aevix_text_copy(void *, const void *, uint64_t, int32_t);
void boolean_fault_point(uint32_t point) { fail_at = point; attempts = 0; }
uint32_t boolean_attempts(void) { return attempts; }
uint32_t boolean_outstanding(void) { return outstanding; }
void *boolean_test_allocate(size_t size) {
    if (++attempts == fail_at) return NULL;
    void *value = malloc(size);
    if (value) outstanding++;
    return value;
}
void boolean_test_free(void *value) { if (value) outstanding--; free(value); }
int32_t boolean_test_text_copy(void *output, const void *input, uint64_t length, int32_t validate) {
    if (length && ++attempts == fail_at) return 1;
    int32_t status = aevix_text_copy(output,input,length,validate);
    if (!status && length) outstanding++;
    return status;
}
]=])
    set(extra_libraries -pthread)
    if(APPLE)
        list(APPEND extra_libraries -lc++)
    elseif(UNIX)
        list(APPEND extra_libraries -lstdc++ -lm)
    endif()
    foreach(optimization -O0 -O2)
        set(binary "${TEST_DIR}/failures${optimization}${CMAKE_EXECUTABLE_SUFFIX}")
        execute_process(COMMAND "${CLANG}" "${optimization}" "${TEST_DIR}/failures.ll" "${TEST_DIR}/failures.c" "${RUNTIME}" ${extra_libraries} -o "${binary}" RESULT_VARIABLE linked OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
        if(NOT "${linked}" STREQUAL "0")
            message(FATAL_ERROR "Boolean allocation fixture link failed: ${output}${error}")
        endif()
        execute_process(COMMAND "${binary}" RESULT_VARIABLE ran OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
        if(NOT "${ran}" STREQUAL "0")
            message(FATAL_ERROR "Boolean allocation/ownership fixture failed ${ran}: ${output}${error}")
        endif()
    endforeach()
else()
    message(STATUS "Boolean allocation failure gate requires CLANG and RUNTIME")
endif()
file(SHA256 "${AEVIXC}" final_sha)
if(NOT "${final_sha}" STREQUAL "${compiler_sha}")
    message(FATAL_ERROR "Compiler changed during Boolean source gate")
endif()
message(STATUS "Boolean20-function source gate passed:15 mode runs,11 negatives, exact Drop/lazy checks and3 allocation failure cases per optimization; explicit source carrier differs from conceptual AllocError/BufferError.")
