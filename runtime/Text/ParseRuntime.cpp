// ========================
//
// Created By : miruku_sora2026
//
// File name : ParseRuntime.cpp
//
// Description :
//     할당·로캘·예외 없이 정수·실수·논리·Unicode 스칼라와 바이트를 엄격히 파싱한다.
//     Strictly parse integer, floating, boolean, Unicode scalar, and byte values without allocation, locale, or exceptions.
//
// ========================
#include "aevix/Runtime/ParseRuntime.hpp"
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

namespace
{
    constexpr std::int32_t empty = 1, syntax = 2, range = 3, nonfinite = 4, scalarError = 5;
    bool decimal(char value) noexcept { return value >= '0' && value <= '9'; }

    std::int32_t integer(std::string_view text, unsigned bits, bool signedValue, void* output) noexcept
    {
        bool negative = false;
        std::size_t at = 0;
        if (text[0] == '+' || text[0] == '-')
        { negative = text[0] == '-'; at = 1; }
        if (at == text.size() || (negative && !signedValue)) return syntax;
        // 형식 오류를 범위 오류보다 먼저 보고하여 잘못된 접미사를 놓치지 않는다.
        // Validate the entire spelling before range checks so invalid suffixes remain syntax errors.
        for (auto i = at; i < text.size(); ++i) if (!decimal(text[i])) return syntax;
        std::array<std::uint32_t, 4> words{};
        for (; at < text.size(); ++at)
        {
            std::uint64_t carry = static_cast<unsigned>(text[at] - '0');
            for (auto& word : words)
            { const auto next = std::uint64_t(word) * 10 + carry; word = std::uint32_t(next); carry = next >> 32; }
            if (carry) return range;
        }
        const unsigned magnitudeBits = bits - (signedValue ? 1 : 0);
        std::array<std::uint32_t, 4> maximum{};
        for (unsigned i = 0; i < magnitudeBits; ++i) maximum[i / 32] |= std::uint32_t(1) << (i % 32);
        if (negative)
        { maximum = {}; maximum[(bits - 1) / 32] = std::uint32_t(1) << ((bits - 1) % 32); }
        for (unsigned i = 4; i-- > 0;)
            if (words[i] != maximum[i])
            { if (words[i] > maximum[i]) return range; break; }
        if (negative)
        {
            std::uint64_t carry = 1;
            for (auto& word : words)
            { const auto next = std::uint64_t(~word) + carry; word = std::uint32_t(next); carry = next >> 32; }
        }
        const auto low = std::uint64_t(words[0]) | (std::uint64_t(words[1]) << 32);
        if (bits == 8) { const auto value = std::uint8_t(low); std::memcpy(output, &value, 1); }
        else if (bits == 16) { const auto value = std::uint16_t(low); std::memcpy(output, &value, 2); }
        else if (bits == 32) { const auto value = std::uint32_t(low); std::memcpy(output, &value, 4); }
        else if (bits == 64) std::memcpy(output, &low, 8);
        else
        {
            static_assert(std::endian::native == std::endian::little, "Initial Aevix targets use little-endian i128 storage.");
            const auto high = std::uint64_t(words[2]) | (std::uint64_t(words[3]) << 32);
            std::memcpy(output, &low, 8); std::memcpy(static_cast<unsigned char*>(output) + 8, &high, 8);
        }
        return 0;
    }

    bool floatingSyntax(std::string_view text) noexcept
    {
        std::size_t at = (text[0] == '+' || text[0] == '-') ? 1 : 0;
        std::size_t digits = 0;
        while (at < text.size() && decimal(text[at])) { ++at; ++digits; }
        if (at < text.size() && text[at] == '.')
        { ++at; while (at < text.size() && decimal(text[at])) { ++at; ++digits; } }
        if (!digits) return false;
        if (at < text.size() && (text[at] == 'e' || text[at] == 'E'))
        {
            ++at;
            if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
            const auto start = at;
            while (at < text.size() && decimal(text[at])) ++at;
            if (at == start) return false;
        }
        return at == text.size();
    }

    template <typename Float>
    std::int32_t floating(std::string_view text, void* output) noexcept
    {
        auto bare = text;
        if (bare[0] == '+' || bare[0] == '-') bare.remove_prefix(1);
        const auto equalsAscii = [&](std::string_view expected)
        {
            if (bare.size() != expected.size()) return false;
            for (std::size_t i = 0; i < bare.size(); ++i)
            { const auto c = bare[i]; if ((c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c) != expected[i]) return false; }
            return true;
        };
        if (equalsAscii("inf") || equalsAscii("infinity") || equalsAscii("nan")) return nonfinite;
        if (!floatingSyntax(text)) return syntax;
        if (text[0] == '+') text.remove_prefix(1);
        Float value{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
        if (parsed.ec == std::errc::result_out_of_range) return range;
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return syntax;
        if (!std::isfinite(value)) return nonfinite;
        std::memcpy(output, &value, sizeof(value)); return 0;
    }

    std::int32_t character(std::string_view text, void* output) noexcept
    {
        const auto first = static_cast<unsigned char>(text[0]);
        unsigned count = 0; std::uint32_t value = 0, minimum = 0;
        if (first < 0x80) { count = 1; value = first; }
        else if (first >= 0xc2 && first <= 0xdf) { count = 2; value = first & 31; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 3; value = first & 15; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 4; value = first & 7; minimum = 0x10000; }
        else return scalarError;
        if (text.size() != count) return scalarError;
        for (unsigned i = 1; i < count; ++i)
        { const auto next = static_cast<unsigned char>(text[i]); if ((next & 0xc0) != 0x80) return scalarError; value = (value << 6) | (next & 63); }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return scalarError;
        std::memcpy(output, &value, sizeof(value)); return 0;
    }
}

extern "C" std::int32_t aevix_parse_scalar(const void* data, std::uint64_t length, std::int32_t domain,
                                         std::int32_t bits, std::int32_t isSigned, void* output) noexcept
{
    if (!output || (!data && length) || length > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max())) return syntax;
    if (isSigned != 0 && isSigned != 1) return syntax;
    const bool integerWidth = bits == 8 || bits == 16 || bits == 32 || bits == 64 || bits == 128;
    if ((domain == 0 && !integerWidth) || (domain == 1 && bits != 32 && bits != 64)
        || (domain == 2 && bits != 1) || (domain == 3 && bits != 32) || (domain == 4 && bits != 8)
        || domain < 0 || domain > 4 || (domain >= 2 && isSigned != 0)) return syntax;
    if (!length) return empty;
    const std::string_view text(static_cast<const char*>(data), static_cast<std::size_t>(length));
    if (domain == 0 || domain == 4) return integer(text, static_cast<unsigned>(bits), isSigned != 0, output);
    if (domain == 1) return bits == 32 ? floating<float>(text, output) : floating<double>(text, output);
    if (domain == 2)
    {
        if (text != "true" && text != "false") return syntax;
        const std::uint8_t value = text == "true" ? 1 : 0; std::memcpy(output, &value, 1); return 0;
    }
    return character(text, output);
}
