# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_encoding_bytes_cli_tests.cmake
#
# Description :
#     바이트 순서별 코덱 기준값과 오류 위치·C 문자열·할당 실패 정리를 검사한다.
#     Check endian codec goldens, error offsets, C strings, and allocation-failure cleanup.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
find_program(PYTHON_EXECUTABLE NAMES python3 REQUIRED)
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
function(encoding_bytes_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 120)
    if(NOT "${status}" STREQUAL "${expected}" OR
       (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
configure_file("${CMAKE_CURRENT_LIST_DIR}/v03_library_encoding_bytes.avx" "${TEST_DIR}/encoding_bytes_usage.avx" COPYONLY)
execute_process(COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_encoding_bytes_golden.py"
    --output "${TEST_DIR}/encoding_bytes_golden.avx" --metadata "${TEST_DIR}/encoding_bytes_golden.json"
    RESULT_VARIABLE generated OUTPUT_VARIABLE golden ERROR_VARIABLE error TIMEOUT 30)
if(NOT "${generated}" STREQUAL "0")
    message(FATAL_ERROR "Independent endian codec oracle failed: ${error}")
endif()
message(STATUS "${golden}")
foreach(fixture encoding_bytes_usage encoding_bytes_golden)
    encoding_bytes_run("${fixture} check" 0 "ownership checked" --check "${TEST_DIR}/${fixture}.avx")
    foreach(optimization -O0 -O2)
        encoding_bytes_run("${fixture} AOT ${optimization}" 0 "" --run "${optimization}" "${TEST_DIR}/${fixture}.avx")
        if(version MATCHES "ORC JIT enabled")
            encoding_bytes_run("${fixture} JIT ${optimization}" 0 "" --jit "${optimization}" "${TEST_DIR}/${fixture}.avx")
        endif()
    endforeach()
endforeach()
file(WRITE "${TEST_DIR}/codec_helper_is_private.avx" [=[
import encoding;
i32 main() { byte input[2] = [Byte.from_u8(0u8), Byte.from_u8(0u8)]; encoding.serialized_unit(Slice.from_array(&input), 0, 2, true); return 0; }
]=])
encoding_bytes_run("codec helper privacy" 1 "private" --check "${TEST_DIR}/codec_helper_is_private.avx")
file(WRITE "${TEST_DIR}/decoded_text_view_cannot_escape.avx" [=[
import encoding;
Result<string_view, encoding.EncodingError> escape() { u32 input[1] = [65u32]; string owner = encoding.decode_utf32(Slice.from_array(&input))?; return Result.ok(string.view(&owner)); }
i32 main() { return 0; }
]=])
encoding_bytes_run("owned decoded text view lifetime" 1 "reference.*local|outlive|escape" --check "${TEST_DIR}/decoded_text_view_cannot_escape.avx")

if(DEFINED CLANG AND EXISTS "${CLANG}" AND DEFINED RUNTIME AND EXISTS "${RUNTIME}")
    file(WRITE "${TEST_DIR}/encoding_bytes_failure.avx" [=[
import encoding;
extern "C" void codec_failure_point(u32 attempt);
extern "C" u32 codec_outstanding();
extern "C" u32 codec_attempts();
bool allocation_failed<T>(Result<T, encoding.EncodingError> input) {
    switch (move input) {
        case Result<T, encoding.EncodingError>.Err { encoding.EncodingError problem = error }:
            switch (problem) { case encoding.EncodingError.AllocationFailure: return true; break; default: return false; break; }
            break;
        default: return false; break;
    }
}
bool invalid<T>(Result<T, encoding.EncodingError> input) {
    switch (move input) {
        case Result<T, encoding.EncodingError>.Err { encoding.EncodingError problem = error }:
            switch (problem) {
                case encoding.EncodingError.InvalidScalar: return true; break;
                case encoding.EncodingError.InvalidUtf8: return true; break;
                case encoding.EncodingError.InvalidUtf16: return true; break;
                case encoding.EncodingError.InvalidAscii: return true; break;
                case encoding.EncodingError.TruncatedCodeUnit: return true; break;
                case encoding.EncodingError.InteriorNul: return true; break;
                default: return false; break;
            }
            break;
        default: return false; break;
    }
}
i32 main() {
    string_view text = "A\u{1f600}";
    byte little16[6] = [Byte.from_u8(65u8), Byte.from_u8(0u8), Byte.from_u8(61u8), Byte.from_u8(216u8), Byte.from_u8(0u8), Byte.from_u8(222u8)];
    byte big16[6] = [Byte.from_u8(0u8), Byte.from_u8(65u8), Byte.from_u8(216u8), Byte.from_u8(61u8), Byte.from_u8(222u8), Byte.from_u8(0u8)];
    byte little32[8] = [Byte.from_u8(65u8), Byte.from_u8(0u8), Byte.from_u8(0u8), Byte.from_u8(0u8), Byte.from_u8(0u8), Byte.from_u8(246u8), Byte.from_u8(1u8), Byte.from_u8(0u8)];
    byte big32[8] = [Byte.from_u8(0u8), Byte.from_u8(0u8), Byte.from_u8(0u8), Byte.from_u8(65u8), Byte.from_u8(0u8), Byte.from_u8(1u8), Byte.from_u8(246u8), Byte.from_u8(0u8)];
    mut u32 point = 1;
    while (point <= 3) {
        codec_failure_point(point);
        if (!allocation_failed(encoding.decode_utf16_le(Slice.from_array(&little16))) || codec_outstanding() != 0 || codec_attempts() != point) return 1;
        codec_failure_point(point);
        if (!allocation_failed(encoding.decode_utf16_be(Slice.from_array(&big16))) || codec_outstanding() != 0 || codec_attempts() != point) return 2;
        codec_failure_point(point);
        if (!allocation_failed(encoding.decode_utf32_le(Slice.from_array(&little32))) || codec_outstanding() != 0 || codec_attempts() != point) return 3;
        codec_failure_point(point);
        if (!allocation_failed(encoding.decode_utf32_be(Slice.from_array(&big32))) || codec_outstanding() != 0 || codec_attempts() != point) return 4;
        point++;
    }
    point = 1;
    while (point <= 2) {
        codec_failure_point(point);
        if (!allocation_failed(encoding.encode_utf16_le(text)) || codec_outstanding() != 0 || codec_attempts() != point) return 5;
        codec_failure_point(point);
        if (!allocation_failed(encoding.encode_utf16_be(text)) || codec_outstanding() != 0 || codec_attempts() != point) return 6;
        codec_failure_point(point);
        if (!allocation_failed(encoding.encode_utf32_le(text)) || codec_outstanding() != 0 || codec_attempts() != point) return 7;
        codec_failure_point(point);
        if (!allocation_failed(encoding.encode_utf32_be(text)) || codec_outstanding() != 0 || codec_attempts() != point) return 8;
        point++;
    }
    u32 scalars[2] = [65u32, 128512u32];
    point = 1;
    while (point <= 2) {
        codec_failure_point(point);
        if (!allocation_failed(encoding.decode_utf32(Slice.from_array(&scalars))) || codec_outstanding() != 0 || codec_attempts() != point) return 9;
        point++;
    }
    codec_failure_point(1);
    if (!allocation_failed(encoding.encode_utf32(text)) || codec_outstanding() != 0 || codec_attempts() != 1) return 10;
    codec_failure_point(1);
    if (!allocation_failed(encoding.to_c_utf8(text)) || codec_outstanding() != 0 || codec_attempts() != 1) return 11;
    codec_failure_point(1);
    if (!allocation_failed(encoding.to_c_utf8("")) || codec_outstanding() != 0 || codec_attempts() != 1) return 12;
    point = 1;
    while (point <= 2) {
        codec_failure_point(point);
        if (!allocation_failed(encoding.encode_utf16(text)) || codec_outstanding() != 0 || codec_attempts() != point) return 14;
        point++;
    }
    u16 units[3] = [65u16, 55357u16, 56832u16];
    point = 1;
    while (point <= 3) {
        codec_failure_point(point);
        if (!allocation_failed(encoding.decode_utf16(Slice.from_array(&units))) || codec_outstanding() != 0 || codec_attempts() != point) return 15;
        point++;
    }
    codec_failure_point(1);
    if (!allocation_failed(encoding.encode_utf8(text)) || codec_outstanding() != 0 || codec_attempts() != 1) return 16;
    codec_failure_point(1);
    if (!allocation_failed(encoding.decode_utf8(string.as_bytes(text))) || codec_outstanding() != 0 || codec_attempts() != 1) return 17;
    codec_failure_point(1);
    if (!allocation_failed(encoding.encode_ascii("ascii")) || codec_outstanding() != 0 || codec_attempts() != 1) return 18;
    codec_failure_point(1);
    if (!allocation_failed(encoding.decode_ascii(string.as_bytes("ascii"))) || codec_outstanding() != 0 || codec_attempts() != 1) return 19;
    byte short_input[1] = [Byte.from_u8(0u8)];
    byte wrong_surrogate[2] = [Byte.from_u8(216u8), Byte.from_u8(0u8)];
    byte wrong_scalar[4] = [Byte.from_u8(0u8), Byte.from_u8(0u8), Byte.from_u8(17u8), Byte.from_u8(0u8)];
    u32 wrong_units[1] = [1114112u32];
    codec_failure_point(1);
    if (!invalid(encoding.decode_utf16_le(Slice.from_array(&short_input))) ||
        !invalid(encoding.decode_utf16_be(Slice.from_array(&wrong_surrogate))) ||
        !invalid(encoding.decode_utf32_le(Slice.from_array(&wrong_scalar))) ||
        !invalid(encoding.decode_utf32_be(Slice.from_array(&short_input))) ||
        !invalid(encoding.decode_utf32(Slice.from_array(&wrong_units))) ||
        !invalid(encoding.to_c_utf8("\u{0}")) ||
        !invalid(encoding.decode_utf8(Slice.from_array(&wrong_surrogate))) ||
        !invalid(encoding.decode_ascii(Slice.from_array(&wrong_surrogate))) ||
        !invalid(encoding.encode_ascii("\u{e9}")) || codec_attempts() != 0 || codec_outstanding() != 0) return 13;
    return 0;
}
]=])
    encoding_bytes_run("allocation cleanup check" 0 "" --check "${TEST_DIR}/encoding_bytes_failure.avx")
    execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/encoding_bytes_failure.avx"
        RESULT_VARIABLE emitted OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 120)
    if(NOT "${emitted}" STREQUAL "0")
        message(FATAL_ERROR "Endian allocation-failure IR emission failed: ${error}")
    endif()
    string(REPLACE "@malloc(" "@codec_test_allocate(" ir "${ir}")
    string(REPLACE "@free(" "@codec_test_free(" ir "${ir}")
    string(REPLACE "@aevix_text_copy(" "@codec_test_text_copy(" ir "${ir}")
    file(WRITE "${TEST_DIR}/encoding_bytes_failure.ll" "${ir}")
    file(WRITE "${TEST_DIR}/encoding_bytes_failure.c" [=[
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
static uint32_t fail_at, calls, outstanding;
int32_t aevix_text_copy(void *, const void *, uint64_t, int32_t);
void codec_failure_point(uint32_t point) { fail_at = point; calls = 0; }
uint32_t codec_outstanding(void) { return outstanding; }
uint32_t codec_attempts(void) { return calls; }
void *codec_test_allocate(size_t size) {
    if (++calls == fail_at) return NULL;
    void *value = malloc(size); if (value) outstanding++; return value;
}
void codec_test_free(void *value) { if (value) outstanding--; free(value); }
int32_t codec_test_text_copy(void *output, const void *input, uint64_t length, int32_t validate) {
    if (length && ++calls == fail_at) return 1;
    int32_t status = aevix_text_copy(output, input, length, validate);
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
    if(WIN32)
        set(binary "${TEST_DIR}/encoding_bytes_failure.exe")
    else()
        set(binary "${TEST_DIR}/encoding_bytes_failure")
    endif()
    execute_process(COMMAND "${CLANG}" "${TEST_DIR}/encoding_bytes_failure.ll" "${TEST_DIR}/encoding_bytes_failure.c"
        "${RUNTIME}" ${extra_libraries} -o "${binary}"
        RESULT_VARIABLE linked OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
    if(NOT "${linked}" STREQUAL "0")
        message(FATAL_ERROR "Encoding failure fixture link failed: ${output}${error}")
    endif()
    execute_process(COMMAND "${binary}" RESULT_VARIABLE ran OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
    if(NOT "${ran}" STREQUAL "0")
        message(FATAL_ERROR "Endian allocation cleanup failed (${ran}): ${output}${error}")
    endif()
else()
    message(STATUS "Encoding byte deterministic allocation-failure gate requires CLANG and RUNTIME")
endif()
message(STATUS "Endian Python goldens, strict offsets, BOM preservation, C terminators, lifetimes and all output-allocation failure points passed")
