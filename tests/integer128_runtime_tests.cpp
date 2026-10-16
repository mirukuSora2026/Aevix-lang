// ========================
//
// Created By : miruku_sora2026
//
// File name : integer128_runtime_tests.cpp
//
// Description :
//     네이티브 확장 정수 없이 128비트 산술·실수 변환·범위 길이의 경계를 검증한다.
//     Verify 128-bit arithmetic, float conversion, and range-count boundaries without native extended integers.
//
// ========================
#include "aevix/Common/UInt128.hpp"
#include "aevix/Runtime/Integer128Runtime.hpp"
#include <bit>
#include <cstdint>
#include <iostream>
namespace
{
    unsigned checks = 0, failures = 0;
    void expect(bool condition, const char* name)
    {
        ++checks; if (!condition) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
    }
    using aevix::UInt128;
    std::int32_t binary(UInt128 left, UInt128 right, unsigned operation, bool signedType, UInt128& result)
    {
        std::uint64_t output[2]{};
        const auto status = aevix_integer128_binary(output, left.low, left.high, right.low, right.high, operation, signedType);
        result = {output[0], output[1]}; return status;
    }
}
int main()
{
    const auto maximum = UInt128::mask(128), minimumSigned = UInt128{1} << 127;
    UInt128 value;
    expect(UInt128::parse("340282366920938463463374607431768211455", value) && value == maximum, "parse full unsigned domain");
    expect(value.decimal() == "340282366920938463463374607431768211455", "format full unsigned domain");
    expect(!UInt128::parse("340282366920938463463374607431768211456", value), "reject an unsigned literal beyond 128 bits");
    expect(binary(maximum, UInt128{1}, 0, false, value) == 1, "unsigned addition overflow");
    expect(binary(minimumSigned - UInt128{1}, UInt128{1}, 0, true, value) == 1, "signed addition overflow");
    expect(binary(minimumSigned, UInt128{1}, 1, true, value) == 1, "signed subtraction overflow");
    expect(binary(UInt128{0, 1}, UInt128{0, 1}, 2, false, value) == 1, "full-width multiplication overflow");
    expect(binary(UInt128{~std::uint64_t{0}}, UInt128{~std::uint64_t{0}}, 2, false, value) == 0 && value == UInt128{1, ~std::uint64_t{1}}, "cross-word multiplication without overflow");
    expect(binary(maximum, UInt128{1, 1}, 3, false, value) == 0 && value == UInt128{~std::uint64_t{0}}, "full-width unsigned division");
    expect(binary(maximum, UInt128{1, 1}, 4, false, value) == 0 && !value, "full-width remainder");
    expect(binary(minimumSigned, maximum, 3, true, value) == 1, "signed minimum divided by negative one");
    expect(binary(UInt128{1}, {}, 3, false, value) == 2, "division by zero status");
    for (std::uint64_t left = 1; left < 257; ++left)
        for (std::uint64_t right = 1; right < 33; ++right)
        {
            expect(binary(UInt128{left * 0x100000001ULL}, UInt128{right}, 3, false, value) == 0 && !value.high && value.low == left * 0x100000001ULL / right, "portable division agrees with the 64-bit subdomain");
        }
    const auto floatBits = aevix_integer128_to_float(maximum.low, maximum.high, 0, 3);
    expect(floatBits == (std::uint64_t{1151} << 52), "u128 maximum rounds to 2^128 in f64");
    expect(aevix_integer128_to_float(minimumSigned.low, minimumSigned.high, 1, 3) == ((std::uint64_t{1} << 63) | (std::uint64_t{1150} << 52)), "signed minimum converts to an exact f64 power");
    expect(aevix_integer128_to_float(maximum.low, maximum.high, 0, 1) == 0x7f80, "u128 maximum rounds beyond finite bf16");
    std::uint64_t converted[2]{};
    expect(aevix_integer128_from_float(converted, std::uint64_t{1123} << 52, 0) == 0 && converted[0] == 0 && converted[1] == (std::uint64_t{1} << 36), "convert exact 2^100 to u128");
    expect(aevix_integer128_from_float(converted, (std::uint64_t{1} << 63) | (std::uint64_t{1150} << 52), 1) == 0 && converted[1] == (std::uint64_t{1} << 63), "convert exact negative 2^127 to i128");
    expect(aevix_integer128_from_float(converted, std::uint64_t{1150} << 52, 1) == 1, "reject positive 2^127 for i128");
    expect(aevix_integer128_from_float(converted, std::bit_cast<std::uint64_t>(1.5), 0) == 3, "reject fractional integer conversion");
    expect(aevix_integer128_from_float(converted, 0x7ff0000000000000ULL, 0) == 2, "reject nonfinite integer conversion");
    std::uint64_t count = 0;
    expect(aevix_integer128_range_count(&count, maximum.low - 2, maximum.high, maximum.low, maximum.high, 1, 0, 5) == 0 && count == 3, "closed range near unsigned maximum");
    expect(aevix_integer128_range_count(&count, 0, 0, maximum.low, maximum.high, 1, 0, 5) == 1, "closed full unsigned range count overflows usize");
    expect(aevix_integer128_range_count(&count, 0, 0, maximum.low, maximum.high, maximum.low, maximum.high, 4) == 0 && count == 1, "large step range fits usize");
    expect(aevix_integer128_range_count(&count, 0, minimumSigned.high, maximum.low, minimumSigned.high - 1, 1, 0, 13) == 1, "closed full signed range count overflows usize");
    std::cout << checks << " integer128 runtime checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
