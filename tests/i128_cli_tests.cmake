# ========================
#
# Created By : miruku_sora2026
#
# File name : i128_cli_tests.cmake
#
# Description :
#     128비트 리터럴·산술·변환·범위 길이를 실제 AOT와 JIT 실행으로 검증한다.
#     Verify 128-bit literals, arithmetic, conversions, and range counts through real AOT and JIT execution.
#
# ========================
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(modes --run)
if(version MATCHES "ORC JIT enabled")
    list(APPEND modes --jit)
endif()
function(program name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}")
    foreach(mode IN LISTS modes)
        set(options)
        if(mode STREQUAL "--run")
            list(APPEND options --clang "${CLANG}")
        endif()
        foreach(optimization -O0 -O2)
            execute_process(COMMAND "${AEVIXC}" "${mode}" ${options} "${optimization}" "${TEST_DIR}/${name}.avx"
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
            if(NOT result EQUAL 0)
                message(FATAL_ERROR "${name} ${mode} ${optimization} failed (${result}): ${output}${error}")
            endif()
        endforeach()
    endforeach()
endfunction()
program(constants [=[
const u128 MAX = 340282366920938463463374607431768211455u128;
const i128 MIN = -170141183460469231731687303715884105728i128;
const u128 BIT = 1u128 << 100;
const u128 QUOTIENT = MAX / 18446744073709551617u128;
const u128 PRODUCT = 18446744073709551615u128 * 18446744073709551615u128;
const i128 SHIFTED = -170141183460469231731687303715884105728i128 >> 127;
i32 main() {
    if (MAX != 0xffffffffffffffffffffffffffffffffu128 || MIN != -170141183460469231731687303715884105728i128) return 1;
    if (BIT != 1267650600228229401496703205376u128 || QUOTIENT != 18446744073709551615u128) return 2;
    if (PRODUCT != 340282366920938463426481119284349108225u128 || SHIFTED != -1i128) return 3;
    return 0;
}
]=])
program(arithmetic [=[
i32 main() {
    u128 a = 18446744073709551616u128;
    u128 b = 18446744073709551615u128;
    u128 product = a * b;
    if (product != 340282366920938463444927863358058659840u128) return 1;
    if (product / a != b || product % a != 0u128) return 2;
    i128 negative = -1267650600228229401496703205376i128;
    i128 quotient = negative / 3i128;
    i128 remainder = negative % 3i128;
    if (quotient * 3i128 + remainder != negative || remainder != -1i128) return 3;
    u128 high = 1u128 << 127;
    if (high >> 127 != 1u128 || ~0u128 != 340282366920938463463374607431768211455u128) return 4;
    i128 signed_bits = -1i128;
    if (signed_bits >> 64 != -1i128 || -negative != 1267650600228229401496703205376i128) return 5;
    return 0;
}
]=])
program(conversions [=[
i32 main() {
    rsn widened = Integer.convert<i128>(18446744073709551615u64);
    rsn narrowed = Integer.convert<u64>(340282366920938463463374607431768211455u128);
    rsn signed_overflow = Integer.convert<i128>(340282366920938463463374607431768211455u128);
    rsn negative_unsigned = Integer.convert<u128>(-1i128);
    rsn identity = Integer.convert<u128>(340282366920938463463374607431768211455u128);
    if (Result.is_err(&widened) || Result.is_ok(&narrowed) || Result.is_ok(&signed_overflow) || Result.is_ok(&negative_unsigned) || Result.is_err(&identity)) return 1;
    rsn exact = Float.convert_exact<f64>(1267650600228229401496703205376u128);
    rsn nonexact = Float.convert_exact<f64>(340282366920938463463374607431768211455u128);
    rsn finite = Float.convert<f64>(340282366920938463463374607431768211455u128);
    rsn too_large = Float.convert<bf16>(340282366920938463463374607431768211455u128);
    if (Result.is_err(&exact) || Result.is_ok(&nonexact) || Result.is_err(&finite) || Result.is_ok(&too_large)) return 2;
    rsn integer = Float.to_integer_exact<u128>(1267650600228229401496703205376.0f64);
    rsn minimum = Float.to_integer_exact<i128>(-170141183460469231731687303715884105728.0f64);
    rsn positive_too_large = Float.to_integer_exact<i128>(170141183460469231731687303715884105728.0f64);
    rsn fractional = Float.to_integer_exact<i128>(1.5f64);
    if (Result.is_err(&integer) || Result.is_err(&minimum) || Result.is_ok(&positive_too_large) || Result.is_ok(&fractional)) return 3;
    return 0;
}
]=])
program(ranges [=[
i32 main() {
    Range<u128> edge = Range.closed(340282366920938463463374607431768211453u128, 340282366920938463463374607431768211455u128);
    mut usize count = 0;
    for (u128 value in edge) count++;
    if (count != 3) return 1;
    Range<i128> negatives = Range.closed(-170141183460469231731687303715884105728i128, -170141183460469231731687303715884105726i128);
    mut usize signed_count = 0;
    for (i128 value in negatives) signed_count++;
    if (signed_count != 3) return 2;
    rsn short_count = Range.len(&edge);
    if (Result.is_err(&short_count)) return 3;
    Range<u128> full = Range.closed(0u128, 340282366920938463463374607431768211455u128);
    rsn overflow = Range.len(&full);
    if (Result.is_ok(&overflow)) return 4;
    Range<u128> half_open = Range.new(0u128, 340282366920938463463374607431768211455u128);
    rsn huge_half = Range.len(&half_open);
    if (Result.is_ok(&huge_half)) return 5;
    return 0;
}
]=])
program(layout_and_generic [=[
struct Wide implements Copy { public u8 lead; public i128 value; public u8 tail; };
T identity<T>(T input) { return input; }
Result<i128, IntegerError> narrow(u128 input) { return Result.ok(Integer.convert<i128>(input)?); }
i32 main() {
    Wide wide = Wide { lead = 3u8, value = -170141183460469231731687303715884105728i128, tail = 7u8 };
    Wide array[2] = [wide; 2];
    i128 copied = identity(array[1].value);
    if (array[1].lead != 3u8 || array[1].tail != 7u8 || copied != wide.value) return 1;
    rsn converted = narrow(170141183460469231731687303715884105727u128);
    switch (converted) {
        case Result<i128, IntegerError>.Ok { i128 value = value }:
            if (value != 170141183460469231731687303715884105727i128) return 2;
            break;
        default: return 3; break;
    }
    Range<u128> range = Range.closed(340282366920938463463374607431768211453u128,340282366920938463463374607431768211455u128);
    rsn result = Range.len(&range);
    switch (result) {
        case Result<usize, RangeError>.Ok { usize value = value }:
            if (value != 3) return 4;
            break;
        default: return 5; break;
    }
    return 0;
}
]=])
foreach(source
    "i32 main() { u128 bad = 340282366920938463463374607431768211456u128; return 0; }"
    "i32 main() { i128 bad = 170141183460469231731687303715884105728i128; return 0; }"
    "i32 main() { i128 bad = -170141183460469231731687303715884105729i128; return 0; }"
    "const u128 BAD = 340282366920938463463374607431768211455u128 + 1u128; i32 main() { return 0; }"
    "extern \"C\" i128 foreign(i128 value); i32 main() { return 0; }")
    file(WRITE "${TEST_DIR}/reject.avx" "${source}")
    execute_process(COMMAND "${AEVIXC}" --check "${TEST_DIR}/reject.avx"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
    if(result EQUAL 0 OR NOT error MATCHES "error:")
        message(FATAL_ERROR "Invalid 128-bit source accepted: ${source} ${output}${error}")
    endif()
endforeach()
set(sequence 0)
foreach(body
    "i128 value = 170141183460469231731687303715884105727i128; i128 bad = value + 1i128;"
    "u128 value = 0u128; u128 bad = value - 1u128;"
    "u128 value = 18446744073709551616u128; u128 bad = value * value;"
    "u128 value = 3u128; u128 zero = 0u128; u128 bad = value / zero;"
    "i128 value = -170141183460469231731687303715884105728i128; i128 minus = -1i128; i128 bad = value / minus;"
    "u128 value = 1u128; usize shift = 128; u128 bad = value << shift;")
    math(EXPR sequence "${sequence} + 1")
    file(WRITE "${TEST_DIR}/trap${sequence}.avx" "i32 main() { ${body} return 0; }")
    set(executable "${TEST_DIR}/trap${sequence}")
    if(WIN32)
        string(APPEND executable ".exe")
    endif()
    execute_process(COMMAND "${AEVIXC}" --build --clang "${CLANG}" "${TEST_DIR}/trap${sequence}.avx" -o "${executable}"
        RESULT_VARIABLE build_result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
    if(NOT build_result EQUAL 0)
        message(FATAL_ERROR "Trap fixture failed to compile rather than execute: ${output}${error}")
    endif()
    execute_process(COMMAND "${executable}" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
    if("${result}" STREQUAL "0")
        message(FATAL_ERROR "128-bit checked arithmetic failed to stop safely: ${body}")
    endif()
endforeach()
message(STATUS "128-bit AOT/JIT arithmetic, conversion, ranges and safe-stop boundaries passed")
