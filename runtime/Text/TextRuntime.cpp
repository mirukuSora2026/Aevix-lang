// ========================
//
// Created By : miruku_sora2026
//
// File name : TextRuntime.cpp
//
// Description :
//     숨은 공유나 예외 없이 UTF-8 소유 문자열과 읽기 뷰의 기본 연산을 구현한다.
//     Implement UTF-8 owners and read views without hidden sharing or exceptions.
//
// ========================
#include "aevix/Runtime/TextRuntime.hpp"
#include <cstdlib>
#include <cstring>
#include <limits>

namespace
{
    struct Text { unsigned char* data; std::uint64_t length; std::uint64_t capacity; };
    static_assert(sizeof(Text) == 24 && offsetof(Text, length) == 8 && offsetof(Text, capacity) == 16);
    bool valid(const unsigned char* bytes, std::uint64_t length)
    {
        if (!bytes && length) return false;
        for (std::uint64_t index = 0; index < length;)
        {
            const auto first = bytes[index++];
            if (first < 0x80) continue;
            unsigned count = 0; std::uint32_t scalar = 0, minimum = 0;
            if (first >= 0xc2 && first <= 0xdf) { count = 1; scalar = first & 31; minimum = 0x80; }
            else if (first >= 0xe0 && first <= 0xef) { count = 2; scalar = first & 15; minimum = 0x800; }
            else if (first >= 0xf0 && first <= 0xf4) { count = 3; scalar = first & 7; minimum = 0x10000; }
            else return false;
            if (count > length - index) return false;
            while (count--)
            {
                const auto next = bytes[index++];
                if ((next & 0xc0) != 0x80) return false;
                scalar = (scalar << 6) | (next & 63);
            }
            if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
        }
        return true;
    }
}

extern "C" std::int32_t aevix_text_copy(void* output, const void* data, std::uint64_t length, std::int32_t validate)
{
    if (!output || (!data && length)) return 3;
    if (length > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max())) return 2;
    if (validate && !valid(static_cast<const unsigned char*>(data), length)) return 3;
    auto* bytes = length ? static_cast<unsigned char*>(std::malloc(static_cast<std::size_t>(length))) : nullptr;
    if (length && !bytes) return 1;
    if (length) std::memcpy(bytes, data, static_cast<std::size_t>(length));
    *static_cast<Text*>(output) = {bytes, length, length};
    return 0;
}

extern "C" std::int32_t aevix_text_append(void* owner, const void* data, std::uint64_t length)
{
    if (!owner || (!data && length)) return 3;
    auto& text = *static_cast<Text*>(owner);
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());
    if (text.length > maximum || length > maximum - text.length) return 2;
    if (!length) return 0;
    const auto required = text.length + length;
    if (required <= text.capacity)
    {
        std::memmove(text.data + text.length, data, static_cast<std::size_t>(length));
        text.length = required; return 0;
    }
    const auto doubled = text.capacity > maximum / 2 ? maximum : text.capacity * 2;
    const auto capacity = required > doubled ? required : doubled;
    auto* replacement = static_cast<unsigned char*>(std::malloc(static_cast<std::size_t>(capacity)));
    if (!replacement) return 1;
    if (text.length) std::memcpy(replacement, text.data, static_cast<std::size_t>(text.length));
    std::memcpy(replacement + text.length, data, static_cast<std::size_t>(length));
    std::free(text.data); text = {replacement, required, capacity};
    return 0;
}

extern "C" std::int32_t aevix_text_push_char(void* owner, std::uint32_t scalar)
{
    if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return 3;
    unsigned char encoded[4]; std::uint64_t length = 0;
    if (scalar < 0x80) encoded[length++] = static_cast<unsigned char>(scalar);
    else if (scalar < 0x800) { encoded[length++] = 0xc0 | (scalar >> 6); encoded[length++] = 0x80 | (scalar & 63); }
    else if (scalar < 0x10000) { encoded[length++] = 0xe0 | (scalar >> 12); encoded[length++] = 0x80 | ((scalar >> 6) & 63); encoded[length++] = 0x80 | (scalar & 63); }
    else { encoded[length++] = 0xf0 | (scalar >> 18); encoded[length++] = 0x80 | ((scalar >> 12) & 63); encoded[length++] = 0x80 | ((scalar >> 6) & 63); encoded[length++] = 0x80 | (scalar & 63); }
    return aevix_text_append(owner, encoded, length);
}

extern "C" std::uint64_t aevix_text_count(const void* data, std::uint64_t length)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint64_t count = 0;
    for (std::uint64_t index = 0; index < length; ++index) count += (bytes[index] & 0xc0) != 0x80;
    return count;
}

extern "C" std::uint32_t aevix_text_decode(const void* data, std::uint64_t length, std::uint64_t offset)
{
    if (offset >= length) return 0xffffffff;
    const auto* bytes = static_cast<const unsigned char*>(data);
    const auto first = bytes[offset++];
    if (first < 0x80) return first;
    unsigned count = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
    std::uint32_t scalar = first & (count == 1 ? 31 : count == 2 ? 15 : 7);
    if (count > length - offset) return 0xffffffff;
    while (count--) scalar = (scalar << 6) | (bytes[offset++] & 63);
    return scalar;
}

extern "C" std::int32_t aevix_text_boundary(const void* data, std::uint64_t length, std::uint64_t offset)
{
    return offset <= length && (offset == length || (static_cast<const unsigned char*>(data)[offset] & 0xc0) != 0x80);
}

extern "C" std::int32_t aevix_text_equal(const void* left, std::uint64_t leftLength, const void* right, std::uint64_t rightLength)
{
    return leftLength == rightLength && (!leftLength || std::memcmp(left, right, static_cast<std::size_t>(leftLength)) == 0);
}
