// ========================
//
// Created By : miruku_sora2026
//
// File name : SystemRuntime.cpp
//
// Description :
//     예외나 사용자 포인터 노출 없이 표준 입출력·로그·스칼라 서식화를 구현한다.
//     Implement standard IO, logging, and scalar formatting without exceptions or exposed user pointers.
//
// ========================
#include "aevix/Runtime/SystemRuntime.hpp"
#include "aevix/Runtime/TextRuntime.hpp"

#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <system_error>

namespace
{
    std::mutex inputMutex;
    std::mutex outputMutex;
    struct IoFailure { std::int32_t code = 0; std::uint64_t progress = 0; };
    thread_local IoFailure ioFailure;
    std::uint32_t failed(int fallback = EIO)
    { ioFailure.code = errno ? errno : fallback; return 1; }
    bool whitespace(int c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    }
    bool writeBytes(std::FILE* stream, const void* data, std::uint64_t length)
    {
        if ((!data && length) || length > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()))
        { ioFailure.code = EINVAL; return false; }
        if (!length) return true;
        errno = 0;
        const auto written = std::fwrite(data, 1, static_cast<std::size_t>(length), stream);
        ioFailure.progress += written;
        if (written != length) { failed(); return false; }
        return true;
    }
    std::uint32_t copyText(const void* bytes, std::size_t count, void** data, std::uint64_t* length, std::uint64_t* capacity)
    {
        if (!data || !length || !capacity) return 3;
        *data = nullptr; *length = 0; *capacity = 0;
        if (!count) return 0;
        auto* allocation = std::malloc(count);
        if (!allocation) return 1;
        std::memcpy(allocation, bytes, count);
        *data = allocation; *length = count; *capacity = count;
        return 0;
    }
    bool validUtf8(const unsigned char* bytes, std::uint64_t length)
    {
        for (std::uint64_t i = 0; i < length;)
        {
            const auto start = i;
            const auto c = bytes[i++];
            if (c < 0x80) continue;
            unsigned extra = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
            if (!extra || extra > length - i) return false;
            for (unsigned n = 0; n < extra; ++n) if ((bytes[i++] & 0xc0) != 0x80) return false;
            const auto scalar = aevix_text_decode(bytes, length, start);
            if (scalar < (extra == 1 ? 0x80U : extra == 2 ? 0x800U : 0x10000U)
                || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
        }
        return true;
    }
}

extern "C" std::uint32_t aevix_system_write(const void* data, std::uint64_t length, std::uint8_t channel, std::uint8_t newline)
{
    ioFailure = {};
    if (channel > 1 || newline > 1) { ioFailure.code = EINVAL; return 1; }
    std::lock_guard lock(outputMutex);
    auto* stream = channel ? stderr : stdout;
    if (!writeBytes(stream, data, length)) return 1;
    static const char lf = '\n';
    return newline && !writeBytes(stream, &lf, 1) ? 1 : 0;
}

extern "C" std::int32_t aevix_system_error_code() { return ioFailure.code; }
extern "C" std::uint64_t aevix_system_error_progress() { return ioFailure.progress; }

extern "C" std::uint32_t aevix_system_write_segments(const void* segments, std::uint64_t count, std::uint8_t newline)
{
    ioFailure = {};
    if ((!segments && count) || count > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) / 16 || newline > 1)
    { ioFailure.code = EINVAL; return 1; }
    struct Segment { const void* data; std::uint64_t length; };
    static_assert(sizeof(Segment) == 16);
    const auto* items = static_cast<const Segment*>(segments);
    // 모든 포인터·크기를 출력 전에 확인해 잘못된 내부 호출의 부분 출력을 피한다.
    // Check every pointer and length before output to avoid partial writes from malformed internal calls.
    for (std::uint64_t i = 0; i < count; ++i)
        if ((!items[i].data && items[i].length) || items[i].length > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()))
        { ioFailure.code = EINVAL; return 1; }
    std::lock_guard lock(outputMutex);
    for (std::uint64_t i = 0; i < count; ++i) if (!writeBytes(stdout, items[i].data, items[i].length)) return 1;
    static const char lf = '\n';
    return newline && !writeBytes(stdout, &lf, 1) ? 1 : 0;
}

extern "C" std::uint32_t aevix_system_flush(std::uint8_t channel)
{
    ioFailure = {};
    if (channel > 1) { ioFailure.code = EINVAL; return 1; }
    std::lock_guard lock(outputMutex);
    errno = 0;
    return std::fflush(channel ? stderr : stdout) == 0 ? 0 : failed();
}

extern "C" std::uint32_t aevix_system_log(std::uint8_t level, const void* message, std::uint64_t length,
    const void* code, std::uint64_t codeLength)
{
    ioFailure = {};
    if (level > 2 || (!message && length) || (!code && codeLength)) { ioFailure.code = EINVAL; return 1; }
    // 수준·메시지·코드를 한 잠금 아래 출력해 다른 언어 로그와 섞이지 않게 한다.
    // Emit the level, message, and code under one lock to keep language log records together.
    std::lock_guard lock(outputMutex);
    const char* labels[] = {"INFO", "WARN", "ERROR"};
    if (!writeBytes(stderr, "[", 1) || !writeBytes(stderr, labels[level], std::strlen(labels[level]))) return 1;
    if (codeLength && (!writeBytes(stderr, " ", 1) || !writeBytes(stderr, code, codeLength))) return 1;
    if (!writeBytes(stderr, "] ", 2) || !writeBytes(stderr, message, length) || !writeBytes(stderr, "\n", 1)) return 1;
    return 0;
}

