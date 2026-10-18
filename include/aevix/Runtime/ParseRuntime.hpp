// ========================
//
// Created By : miruku_sora2026
//
// File name : ParseRuntime.hpp
//
// Description :
//     공백·접두사 없이 전체 문자열을 검사하는 스칼라 파싱의 내부 C 호출 규약을 정의한다.
//     Define the internal C ABI for complete scalar parsing without whitespace or prefixes.
//
// ========================
#pragma once
#include <cstdint>

extern "C"
{
    // domain: 정수 0, 실수 1, bool 2, char 3, byte 4. 실패 시 output은 변경하지 않는다.
    // Domains: integer 0, float 1, bool 2, char 3, byte 4. Failure leaves output unchanged.
    // 상태 1~5는 EmptyInput, InvalidSyntax, OutOfRange, NonFinite, InvalidScalar다.
    // Status 1 through 5 denotes EmptyInput, InvalidSyntax, OutOfRange, NonFinite, InvalidScalar.
    std::int32_t aevix_parse_scalar(const void* data, std::uint64_t length, std::int32_t domain,
                                    std::int32_t bits, std::int32_t isSigned, void* output) noexcept;
}
