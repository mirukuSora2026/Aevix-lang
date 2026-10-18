// ========================
//
// Created By : miruku_sora2026
//
// File name : LiteralText.hpp
//
// Description :
//     렉서가 검증한 일반 문자열과 원시 문자열의 철자를 구분하고 원시 본문을 대여한다.
//     Classify lexer-validated ordinary and raw string spellings and borrow raw contents.
//
// ========================

#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace aevix
{
    inline constexpr std::size_t MaxRawStringHashes = 255;

    /// 이스케이프 처리나 줄바꿈 정규화 없이 원시 리터럴의 소스 바이트를 반환한다.
    /// Return raw literal source bytes without escape processing or newline normalization.
    /// UTF-8과 본문 안의 닫는 구분자는 호출 전에 렉서가 검사해야 한다.
    /// The lexer must validate UTF-8 and closing delimiters within the contents before this call.
    [[nodiscard]] constexpr std::optional<std::string_view>
    rawStringContents(std::string_view spelling) noexcept
    {
        if (spelling.size() < 3 || spelling.front() != 'r')
            return std::nullopt;

        std::size_t opening = 1;
        while (opening < spelling.size() && spelling[opening] == '#')
            ++opening;

        const auto hashes = opening - 1;
        if (hashes > MaxRawStringHashes || opening == spelling.size()
            || spelling[opening] != '"'
            || spelling.size() < opening + 2 + hashes)
            return std::nullopt;

        const auto closing = spelling.size() - hashes - 1;
        if (spelling[closing] != '"') return std::nullopt;
        for (std::size_t index = closing + 1; index < spelling.size(); ++index)
            if (spelling[index] != '#') return std::nullopt;

        return spelling.substr(opening + 1, closing - opening - 1);
    }

    [[nodiscard]] constexpr bool isRawStringLiteral(std::string_view spelling) noexcept
    {
        return rawStringContents(spelling).has_value();
    }

    [[nodiscard]] constexpr bool isStringLiteralSpelling(std::string_view spelling) noexcept
    {
        return spelling.starts_with('"') || isRawStringLiteral(spelling);
    }
}
