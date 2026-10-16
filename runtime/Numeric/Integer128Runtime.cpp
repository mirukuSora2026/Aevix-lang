// ========================
//
// Created By : miruku_sora2026
//
// File name : Integer128Runtime.cpp
//
// Description :
//     확장 정수 ABI나 컴파일러 보조 라이브러리 없이 검사된 128비트 연산을 수행한다.
//     Perform checked 128-bit operations without extended-integer ABIs or compiler helper libraries.
//
// ========================
#include "aevix/Runtime/Integer128Runtime.hpp"
#include "aevix/Common/UInt128.hpp"
namespace
{
    using aevix::UInt128;
    bool negative(UInt128 value, bool signedType) { return signedType && (value.high >> 63) != 0; }
    UInt128 magnitude(UInt128 value, bool signedType) { return negative(value, signedType) ? UInt128{} - value : value; }
    void store(std::uint64_t* output, UInt128 value) { if (output) { output[0] = value.low; output[1] = value.high; } }
}
extern "C" std::int32_t aevix_integer128_binary(std::uint64_t* output, std::uint64_t leftLow, std::uint64_t leftHigh,
    std::uint64_t rightLow, std::uint64_t rightHigh, std::int32_t operation, std::int32_t isSigned)
{
    const UInt128 left{leftLow, leftHigh}, right{rightLow, rightHigh};
    const bool signedType = isSigned != 0, leftNegative = negative(left, signedType), rightNegative = negative(right, signedType);
    UInt128 result;
    bool overflow = false;
    if (operation == 0)
    {
        result = left + right;
        overflow = signedType ? leftNegative == rightNegative && negative(result, true) != leftNegative : result < left;
    }
    else if (operation == 1)
    {
        result = left - right;
        overflow = signedType ? leftNegative != rightNegative && negative(result, true) != leftNegative : left < right;
    }
    else if (operation == 2)
    {
        overflow = UInt128::multiply(magnitude(left, signedType), magnitude(right, signedType), result);
        const bool resultNegative = signedType && (leftNegative != rightNegative);
        if (signedType) overflow = overflow || result > ((UInt128{1} << 127) - UInt128{resultNegative ? 0ULL : 1ULL});
        if (resultNegative) result = UInt128{} - result;
    }
    else if (operation == 3 || operation == 4)
    {
        if (!right) { store(output, {}); return 2; }
        if (signedType && left == (UInt128{1} << 127) && right == UInt128::mask(128)) { store(output, {}); return 1; }
        UInt128 quotient, remainder;
        UInt128::divide(magnitude(left, signedType), magnitude(right, signedType), quotient, remainder);
        result = operation == 3 ? quotient : remainder;
        if (operation == 3 ? leftNegative != rightNegative : leftNegative) result = UInt128{} - result;
    }
    else { store(output, {}); return 1; }
    store(output, result); return overflow ? 1 : 0;
}
extern "C" std::uint64_t aevix_integer128_to_float(std::uint64_t low, std::uint64_t high, std::int32_t isSigned, std::int32_t format)
{
    const UInt128 input{low, high};
    const bool sign = negative(input, isSigned != 0);
    auto absolute = magnitude(input, isSigned != 0);
    if (format < 0 || format > 3) return 0;
    const unsigned fractions[] = {10, 7, 23, 52}, biases[] = {15, 127, 127, 1023}, widths[] = {16, 16, 32, 64};
    const unsigned fraction = fractions[format], bias = biases[format], width = widths[format];
    const auto signBit = sign ? std::uint64_t{1} << (width - 1) : std::uint64_t{0};
    if (!absolute) return signBit;
    unsigned exponent = 127;
    while (!((absolute >> exponent) & UInt128{1})) --exponent;
    if (exponent > bias) return signBit | (static_cast<std::uint64_t>(bias * 2 + 1) << fraction);
    UInt128 significand;
    if (exponent <= fraction) significand = absolute << (fraction - exponent);
    else
    {
        const auto shift = exponent - fraction;
        significand = absolute >> shift;
        const auto remainder = absolute & UInt128::mask(shift), halfway = UInt128{1} << (shift - 1);
        if (remainder > halfway || (remainder == halfway && (significand.low & 1))) significand = significand + UInt128{1};
        if (significand >= (UInt128{1} << (fraction + 1))) { significand = significand >> 1; ++exponent; }
    }
    if (exponent > bias) return signBit | (static_cast<std::uint64_t>(bias * 2 + 1) << fraction);
    return signBit | (static_cast<std::uint64_t>(exponent + bias) << fraction) | (significand.low & ((std::uint64_t{1} << fraction) - 1));
}
extern "C" std::int32_t aevix_integer128_from_float(std::uint64_t* output, std::uint64_t doubleBits, std::int32_t isSigned)
{
    store(output, {});
    const unsigned rawExponent = static_cast<unsigned>((doubleBits >> 52) & 2047);
    const auto fraction = doubleBits & 0xfffffffffffffULL;
    const bool sign = (doubleBits >> 63) != 0;
    if (rawExponent == 2047) return 2;
    if (!rawExponent && !fraction) return 0;
    if (sign && !isSigned) return 1;
    const int exponent = static_cast<int>(rawExponent) - 1023;
    if (exponent < 0) return 3;
    if (exponent > 127) return 1;
    UInt128 magnitude{fraction | 0x10000000000000ULL};
    if (exponent >= 52) magnitude = magnitude << static_cast<unsigned>(exponent - 52);
    else
    {
        const unsigned shift = 52 - static_cast<unsigned>(exponent);
        if (magnitude & UInt128::mask(shift)) return 3;
        magnitude = magnitude >> shift;
    }
    if (isSigned && magnitude > ((UInt128{1} << 127) - UInt128{sign ? 0ULL : 1ULL})) return 1;
    store(output, sign ? UInt128{} - magnitude : magnitude); return 0;
}

extern "C" std::int32_t aevix_integer128_range_count(std::uint64_t* output,
    std::uint64_t startLow, std::uint64_t startHigh, std::uint64_t endLow, std::uint64_t endHigh,
    std::uint64_t stepLow, std::uint64_t stepHigh, std::int32_t flags)
{
    if (output) *output = 0;
    if (!(flags & 4)) return 0;
    const UInt128 start{startLow, startHigh}, end{endLow, endHigh}, step{stepLow, stepHigh};
    if (!step) return 1;
    const auto orderKey = (flags & 8) ? UInt128{0, std::uint64_t{1} << 63} : UInt128{};
    const auto left = start ^ orderKey, right = end ^ orderKey;
    const bool closed = (flags & 1) != 0, descending = (flags & 2) != 0;
    if (descending ? (left < right || (!closed && left == right)) : (left > right || (!closed && left == right))) return 0;
    const auto distance = descending ? start - end : end - start;
    UInt128 quotient, remainder; UInt128::divide(distance, step, quotient, remainder);
    const bool extra = closed || static_cast<bool>(remainder);
    if (extra)
    {
        const auto next = quotient + UInt128{1};
        if (next < quotient) return 1;
        quotient = next;
    }
    if (quotient.high) return 1;
    if (output) *output = quotient.low;
    return 0;
}
