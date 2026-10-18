// ========================
//
// Created By : miruku_sora2026
//
// File name : SystemRuntime.hpp
//
// Description :
//     표준 스트림의 동기 입출력과 로그에 사용하는 길이 제한 C 호출 규약을 정의한다.
//     Define bounded C calls for synchronous standard-stream IO and logging.
//
// ========================
#pragma once
#include <cstdint>

extern "C"
{
    // 채널 0은 stdout, 1은 stderr다. 상태 0은 성공, 1은 쓰기 실패다.
    // Channel zero is stdout, one is stderr. Status zero is success, one is write failure.
    std::uint32_t aevix_system_write(const void* data, std::uint64_t length, std::uint8_t channel, std::uint8_t newline);
    std::uint32_t aevix_system_flush(std::uint8_t channel);
    std::uint32_t aevix_system_log(std::uint8_t level, const void* message, std::uint64_t length,
        const void* code, std::uint64_t codeLength);
    std::int32_t aevix_system_error_code();
    std::uint64_t aevix_system_error_progress();
    // 각 항목은 { const void* data; uint64_t length; }이며 호출 동안만 대여한다.
    // Each item is { const void* data; uint64_t length; }, borrowed only for this call.
    std::uint32_t aevix_system_write_segments(const void* segments, std::uint64_t count, std::uint8_t newline);
    // 항목은 { const void* data; uint64_t length; uint32_t kind; uint32_t bits; }다.
    // Each part is { const void* data; uint64_t length; uint32_t kind; uint32_t bits; }.
    // kind 6은 텍스트다. mode는 print=0, println=1, log=2다.
    // Kind six is text. Mode is print=0, println=1, log=2.
    std::uint32_t aevix_system_output(const void* parts, std::uint64_t count, std::uint8_t mode,
        std::uint8_t level, const void* code, std::uint64_t codeLength);

    // mode 0은 줄, 1은 공백 구분 토큰이다. 성공한 버퍼는 호출자가 free로 해제한다.
    // Mode zero reads a line, one a whitespace-delimited token. The caller frees a successful buffer.
    // 상태: 성공, EOF, 할당 실패, 크기 초과, 읽기 실패, UTF-8 오류.
    // Status: success, EOF, allocation failure, size overflow, read failure, invalid UTF-8.
    std::uint32_t aevix_system_input(std::uint8_t mode, void** data, std::uint64_t* length, std::uint64_t* capacity);

    // kind: 부호 정수, 무부호 정수, 실수, bool, char, byte. 성공한 출력은 소유 문자열이다.
    // Kind: signed integer, unsigned integer, float, bool, char, byte. Successful output is owned text.
    // 상태: 성공, 할당 실패, 크기 초과, 지원하지 않는 값/형식.
    // Status: success, allocation failure, size overflow, unsupported value/format.
    std::uint32_t aevix_system_format_scalar(const void* scalar, std::uint32_t kind, std::uint32_t bits,
        void** data, std::uint64_t* length, std::uint64_t* capacity);
}
