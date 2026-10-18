# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_encoding_cli_tests.cmake
#
# Description :
#     전체 스칼라와 짧은 바이트 입력의 코덱 기준값·대여 수명·할당 실패를 검사한다.
#     Check all-scalar codec goldens, short-byte inputs, borrowed lifetimes, and allocation failure.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
if(NOT DEFINED PYTHON_EXECUTABLE)
    find_program(PYTHON_EXECUTABLE NAMES python3 REQUIRED)
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
file(SHA256 "${AEVIXC}" compiler_sha)
message(STATUS "Encoding source gates use compiler SHA256 ${compiler_sha}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
function(encoding_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR
       (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
configure_file("${CMAKE_CURRENT_LIST_DIR}/v03_library_encoding.avx" "${TEST_DIR}/encoding_usage.avx" COPYONLY)
execute_process(COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_encoding_golden.py"
    --output "${TEST_DIR}/encoding_exhaustive_usage.avx" --metadata "${TEST_DIR}/encoding_golden.json"
    RESULT_VARIABLE generated OUTPUT_VARIABLE golden ERROR_VARIABLE error TIMEOUT 30)
if(NOT "${generated}" STREQUAL "0")
    message(FATAL_ERROR "Python strict-codec golden generation failed: ${error}")
endif()
message(STATUS "Independent Python codec oracle: ${golden}")
foreach(fixture encoding_usage encoding_exhaustive_usage)
    encoding_run("${fixture} check" 0 "ownership checked" --check "${TEST_DIR}/${fixture}.avx")
    foreach(optimization -O0 -O2)
        encoding_run("${fixture} AOT ${optimization}" 0 "" --run "${optimization}" "${TEST_DIR}/${fixture}.avx")
        if(version MATCHES "ORC JIT enabled")
            encoding_run("${fixture} JIT ${optimization}" 0 "" --jit "${optimization}" "${TEST_DIR}/${fixture}.avx")
        endif()
    endforeach()
endforeach()
function(encoding_reject name pattern source)
    file(WRITE "${TEST_DIR}/${name}.avx" "import encoding;\n${source}\n")
    encoding_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()
encoding_reject(bom_slice_cannot_escape "reference.*local|outlive|escape" [=[
Slice<byte> escape() {
    byte local[4] = [Byte.from_u8(239u8), Byte.from_u8(187u8), Byte.from_u8(191u8), Byte.from_u8(65u8)];
    return encoding.strip_utf8_bom(Slice.from_array(&local));
}
i32 main() { return 0; }
]=])
encoding_reject(stripped_bytes_keep_owner "Cannot modify, move, or exclusively borrow a place while it is borrowed" [=[
i32 main() {
    mut byte owner[4] = [Byte.from_u8(239u8), Byte.from_u8(187u8), Byte.from_u8(191u8), Byte.from_u8(65u8)];
    Slice<byte> retained = encoding.strip_utf8_bom(Slice.from_array(&owner));
    owner[3] = Byte.from_u8(66u8);
    return Byte.to_u8(retained[0]) == 65u8 ? 0 : 1;
}
]=])
encoding_reject(codec_input_is_readonly "immutable|read-only" [=[
void mutate(Slice<byte> input) { input[0] = Byte.from_u8(66u8); }
i32 main() { return 0; }
]=])

# 검증 후 출력 할당과 두 단계 변환의 첫 번째·두 번째 실패에서 정확한 해제를 검사한다.
# Verify post-validation output allocation and exact cleanup at both allocations of two-stage conversion.
if(DEFINED CLANG AND EXISTS "${CLANG}" AND DEFINED RUNTIME AND EXISTS "${RUNTIME}")
    file(WRITE "${TEST_DIR}/encoding_failure.avx" [=[
import encoding;
extern "C" void encoding_failure_point(u32 attempt);
extern "C" u32 encoding_outstanding();
extern "C" u32 encoding_attempts();
bool allocation_failed<T>(Result<Vector<T>, encoding.EncodingError> input) {
    switch (move input) {
        case Result<Vector<T>, encoding.EncodingError>.Ok: return false; break;
        case Result<Vector<T>, encoding.EncodingError>.Err { encoding.EncodingError problem = error }:
            switch (problem) { case encoding.EncodingError.AllocationFailure: return true; break; default: return false; break; }
            break;
    }
}
bool success_empty<T>(Result<Vector<T>, encoding.EncodingError> input) {
    switch (move input) { case Result<Vector<T>, encoding.EncodingError>.Ok { Vector<T> output = value }: return Vector.len(&output) == 0; break; default: return false; break; }
}
bool invalid_before_allocation<T>(Result<Vector<T>, encoding.EncodingError> input) {
    switch (move input) {
        case Result<Vector<T>, encoding.EncodingError>.Ok: return false; break;
        case Result<Vector<T>, encoding.EncodingError>.Err { encoding.EncodingError problem = error }:
            switch (problem) {
                case encoding.EncodingError.InvalidUtf8: return true; break;
                case encoding.EncodingError.InvalidUtf16: return true; break;
                case encoding.EncodingError.InvalidScalar: return true; break;
                case encoding.EncodingError.InvalidAscii: return true; break;
                default: return false; break;
            }
            break;
    }
}
i32 main() {
    u32 scalars[3] = [0u32, 65u32, 128512u32];
    byte bytes[6] = [Byte.from_u8(0u8), Byte.from_u8(65u8), Byte.from_u8(240u8), Byte.from_u8(159u8), Byte.from_u8(152u8), Byte.from_u8(128u8)];
    u16 units[4] = [0u16, 65u16, 55357u16, 56832u16];
    u32 ascii[3] = [0u32, 65u32, 127u32];
    byte ascii_bytes[3] = [Byte.from_u8(0u8), Byte.from_u8(65u8), Byte.from_u8(127u8)];
    encoding_failure_point(1);
    if (!allocation_failed(encoding.encode_utf8(Slice.from_array(&scalars))) || encoding_outstanding() != 0 || encoding_attempts() != 1) return 1;
    encoding_failure_point(1);
    if (!allocation_failed(encoding.encode_utf16(Slice.from_array(&scalars))) || encoding_outstanding() != 0 || encoding_attempts() != 1) return 2;
    encoding_failure_point(1);
    if (!allocation_failed(encoding.decode_utf8_scalars(Slice.from_array(&bytes))) || encoding_outstanding() != 0 || encoding_attempts() != 1) return 3;
    encoding_failure_point(1);
    if (!allocation_failed(encoding.decode_utf16_scalars(Slice.from_array(&units))) || encoding_outstanding() != 0 || encoding_attempts() != 1) return 4;
    encoding_failure_point(1);
    if (!allocation_failed(encoding.encode_ascii(Slice.from_array(&ascii))) || encoding_outstanding() != 0 || encoding_attempts() != 1) return 5;
    encoding_failure_point(1);
    if (!allocation_failed(encoding.decode_ascii_scalars(Slice.from_array(&ascii_bytes))) || encoding_outstanding() != 0 || encoding_attempts() != 1) return 6;
    mut u32 point = 1;
    while (point <= 2) {
        encoding_failure_point(point);
        if (!allocation_failed(encoding.utf8_to_utf16(Slice.from_array(&bytes))) || encoding_outstanding() != 0 || encoding_attempts() != point) return 7;
        encoding_failure_point(point);
        if (!allocation_failed(encoding.utf16_to_utf8(Slice.from_array(&units))) || encoding_outstanding() != 0 || encoding_attempts() != point) return 8;
        point++;
    }
    byte invalid8[2] = [Byte.from_u8(65u8), Byte.from_u8(255u8)];
    u16 invalid16[2] = [65u16, 55296u16];
    u32 invalid32[2] = [65u32, 55296u32];
    encoding_failure_point(1);
    if (!invalid_before_allocation(encoding.decode_utf8_scalars(Slice.from_array(&invalid8))) ||
        !invalid_before_allocation(encoding.utf8_to_utf16(Slice.from_array(&invalid8))) ||
        !invalid_before_allocation(encoding.decode_utf16_scalars(Slice.from_array(&invalid16))) ||
        !invalid_before_allocation(encoding.utf16_to_utf8(Slice.from_array(&invalid16))) ||
        !invalid_before_allocation(encoding.encode_utf8(Slice.from_array(&invalid32))) ||
        !invalid_before_allocation(encoding.encode_utf16(Slice.from_array(&invalid32))) ||
        !invalid_before_allocation(encoding.decode_ascii_scalars(Slice.from_array(&invalid8))) ||
        !invalid_before_allocation(encoding.encode_ascii(Slice.from_array(&invalid32))) ||
        encoding_attempts() != 0 || encoding_outstanding() != 0) return 9;
    byte empty8[0] = []; u16 empty16[0] = []; u32 empty32[0] = [];
    if (!success_empty(encoding.encode_utf8(Slice.from_array(&empty32))) ||
        !success_empty(encoding.encode_utf16(Slice.from_array(&empty32))) ||
        !success_empty(encoding.decode_utf8_scalars(Slice.from_array(&empty8))) ||
        !success_empty(encoding.decode_utf16_scalars(Slice.from_array(&empty16))) ||
        !success_empty(encoding.utf8_to_utf16(Slice.from_array(&empty8))) ||
        !success_empty(encoding.utf16_to_utf8(Slice.from_array(&empty16))) ||
        !success_empty(encoding.encode_ascii(Slice.from_array(&empty32))) ||
        !success_empty(encoding.decode_ascii_scalars(Slice.from_array(&empty8))) ||
        encoding_attempts() != 0 || encoding_outstanding() != 0) return 10;
    if (scalars[0] != 0u32 || scalars[2] != 128512u32 || Byte.to_u8(bytes[0]) != 0u8 || units[2] != 55357u16) return 11;
    return 0;
}
]=])
    encoding_run("allocation-failure source check" 0 "" --check "${TEST_DIR}/encoding_failure.avx")
    execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/encoding_failure.avx"
        RESULT_VARIABLE emitted OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${emitted}" STREQUAL "0")
        message(FATAL_ERROR "Encoding allocation-failure IR emission failed: ${error}")
    endif()
    string(REPLACE "@malloc(" "@encoding_test_allocate(" ir "${ir}")
    string(REPLACE "@free(" "@encoding_test_free(" ir "${ir}")
    file(WRITE "${TEST_DIR}/encoding_failure.ll" "${ir}")
    file(WRITE "${TEST_DIR}/encoding_failure.c" [=[
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
static uint32_t fail_at, calls, outstanding;
void encoding_failure_point(uint32_t point) { fail_at = point; calls = 0; }
uint32_t encoding_outstanding(void) { return outstanding; }
uint32_t encoding_attempts(void) { return calls; }
void *encoding_test_allocate(size_t size) {
    if (++calls == fail_at) return NULL;
    void *value = malloc(size); if (value) outstanding++; return value;
}
void encoding_test_free(void *value) { if (value) outstanding--; free(value); }
]=])
    set(extra_libraries -pthread)
    if(APPLE)
        list(APPEND extra_libraries -lc++)
    elseif(UNIX)
        list(APPEND extra_libraries -lstdc++ -lm)
    endif()
    if(WIN32)
        set(failure_binary "${TEST_DIR}/encoding_failure.exe")
    else()
        set(failure_binary "${TEST_DIR}/encoding_failure")
    endif()
    execute_process(COMMAND "${CLANG}" "${TEST_DIR}/encoding_failure.ll" "${TEST_DIR}/encoding_failure.c"
        "${RUNTIME}" ${extra_libraries} -o "${failure_binary}"
        RESULT_VARIABLE linked OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
    if(NOT "${linked}" STREQUAL "0")
        message(FATAL_ERROR "Encoding allocation-failure fixture link failed: ${output}${error}")
    endif()
    execute_process(COMMAND "${failure_binary}" RESULT_VARIABLE ran OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
    if(NOT "${ran}" STREQUAL "0")
        message(FATAL_ERROR "Encoding allocation cleanup failed (${ran}): ${output}${error}")
    endif()
else()
    message(STATUS "Encoding deterministic allocation-failure gate requires CLANG and RUNTIME")
endif()
message(STATUS "Strict Unicode scalar/UTF8/UTF16/ASCII/BOM, complete scalar golden, short inputs, loans and allocation cleanup passed")
