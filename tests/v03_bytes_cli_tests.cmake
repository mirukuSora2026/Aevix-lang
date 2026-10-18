# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_bytes_cli_tests.cmake
#
# Description :
#     바이트 순서·정수 및 실수 비트·엄격한 hex·경계 보존과 할당 실패 정리를 검사한다.
#     Check byte order, integer/float bits, strict hex, bounds preservation, and allocation cleanup.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
find_program(PYTHON_EXECUTABLE NAMES python3 REQUIRED)
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version TIMEOUT 10)
file(SHA256 "${AEVIXC}" compiler_sha)
message(STATUS "Bytes source gate uses compiler SHA256 ${compiler_sha}")
function(bytes_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 120)
    if(NOT "${status}" STREQUAL "${expected}" OR
       (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
configure_file("${CMAKE_CURRENT_LIST_DIR}/v03_library_bytes.avx" "${TEST_DIR}/bytes_usage.avx" COPYONLY)
execute_process(COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_bytes_golden.py"
    --output "${TEST_DIR}/bytes_golden.avx" --metadata "${TEST_DIR}/bytes_golden.json"
    --scalar-output "${TEST_DIR}/bytes_scalar.avx"
    --copy-output "${TEST_DIR}/bytes_copy.avx"
    RESULT_VARIABLE generated OUTPUT_VARIABLE golden ERROR_VARIABLE error TIMEOUT 30)
if(NOT "${generated}" STREQUAL "0")
    message(FATAL_ERROR "Independent integer byte oracle failed: ${error}")
endif()
message(STATUS "${golden}")
file(WRITE "${TEST_DIR}/bytes_named_value.avx" [=[
i32 main() {
    byte sample = Byte.from_u8(value = 255u8);
    return Byte.to_u8(value = sample) == 255u8 ? 0 : 1;
}
]=])
set(bytes_fixtures bytes_usage bytes_golden bytes_scalar bytes_copy bytes_named_value)
if(BYTES_SCALAR_ONLY)
    set(bytes_fixtures bytes_scalar bytes_copy bytes_named_value)
endif()
foreach(fixture ${bytes_fixtures})
    bytes_run("${fixture} check" 0 "ownership checked" --check "${TEST_DIR}/${fixture}.avx")
    foreach(optimization -O0 -O2)
        bytes_run("${fixture} AOT ${optimization}" 0 "" --run "${optimization}" "${TEST_DIR}/${fixture}.avx")
        if(version MATCHES "ORC JIT enabled")
            bytes_run("${fixture} JIT ${optimization}" 0 "" --jit "${optimization}" "${TEST_DIR}/${fixture}.avx")
        endif()
    endforeach()
endforeach()
file(WRITE "${TEST_DIR}/bytes_named_from_typo.avx" [=[
i32 main() { byte sample = Byte.from_u8(vlaue = 255u8); return 0; }
]=])
bytes_run("Byte.from_u8 rejects misspelled argument" 1 "value|argument|match" --check "${TEST_DIR}/bytes_named_from_typo.avx")
file(WRITE "${TEST_DIR}/bytes_named_to_typo.avx" [=[
i32 main() { return Byte.to_u8(vlaue = Byte.from_u8(255u8)); }
]=])
bytes_run("Byte.to_u8 rejects misspelled argument" 1 "value|argument|match" --check "${TEST_DIR}/bytes_named_to_typo.avx")
file(WRITE "${TEST_DIR}/bytes_helper_private.avx" [=[
import bytes;
i32 main() { byte input[0] = []; bytes.read_unsigned(Slice.from_array(&input), 0, 1, true); return 0; }
]=])
bytes_run("private codec helper" 1 "private" --check "${TEST_DIR}/bytes_helper_private.avx")
file(WRITE "${TEST_DIR}/bytes_u8_is_distinct.avx" [=[
import bytes;
i32 main() { u8 input[1] = [1u8]; bytes.read_u8(Slice.from_array(&input), 0); return 0; }
]=])
bytes_run("byte is distinct from u8" 1 "match|requires|received|compatible" --check "${TEST_DIR}/bytes_u8_is_distinct.avx")
file(WRITE "${TEST_DIR}/bytes_exclusive_output.avx" [=[
import bytes;
i32 main() { byte input[1] = [Byte.from_u8(1u8)]; bytes.write_u8(Slice.from_array(&input), 0, 2u8); return 0; }
]=])
bytes_run("writes require exclusive views" 1 "match|requires|received|compatible" --check "${TEST_DIR}/bytes_exclusive_output.avx")
file(WRITE "${TEST_DIR}/bytes_live_read_loan.avx" [=[
import bytes;
i32 main() { mut byte input[1] = [Byte.from_u8(1u8)]; Slice<byte> read = Slice.from_array(&input); bytes.write_u8(Slice.from_array_mut(&mut input), 0, 2u8); return Byte.to_u8(read[0]) == 1u8 ? 0 : 1; }
]=])
bytes_run("live source loan conflicts with mutation" 1 "borrow|loan|reference" --check "${TEST_DIR}/bytes_live_read_loan.avx")

if(DEFINED CLANG AND EXISTS "${CLANG}" AND DEFINED RUNTIME AND EXISTS "${RUNTIME}")
    file(WRITE "${TEST_DIR}/bytes_failures.avx" [=[
import bytes;
extern "C" void bytes_fault_point(u32 attempt);
extern "C" u32 bytes_outstanding();
extern "C" u32 bytes_attempts();
T success<T,E>(Result<T,E> input) {
    switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; default: Never.unreachable(); break; }
}
bool failed<T>(Result<T,bytes.ByteError> input) {
    switch (move input) {
        case Result<T,bytes.ByteError>.Err { bytes.ByteError reason = error }:
            switch (reason) { case bytes.ByteError.AllocationFailure: return true; break; default: return false; break; }
            break;
        default: return false; break;
    }
}
bool malformed<T>(Result<T,bytes.ByteError> input) {
    switch (move input) {
        case Result<T,bytes.ByteError>.Err { bytes.ByteError reason = error }:
            switch (reason) { case bytes.ByteError.InvalidHexLength: return true; break; case bytes.ByteError.InvalidHexDigit: return true; break; default: return false; break; }
            break;
        default: return false; break;
    }
}
i32 main() {
    byte input[3] = [Byte.from_u8(0u8), Byte.from_u8(128u8), Byte.from_u8(255u8)];
    bytes_fault_point(1);
    if (!failed(bytes.decode_hex("0080ff")) || bytes_attempts() != 1 || bytes_outstanding() != 0) return 1;
    mut u32 point = 1;
    while (point <= 2) {
        bytes_fault_point(point);
        if (!failed(bytes.encode_hex(Slice.from_array(&input), false)) || bytes_attempts() != point || bytes_outstanding() != 0) return 2;
        point++;
    }
    bytes_fault_point(1);
    if (!failed(bytes.format_hex(Byte.from_u8(255u8), true)) || bytes_attempts() != 1 || bytes_outstanding() != 0) return 3;
    bytes_fault_point(1);
    if (!failed(bytes.format_binary(Byte.from_u8(255u8))) || bytes_attempts() != 1 || bytes_outstanding() != 0) return 11;
    bytes_fault_point(1);
    if (!malformed(bytes.decode_hex("00g0")) || !malformed(bytes.decode_hex("0xz")) || !malformed(bytes.parse_hex("+0")) || bytes_attempts() != 0 || bytes_outstanding() != 0) return 4;
    bytes_fault_point(0);
    {
        Vector<byte> decoded = success(bytes.decode_hex("0080ff"));
        string encoded = success(bytes.encode_hex(Slice.from_vector(&decoded), false));
        string formatted = success(bytes.format_hex(Byte.from_u8(128u8), true));
        if (!string.equals(string.view(&encoded), "0080ff") || !string.equals(string.view(&formatted), "80")) return 5;
    }
    if (bytes_attempts() != 4 || bytes_outstanding() != 0) return 6;
    bytes_fault_point(1);
    {
        Vector<byte> empty = success(bytes.decode_hex(""));
        string encoded_empty = success(bytes.encode_hex(Slice.from_vector(&empty), false));
        if (Vector.len(&empty) != 0 || !string.is_empty(string.view(&encoded_empty))) return 7;
    }
    if (bytes_attempts() != 0 || bytes_outstanding() != 0) return 8;
    bytes_fault_point(1);
    mut byte floating[8] = [Byte.from_u8(0u8), Byte.from_u8(0u8), Byte.from_u8(0u8), Byte.from_u8(0u8), Byte.from_u8(0u8), Byte.from_u8(0u8), Byte.from_u8(0u8), Byte.from_u8(0u8)];
    success(bytes.write_f16_le(Slice.from_array_mut(&mut floating), 0, Float.from_bits<f16>(65535u16)));
    if (Float.to_bits(success(bytes.read_f16_le(Slice.from_array(&floating), 0))) != 65535u16) return 9;
    success(bytes.write_f16_be(Slice.from_array_mut(&mut floating), 0, Float.from_bits<f16>(65535u16)));
    if (Float.to_bits(success(bytes.read_f16_be(Slice.from_array(&floating), 0))) != 65535u16) return 9;
    success(bytes.write_bf16_le(Slice.from_array_mut(&mut floating), 0, Float.from_bits<bf16>(65535u16)));
    if (Float.to_bits(success(bytes.read_bf16_le(Slice.from_array(&floating), 0))) != 65535u16) return 9;
    success(bytes.write_bf16_be(Slice.from_array_mut(&mut floating), 0, Float.from_bits<bf16>(65535u16)));
    if (Float.to_bits(success(bytes.read_bf16_be(Slice.from_array(&floating), 0))) != 65535u16) return 9;
    success(bytes.write_f32_le(Slice.from_array_mut(&mut floating), 0, Float.from_bits<f32>(4294967295u32)));
    if (Float.to_bits(success(bytes.read_f32_le(Slice.from_array(&floating), 0))) != 4294967295u32) return 9;
    success(bytes.write_f32_be(Slice.from_array_mut(&mut floating), 0, Float.from_bits<f32>(4294967295u32)));
    if (Float.to_bits(success(bytes.read_f32_be(Slice.from_array(&floating), 0))) != 4294967295u32) return 9;
    success(bytes.write_f64_le(Slice.from_array_mut(&mut floating), 0, Float.from_bits<f64>(18446744073709551615u64)));
    if (Float.to_bits(success(bytes.read_f64_le(Slice.from_array(&floating), 0))) != 18446744073709551615u64) return 9;
    success(bytes.write_f64_be(Slice.from_array_mut(&mut floating), 0, Float.from_bits<f64>(18446744073709551615u64)));
    if (Float.to_bits(success(bytes.read_f64_be(Slice.from_array(&floating), 0))) != 18446744073709551615u64) return 9;
    if (bytes_attempts() != 0 || bytes_outstanding() != 0) return 10;
    bytes_fault_point(1);
    byte sample = Byte.from_u8(170u8);
    if (Byte.to_u8(bytes.bit_and(sample, bytes.ones())) != 170u8 || Byte.to_u8(bytes.bit_or(sample, bytes.zero())) != 170u8) return 12;
    if (Byte.to_u8(bytes.bit_xor(sample, sample)) != 0u8 || Byte.to_u8(bytes.bit_and_not(sample, sample)) != 0u8 || Byte.to_u8(bytes.bit_not(sample)) != 85u8) return 12;
    if (bytes.bit_width() != 8u32 || bytes.byte_width() != 1u32 || bytes.is_zero(sample) || bytes.is_all_ones(sample)) return 12;
    if (bytes.count_ones(sample) != 4u32 || bytes.count_zeros(sample) != 4u32 || bytes.leading_zeros(sample) != 0u32 || bytes.leading_ones(sample) != 1u32) return 12;
    if (bytes.trailing_zeros(sample) != 1u32 || bytes.trailing_ones(sample) != 0u32 || bytes.bit_length(sample) != 8u32 || bytes.parity_odd(sample)) return 12;
    if (Byte.to_u8(bytes.reverse_bits(sample)) != 85u8 || Byte.to_u8(bytes.rotate_left(sample, 1u32)) != 85u8 || Byte.to_u8(bytes.rotate_right(sample, 1u32)) != 85u8) return 12;
    if (Byte.to_u8(success(bytes.shift_left(sample, 1u32))) != 84u8 || Byte.to_u8(success(bytes.shift_right(sample, 1u32))) != 85u8) return 12;
    if (!success(bytes.test_bit(sample, 1u32)) || Byte.to_u8(success(bytes.set_bit(sample, 0u32))) != 171u8 || Byte.to_u8(success(bytes.clear_bit(sample, 1u32))) != 168u8) return 12;
    if (Byte.to_u8(success(bytes.toggle_bit(sample, 0u32))) != 171u8 || Byte.to_u8(success(bytes.with_bit(sample, 0u32, false))) != 170u8) return 12;
    if (Byte.to_u8(success(bytes.extract_bits(sample, 1u32, 3u32))) != 5u8 || Byte.to_u8(success(bytes.deposit_bits(sample, 0u32, 1u32, Byte.from_u8(1u8)))) != 171u8) return 12;
    if (Byte.to_u8(bytes.high_nibble(sample)) != 10u8 || Byte.to_u8(bytes.low_nibble(sample)) != 10u8 || !bytes.equal(success(bytes.join_nibbles(Byte.from_u8(10u8), Byte.from_u8(10u8))), sample)) return 12;
    if (!bytes.equal(success(bytes.parse_binary("10101010")), sample)) return 12;
    success(bytes.format_binary_into(sample, Slice.from_array_mut(&mut floating)));
    bytes.compare_bits(sample, sample);
    bytes.compare_lexicographic(Slice.from_array(&floating), Slice.from_array(&floating));
    mut byte overlap[4] = [Byte.from_u8(7u8), Byte.from_u8(38u8), Byte.from_u8(69u8), Byte.from_u8(100u8)];
    success(bytes.copy_within(Slice.from_array_mut(&mut overlap), Range.closed(0usize, 2usize), 1));
    if (Byte.to_u8(overlap[0]) != 7u8 || Byte.to_u8(overlap[1]) != 7u8 || Byte.to_u8(overlap[2]) != 38u8 || Byte.to_u8(overlap[3]) != 69u8) return 16;
    Result<unit,bytes.ByteError> rejected_copy = bytes.copy_within(Slice.from_array_mut(&mut overlap), Range.new(0usize, 1usize), 18446744073709551615usize);
    if (!Result.is_err(&rejected_copy) || Byte.to_u8(overlap[0]) != 7u8 || Byte.to_u8(overlap[3]) != 69u8) return 17;
    if (bytes_attempts() != 0 || bytes_outstanding() != 0) return 13;
    bytes_fault_point(0);
    {
        string binary = success(bytes.format_binary(sample));
        if (!string.equals(string.view(&binary), "10101010")) return 14;
    }
    if (bytes_attempts() != 1 || bytes_outstanding() != 0) return 15;
    return 0;
}
]=])
    bytes_run("allocation fixture check" 0 "" --check "${TEST_DIR}/bytes_failures.avx")
    execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/bytes_failures.avx"
        RESULT_VARIABLE emitted OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 120)
    if(NOT "${emitted}" STREQUAL "0")
        message(FATAL_ERROR "Byte allocation-failure IR emission failed: ${error}")
    endif()
    string(REPLACE "@malloc(" "@bytes_test_allocate(" ir "${ir}")
    string(REPLACE "@free(" "@bytes_test_free(" ir "${ir}")
    string(REPLACE "@aevix_text_copy(" "@bytes_test_text_copy(" ir "${ir}")
    file(WRITE "${TEST_DIR}/bytes_failures.ll" "${ir}")
    file(WRITE "${TEST_DIR}/bytes_failures.c" [=[
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
static uint32_t fail_at, calls, outstanding;
int32_t aevix_text_copy(void *, const void *, uint64_t, int32_t);
void bytes_fault_point(uint32_t point) { fail_at = point; calls = 0; }
uint32_t bytes_outstanding(void) { return outstanding; }
uint32_t bytes_attempts(void) { return calls; }
void *bytes_test_allocate(size_t size) {
    if (++calls == fail_at) return NULL;
    void *value = malloc(size); if (value) outstanding++; return value;
}
void bytes_test_free(void *value) { if (value) outstanding--; free(value); }
int32_t bytes_test_text_copy(void *output, const void *input, uint64_t length, int32_t validate) {
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
        set(binary "${TEST_DIR}/bytes_failures.exe")
    else()
        set(binary "${TEST_DIR}/bytes_failures")
    endif()
    execute_process(COMMAND "${CLANG}" "${TEST_DIR}/bytes_failures.ll" "${TEST_DIR}/bytes_failures.c"
        "${RUNTIME}" ${extra_libraries} -o "${binary}"
        RESULT_VARIABLE linked OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
    if(NOT "${linked}" STREQUAL "0")
        message(FATAL_ERROR "Byte failure fixture link failed: ${output}${error}")
    endif()
    execute_process(COMMAND "${binary}" RESULT_VARIABLE ran OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
    if(NOT "${ran}" STREQUAL "0")
        message(FATAL_ERROR "Byte allocation cleanup failed (${ran}): ${output}${error}")
    endif()
else()
    message(STATUS "Byte deterministic allocation-failure gate requires CLANG and RUNTIME")
endif()
message(STATUS "Bytes source gates passed; native-endian and the full Byte catalog remain separate; float codecs preserve exact representation bits.")
