// ========================
//
// Created By : miruku_sora2026
//
// File name : TextRuntime.hpp
//
// Description :
//     UTF-8 문자열의 검증·복사·수정에 사용하는 안정적인 C 호출 규약을 정의한다.
//     Define the stable C ABI for validating, copying, and modifying UTF-8 text.
//
// ========================
#pragma once
#include <cstddef>
#include <cstdint>

extern "C"
{
    // 상태 0은 성공, 1~4는 할당·크기·UTF-8·경계 오류다.
    // Status zero means success; 1 through 4 mean allocation, size, UTF-8, and boundary errors.
    std::int32_t aevix_text_copy(void* output, const void* data, std::uint64_t length, std::int32_t validate);
    std::int32_t aevix_text_append(void* owner, const void* data, std::uint64_t length);
    std::int32_t aevix_text_push_char(void* owner, std::uint32_t scalar);
    std::uint64_t aevix_text_count(const void* data, std::uint64_t length);
    std::uint32_t aevix_text_decode(const void* data, std::uint64_t length, std::uint64_t offset);
    std::int32_t aevix_text_boundary(const void* data, std::uint64_t length, std::uint64_t offset);
    std::int32_t aevix_text_equal(const void* left, std::uint64_t leftLength, const void* right, std::uint64_t rightLength);
}