extern "C" std::uint32_t aevix_system_input(std::uint8_t mode, void** data, std::uint64_t* length, std::uint64_t* capacity)
{
    ioFailure = {};
    if (data) *data = nullptr;
    if (length) *length = 0;
    if (capacity) *capacity = 0;
    if (!data || !length || !capacity || mode > 1) { ioFailure.code = EINVAL; return 4; }
    *data = nullptr; *length = 0; *capacity = 0;
    std::lock_guard lock(inputMutex);
    unsigned char* buffer = nullptr;
    std::uint64_t count = 0, reserved = 0;
    bool observed = false, endedWithNewline = false;
    errno = 0;
    int current = std::fgetc(stdin);
    if (mode) while (current != EOF && whitespace(current)) current = std::fgetc(stdin);
    while (current != EOF)
    {
        if ((!mode && current == '\n') || (mode && whitespace(current)))
        {
            endedWithNewline = !mode;
            // 숫자 토큰 뒤의 구분자는 보존한다. 다음 줄 입력도 C++ getline처럼 읽는다.
            // Preserve the token delimiter, so a following line read behaves like C++ getline.
            if (mode && std::ungetc(current, stdin) == EOF) { failed(); std::free(buffer); return 4; }
            observed = true; break;
        }
        observed = true;
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());
        if (count == maximum) { std::free(buffer); return 3; }
        if (count == reserved)
        {
            const auto next = !reserved ? 128 : reserved > maximum / 2 ? maximum : reserved * 2;
            auto* replacement = static_cast<unsigned char*>(std::realloc(buffer, static_cast<std::size_t>(next)));
            if (!replacement) { std::free(buffer); return 2; }
            buffer = replacement; reserved = next;
        }
        buffer[count++] = static_cast<unsigned char>(current); ++ioFailure.progress;
        current = std::fgetc(stdin);
    }
    if (current == EOF && std::ferror(stdin)) { failed(); std::free(buffer); return 4; }
    if (!observed) { std::free(buffer); return 1; }
    if (endedWithNewline && count && buffer[count - 1] == '\r') --count;
    if (!validUtf8(buffer, count)) { std::free(buffer); return 5; }
    *data = buffer; *length = count; *capacity = reserved;
    return 0;
}

extern "C" std::uint32_t aevix_system_format_scalar(const void* scalar, std::uint32_t kind, std::uint32_t bits,
    void** data, std::uint64_t* length, std::uint64_t* capacity)
{
    if (!data || !length || !capacity) return 3;
    *data = nullptr; *length = 0; *capacity = 0;
    if (!scalar) return 3;
    char buffer[512];
    std::size_t count = 0;
    if (kind == 5)
    {
        if (bits != 8) return 3;
        const auto value = *static_cast<const unsigned char*>(scalar);
        const char* hex = "0123456789abcdef";
        buffer[0] = '0'; buffer[1] = 'x'; buffer[2] = hex[value >> 4]; buffer[3] = hex[value & 15]; count = 4;
    }
    else if (kind == 0 || kind == 1)
    {
        if (bits != 8 && bits != 16 && bits != 32 && bits != 64 && bits != 128) return 3;
        unsigned char magnitude[16]{};
        std::memcpy(magnitude, scalar, bits / 8);
        // 지원 타깃은 little endian이다. 128비트도 확장 정수에 의존하지 않고 나눈다.
        // Supported targets are little endian. Divide even 128-bit values without compiler integer extensions.
        const bool negative = kind == 0 && (magnitude[bits / 8 - 1] & 0x80);
        if (negative)
        {
            unsigned carry = 1;
            for (unsigned i = 0; i < bits / 8; ++i)
            { const auto digit = static_cast<unsigned>(static_cast<unsigned char>(~magnitude[i])) + carry;
                magnitude[i] = static_cast<unsigned char>(digit); carry = digit >> 8; }
        }
        do
        {
            unsigned remainder = 0;
            for (unsigned i = bits / 8; i > 0; --i)
            { const auto digit = (remainder << 8) | magnitude[i - 1]; magnitude[i - 1] = static_cast<unsigned char>(digit / 10); remainder = digit % 10; }
            buffer[count++] = static_cast<char>('0' + remainder);
            bool nonzero = false;
            for (unsigned i = 0; i < bits / 8; ++i) nonzero |= magnitude[i] != 0;
            if (!nonzero) break;
        } while (true);
        if (negative) buffer[count++] = '-';
        for (std::size_t i = 0; i < count / 2; ++i)
        { const auto temp = buffer[i]; buffer[i] = buffer[count - i - 1]; buffer[count - i - 1] = temp; }
    }
    else if (kind == 2)
    {
        std::to_chars_result result;
        const auto special = [&](auto value) -> const char*
        { return std::isnan(value) ? "nan" : std::isinf(value) ? std::signbit(value) ? "-inf" : "inf" : nullptr; };
        const char* nonfinite = nullptr;
        if (bits == 32)
        {
            float value; std::memcpy(&value, scalar, sizeof(value)); nonfinite = special(value);
            if (!nonfinite) result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        }
        else if (bits == 64)
        {
            double value; std::memcpy(&value, scalar, sizeof(value)); nonfinite = special(value);
            if (!nonfinite) result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        }
        else return 3;
        if (nonfinite) return copyText(nonfinite, std::strlen(nonfinite), data, length, capacity);
        if (result.ec != std::errc{}) return 3;
        count = static_cast<std::size_t>(result.ptr - buffer);
    }
    else if (kind == 3)
    { if (bits != 1) return 3; const auto value = *static_cast<const unsigned char*>(scalar); if (value > 1) return 3; const char* text = value ? "true" : "false"; count = std::strlen(text); std::memcpy(buffer, text, count); }
    else if (kind == 4)
    {
        if (bits != 32) return 3;
        std::uint32_t value; std::memcpy(&value, scalar, sizeof(value));
        if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 3;
        if (value < 0x80) buffer[count++] = static_cast<char>(value);
        else if (value < 0x800) { buffer[count++] = static_cast<char>(0xc0 | (value >> 6)); buffer[count++] = static_cast<char>(0x80 | (value & 63)); }
        else if (value < 0x10000) { buffer[count++] = static_cast<char>(0xe0 | (value >> 12)); buffer[count++] = static_cast<char>(0x80 | ((value >> 6) & 63)); buffer[count++] = static_cast<char>(0x80 | (value & 63)); }
        else { buffer[count++] = static_cast<char>(0xf0 | (value >> 18)); buffer[count++] = static_cast<char>(0x80 | ((value >> 12) & 63)); buffer[count++] = static_cast<char>(0x80 | ((value >> 6) & 63)); buffer[count++] = static_cast<char>(0x80 | (value & 63)); }
    }
    else return 3;
    return copyText(buffer, count, data, length, capacity);
}

