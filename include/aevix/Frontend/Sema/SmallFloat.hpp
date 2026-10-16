// ========================
//
// Created By : miruku_sora2026
//
// File name : SmallFloat.hpp
//
// Description :
//     16비트 부동소수 값을 CPU 명령에 의존하지 않고 짝수 반올림으로 인코딩한다.
//     Encode 16-bit floating values with ties-to-even rounding without CPU-specific instructions.
//
// ========================
#pragma once
#include <bit>
#include <cstdint>
namespace aevix
{
    inline std::uint16_t encodeSmallFloat(double value, bool bfloat)
    {
        const auto raw = std::bit_cast<std::uint64_t>(value);
        const auto sign = static_cast<std::uint16_t>((raw >> 48) & 0x8000);
        const auto absolute = raw & 0x7fffffffffffffffULL;
        const unsigned exponent = static_cast<unsigned>(absolute >> 52);
        const unsigned fraction = bfloat ? 7 : 10, bias = bfloat ? 127 : 15;
        const unsigned maxExponent = bfloat ? 255 : 31;
        if (exponent == 2047) return static_cast<std::uint16_t>(sign | (maxExponent << fraction) | ((absolute & 0xfffffffffffffULL) ? (1U << (fraction - 1)) : 0));
        if (exponent > 1023 + bias) return static_cast<std::uint16_t>(sign | (maxExponent << fraction));
        const unsigned normal = 1024 - bias;
        const unsigned shift = 52 - fraction + (exponent < normal ? normal - exponent : 0);
        if (shift > 63) return sign;
        const auto mantissa = exponent < normal ? (absolute & 0xfffffffffffffULL) | (1ULL << 52) : absolute;
        auto packed = mantissa >> shift;
        if (exponent >= normal) packed -= std::uint64_t(1023 - bias) << fraction;
        const auto remainder = mantissa & ((1ULL << shift) - 1), halfway = 1ULL << (shift - 1);
        if (remainder > halfway || (remainder == halfway && (packed & 1))) ++packed;
        return static_cast<std::uint16_t>(sign | packed);
    }
}
