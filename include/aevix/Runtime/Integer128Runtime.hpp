// ========================
//
// Created By : miruku_sora2026
//
// File name : Integer128Runtime.hpp
//
// Description :
//     128비트 정수를 두 워드로 전달하는 이식 가능한 산술·변환 C 규약을 정의한다.
//     Define a portable two-word C ABI for 128-bit arithmetic and conversions.
//
// ========================
#pragma once
#include <cstdint>
extern "C"
{
    // 연산 0~4는 덧셈·뺄셈·곱셈·나눗셈·나머지이며 상태 1은 넘침, 2는 0 나눗셈이다.
    // Operations 0-4 are add, subtract, multiply, divide, remainder; status 1 means overflow and 2 means division by zero.
    std::int32_t aevix_integer128_binary(std::uint64_t* output, std::uint64_t leftLow, std::uint64_t leftHigh,
        std::uint64_t rightLow, std::uint64_t rightHigh, std::int32_t operation, std::int32_t isSigned);
    // 형식 0~3은 f16·bf16·f32·f64이며 IEEE 비트 패턴을 반환한다.
    // Formats 0-3 select f16, bf16, f32, f64; the result is an IEEE bit pattern.
    std::uint64_t aevix_integer128_to_float(std::uint64_t low, std::uint64_t high, std::int32_t isSigned, std::int32_t format);
    // 상태 1·2·3은 범위 오류·유한하지 않은 값·정확하지 않은 정수 변환이다.
    // Statuses 1, 2, 3 mean out of range, nonfinite input, and nonexact integer conversion.
    std::int32_t aevix_integer128_from_float(std::uint64_t* output, std::uint64_t doubleBits, std::int32_t isSigned);
}
extern "C" std::int32_t aevix_integer128_range_count(std::uint64_t* output,
    std::uint64_t startLow, std::uint64_t startHigh, std::uint64_t endLow, std::uint64_t endHigh,
    std::uint64_t stepLow, std::uint64_t stepHigh, std::int32_t flags);