extern "C" std::uint32_t aevix_system_output(const void* parts, std::uint64_t count, std::uint8_t mode,
    std::uint8_t level, const void* code, std::uint64_t codeLength)
{
    ioFailure = {};
    struct Part { const void* data; std::uint64_t length; std::uint32_t kind; std::uint32_t bits; };
    struct Segment { const void* data; std::uint64_t length; void* allocation; };
    static_assert(sizeof(Part) == 24 && sizeof(Segment) == 24);
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());
    if (mode > 2 || (mode == 2 && level > 2) || (!parts && count)
        || (!code && codeLength) || codeLength > maximum || count > maximum / sizeof(Segment))
    { ioFailure.code = EINVAL; return 4; }
    const auto* input = static_cast<const Part*>(parts);
    Segment local[16]{};
    auto* segments = count <= 16 ? local : static_cast<Segment*>(std::calloc(static_cast<std::size_t>(count), sizeof(Segment)));
    if (!segments) return 2;
    const auto cleanup = [&]()
    {
        for (std::uint64_t i = 0; i < count; ++i) std::free(segments[i].allocation);
        if (segments != local) std::free(segments);
    };
    // 모든 값의 서식화를 먼저 끝낸다. 실패하면 이 호출은 아직 출력하지 않았다.
    // Finish formatting every value first. On failure this call has not written output.
    for (std::uint64_t i = 0; i < count; ++i)
    {
        if (input[i].kind == 6)
        {
            if ((!input[i].data && input[i].length) || input[i].length > maximum) { cleanup(); return 4; }
            segments[i] = {input[i].data, input[i].length, nullptr};
        }
        else
        {
            void* formatted = nullptr; std::uint64_t length = 0, capacity = 0;
            const auto status = aevix_system_format_scalar(input[i].data, input[i].kind, input[i].bits, &formatted, &length, &capacity);
            if (status) { cleanup(); return status == 1 ? 2 : status == 2 ? 3 : 4; }
            segments[i] = {formatted, length, formatted};
        }
    }
    std::uint32_t status = 0;
    {
        std::lock_guard lock(outputMutex);
        auto* stream = mode == 2 ? stderr : stdout;
        if (mode == 2)
        {
            const char* labels[] = {"INFO", "WARN", "ERROR"};
            if (!writeBytes(stream, "[", 1) || !writeBytes(stream, labels[level], std::strlen(labels[level]))
                || (codeLength && (!writeBytes(stream, " ", 1) || !writeBytes(stream, code, codeLength)))
                || !writeBytes(stream, "] ", 2)) status = 1;
        }
        for (std::uint64_t i = 0; i < count && !status; ++i)
            if (!writeBytes(stream, segments[i].data, segments[i].length)) status = 1;
        if (mode && !status && !writeBytes(stream, "\n", 1)) status = 1;
    }
    cleanup();
    return status;
}
