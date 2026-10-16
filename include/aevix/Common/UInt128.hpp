// ========================
//
// Created By : miruku_sora2026
//
// File name : UInt128.hpp
//
// Description :
//     플랫폼 전용 정수 확장 없이 두 64비트 워드로 128비트 연산을 구현한다.
//     Implement 128-bit arithmetic with two 64-bit words and no platform-specific integer extensions.
//
// ========================
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <algorithm>

namespace aevix
{
    struct UInt128 final
    {
        std::uint64_t low = 0;
        std::uint64_t high = 0;
        constexpr UInt128() = default;
        constexpr UInt128(std::uint64_t lowWord, std::uint64_t highWord = 0) : low(lowWord), high(highWord) {}
        constexpr explicit operator bool() const { return low || high; }
        friend constexpr bool operator==(UInt128 a, UInt128 b) { return a.low == b.low && a.high == b.high; }
        friend constexpr bool operator!=(UInt128 a, UInt128 b) { return !(a == b); }
        friend constexpr bool operator<(UInt128 a, UInt128 b) { return a.high != b.high ? a.high < b.high : a.low < b.low; }
        friend constexpr bool operator>(UInt128 a, UInt128 b) { return b < a; }
        friend constexpr bool operator<=(UInt128 a, UInt128 b) { return !(b < a); }
        friend constexpr bool operator>=(UInt128 a, UInt128 b) { return !(a < b); }
        friend constexpr UInt128 operator~(UInt128 a) { return {~a.low, ~a.high}; }
        friend constexpr UInt128 operator&(UInt128 a, UInt128 b) { return {a.low & b.low, a.high & b.high}; }
        friend constexpr UInt128 operator|(UInt128 a, UInt128 b) { return {a.low | b.low, a.high | b.high}; }
        friend constexpr UInt128 operator^(UInt128 a, UInt128 b) { return {a.low ^ b.low, a.high ^ b.high}; }
        friend constexpr UInt128 operator+(UInt128 a, UInt128 b) { const auto low = a.low + b.low; return {low, a.high + b.high + (low < a.low)}; }
        friend constexpr UInt128 operator-(UInt128 a, UInt128 b) { return {a.low - b.low, a.high - b.high - (a.low < b.low)}; }
        friend constexpr UInt128 operator<<(UInt128 a, unsigned count)
        {
            if (!count) return a;
            if (count >= 128) return {};
            if (count >= 64) return {0, a.low << (count - 64)};
            return {a.low << count, (a.high << count) | (a.low >> (64 - count))};
        }
        friend constexpr UInt128 operator>>(UInt128 a, unsigned count)
        {
            if (!count) return a;
            if (count >= 128) return {};
            if (count >= 64) return {a.high >> (count - 64), 0};
            return {(a.low >> count) | (a.high << (64 - count)), a.high >> count};
        }
        static constexpr UInt128 mask(unsigned bits)
        {
            return bits >= 128 ? UInt128{~std::uint64_t{0}, ~std::uint64_t{0}} : (UInt128{1} << bits) - UInt128{1};
        }
        static constexpr bool multiply(UInt128 a, UInt128 b, UInt128& result)
        {
            const std::uint32_t left[4] = {static_cast<std::uint32_t>(a.low), static_cast<std::uint32_t>(a.low >> 32), static_cast<std::uint32_t>(a.high), static_cast<std::uint32_t>(a.high >> 32)};
            const std::uint32_t right[4] = {static_cast<std::uint32_t>(b.low), static_cast<std::uint32_t>(b.low >> 32), static_cast<std::uint32_t>(b.high), static_cast<std::uint32_t>(b.high >> 32)};
            std::uint32_t words[8]{};
            for (unsigned i = 0; i < 4; ++i)
            {
                std::uint64_t carry = 0;
                for (unsigned j = 0; j < 4; ++j)
                {
                    const auto partial = static_cast<std::uint64_t>(left[i]) * right[j] + words[i + j] + carry;
                    words[i + j] = static_cast<std::uint32_t>(partial); carry = partial >> 32;
                }
                words[i + 4] = static_cast<std::uint32_t>(carry);
            }
            result = {static_cast<std::uint64_t>(words[0]) | (static_cast<std::uint64_t>(words[1]) << 32), static_cast<std::uint64_t>(words[2]) | (static_cast<std::uint64_t>(words[3]) << 32)};
            return words[4] || words[5] || words[6] || words[7];
        }
        friend constexpr UInt128 operator*(UInt128 a, UInt128 b) { UInt128 result; multiply(a, b, result); return result; }
        static constexpr bool divide(UInt128 dividend, UInt128 divisor, UInt128& quotient, UInt128& remainder)
        {
            quotient = {}; remainder = {};
            if (!divisor) return false;
            if (dividend < divisor) { remainder = dividend; return true; }
            for (unsigned index = 128; index > 0; --index)
            {
                const bool carry = (remainder.high >> 63) != 0;
                remainder = (remainder << 1) | ((dividend >> (index - 1)) & UInt128{1});
                if (carry || remainder >= divisor)
                { remainder = remainder - divisor; quotient = quotient | (UInt128{1} << (index - 1)); }
            }
            return true;
        }
        friend constexpr UInt128 operator/(UInt128 a, UInt128 b) { UInt128 quotient, remainder; divide(a, b, quotient, remainder); return quotient; }
        friend constexpr UInt128 operator%(UInt128 a, UInt128 b) { UInt128 quotient, remainder; divide(a, b, quotient, remainder); return remainder; }
        static bool parse(std::string_view text, UInt128& value, unsigned base = 10)
        {
            value = {};
            if (base < 2 || base > 16 || text.empty()) return false;
            bool digitSeen = false;
            for (const auto character : text)
            {
                if (character == '_') continue;
                const unsigned digit = character >= '0' && character <= '9' ? static_cast<unsigned>(character - '0') : character >= 'a' && character <= 'f' ? static_cast<unsigned>(character - 'a' + 10) : character >= 'A' && character <= 'F' ? static_cast<unsigned>(character - 'A' + 10) : base;
                if (digit >= base) return false;
                UInt128 product;
                if (multiply(value, UInt128{base}, product)) return false;
                const auto next = product + UInt128{digit};
                if (next < product) return false;
                value = next; digitSeen = true;
            }
            return digitSeen;
        }
        std::string decimal() const
        {
            if (!*this) return "0";
            std::string result;
            auto remaining = *this;
            while (remaining)
            {
                UInt128 quotient, remainder; divide(remaining, UInt128{10}, quotient, remainder);
                result.push_back(static_cast<char>('0' + remainder.low)); remaining = quotient;
            }
            std::reverse(result.begin(), result.end()); return result;
        }
    };
}
