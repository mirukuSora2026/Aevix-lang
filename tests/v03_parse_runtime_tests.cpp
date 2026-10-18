// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_parse_runtime_tests.cpp
//
// Description :
//     스칼라 전체 파싱의 범위·형식·Unicode 오류와 실패 시 출력 보존을 검사한다.
//     Check complete scalar parsing boundaries, syntax, Unicode failures, and unchanged output on errors.
//
// ========================
#include "aevix/Runtime/ParseRuntime.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string_view>

namespace
{
    unsigned checks = 0, failures = 0;
    void expect(bool value, const char* label)
    { ++checks; if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; } }
    template <typename T>
    T success(std::string_view text, int domain, int bits, int sign, T wanted)
    {
        T value{};
        const auto status = aevix_parse_scalar(text.data(), text.size(), domain, bits, sign, &value);
        expect(status == 0 && value == wanted, "valid parse produces the requested scalar"); return value;
    }
    void failure(std::string_view text, int domain, int bits, int sign, int wanted)
    {
        std::array<unsigned char, 16> output; output.fill(0xa5);
        const auto status = aevix_parse_scalar(text.data(), text.size(), domain, bits, sign, output.data());
        expect(status == wanted, "failure has the expected error status");
        for (const auto byte : output) expect(byte == 0xa5, "failure preserves every output byte");
    }
}

int main()
{
    success<std::int8_t>("-128", 0, 8, 1, -128); success<std::int8_t>("127", 0, 8, 1, 127);
    success<std::uint8_t>("255", 0, 8, 0, 255); failure("256", 0, 8, 0, 3);
    success<std::int16_t>("-32768", 0, 16, 1, -32768); failure("32768", 0, 16, 1, 3);
    success<std::uint16_t>("65535", 0, 16, 0, 65535); failure("65536", 0, 16, 0, 3);
    success<std::int32_t>("-2147483648", 0, 32, 1, std::numeric_limits<std::int32_t>::min());
    success<std::uint32_t>("4294967295", 0, 32, 0, std::numeric_limits<std::uint32_t>::max());
    success<std::int64_t>("-9223372036854775808", 0, 64, 1, std::numeric_limits<std::int64_t>::min());
    success<std::uint64_t>("18446744073709551615", 0, 64, 0, std::numeric_limits<std::uint64_t>::max());
    failure("9223372036854775808", 0, 64, 1, 3); failure("-9223372036854775809", 0, 64, 1, 3);
    std::array<std::uint64_t, 2> wide{};
    const auto parseWide = [&](std::string_view text, int sign, std::uint64_t low, std::uint64_t high)
    {
        expect(aevix_parse_scalar(text.data(), text.size(), 0, 128, sign, wide.data()) == 0
            && wide[0] == low && wide[1] == high, "128-bit parsing preserves both limbs");
    };
    parseWide("340282366920938463463374607431768211455", 0, ~std::uint64_t(0), ~std::uint64_t(0));
    parseWide("170141183460469231731687303715884105727", 1, ~std::uint64_t(0), 0x7fffffffffffffffULL);
    parseWide("-170141183460469231731687303715884105728", 1, 0, 0x8000000000000000ULL);
    parseWide("-1", 1, ~std::uint64_t(0), ~std::uint64_t(0));
    failure("340282366920938463463374607431768211456", 0, 128, 0, 3);
    failure("170141183460469231731687303715884105728", 0, 128, 1, 3);
    failure("-170141183460469231731687303715884105729", 0, 128, 1, 3);
    for (const auto input : {"", "+", "-", " 1", "1 ", "1_0", "0xff", "1e2", "1x", "999999999999999999999999999999999999999999x"})
        failure(input, 0, 64, 1, input[0] ? 2 : 1);
    failure("-0", 0, 8, 0, 2); success<std::int8_t>("-0", 0, 8, 1, 0);
    success<std::int32_t>("+00042", 0, 32, 1, 42);
    success<float>("1.5", 1, 32, 0, 1.5f); success<double>("+1.5e2", 1, 64, 0, 150.0);
    success<double>(".5", 1, 64, 0, 0.5); success<double>("1.", 1, 64, 0, 1.0);
    expect(std::signbit(success<double>("-0", 1, 64, 0, -0.0)), "floating negative zero is preserved");
    success<double>("4.9406564584124654e-324", 1, 64, 0, std::numeric_limits<double>::denorm_min());
    failure("1e9999", 1, 64, 0, 3); failure("1e-9999", 1, 64, 0, 3);
    failure("3.5e38", 1, 32, 0, 3);
    for (const auto input : {"nan", "NaN", "NAN", "inf", "INF", "-inf", "+infinity", "Infinity"}) failure(input, 1, 64, 0, 4);
    for (const auto input : {".", "1e", "1e+", "--1", "0x1p1", "1.0x", " 1.0", "1,5"}) failure(input, 1, 64, 0, 2);
    success<std::uint8_t>("true", 2, 1, 0, 1); success<std::uint8_t>("false", 2, 1, 0, 0);
    failure("True", 2, 1, 0, 2); failure("1", 2, 1, 0, 2);
    success<std::uint32_t>("A", 3, 32, 0, 65); success<std::uint32_t>("한", 3, 32, 0, 0xd55c);
    success<std::uint32_t>("😀", 3, 32, 0, 0x1f600);
    success<std::uint32_t>(std::string_view("\0", 1), 3, 32, 0, 0);
    for (const auto input : {std::string_view("ab"), std::string_view("\xc0\x80", 2), std::string_view("\xed\xa0\x80", 3),
                            std::string_view("\xf4\x90\x80\x80", 4), std::string_view("\xe1\x80", 2)}) failure(input, 3, 32, 0, 5);
    success<std::uint8_t>("255", 4, 8, 0, 255); failure("256", 4, 8, 0, 3);
    failure("ff", 4, 8, 0, 2); failure("-1", 4, 8, 0, 2);
    failure(std::string_view("1\0", 2), 0, 32, 1, 2);
    failure("1", 0, 7, 0, 2); failure("1", 1, 16, 0, 2); failure("1", 99, 32, 0, 2);
    failure("true", 2, 1, 1, 2);
    std::uint64_t value = 0xa5;
    expect(aevix_parse_scalar(nullptr, 1, 0, 64, 0, &value) == 2 && value == 0xa5, "invalid native input does not write output");
    expect(aevix_parse_scalar(nullptr, 0, 0, 64, 0, &value) == 1, "null empty view is valid empty input");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
