// ========================
//
// Created By : miruku_sora2026
//
// File name : Lexer.cpp
//
// Description :
//     주석, 숫자 및 문자열 리터럴을 토큰화하고 잘못된 문자와 UTF-8 입력을 진단한다.
//     Tokenize comments, numbers, and text literals, and diagnose invalid characters and UTF-8 input.
//
// ========================

#include "aevix/Frontend/Lexer/Lexer.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace aevix
{
    namespace
    {
        bool digit(char c)
        {
            return c >= '0' && c <= '9';
        }

        bool letter(char c)
        {
            return (c >= 'a' && c <= 'z')
                || (c >= 'A' && c <= 'Z')
                || c == '_';
        }

        bool identifier(char c)
        {
            return letter(c) || digit(c);
        }

        int digitValue(char c)
        {
            if (digit(c)) return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        bool scalar(std::uint32_t value)
        {
            return value <= 0x10ffff
                && !(value >= 0xd800 && value <= 0xdfff);
        }

        // 잘못된 UTF-8에서도 최소 한 바이트를 소비해 무한 반복을 막는다.
        // Consume at least one byte even for invalid UTF-8 to prevent infinite loops.
        bool utf8(std::string_view text, std::size_t& position)
        {
            const auto first = static_cast<unsigned char>(text[position++]);

            if (first < 0x80) return true;

            unsigned remaining = 0;
            std::uint32_t value = 0;
            std::uint32_t minimum = 0;

            if (first >= 0xc2 && first <= 0xdf)
            {
                remaining = 1;
                value = first & 0x1f;
                minimum = 0x80;
            }
            else if (first >= 0xe0 && first <= 0xef)
            {
                remaining = 2;
                value = first & 0x0f;
                minimum = 0x800;
            }
            else if (first >= 0xf0 && first <= 0xf4)
            {
                remaining = 3;
                value = first & 0x07;
                minimum = 0x10000;
            }
            else
            {
                return false;
            }

            for (unsigned i = 0; i < remaining; ++i)
            {
                if (position == text.size()) return false;

                const auto next = static_cast<unsigned char>(text[position]);

                if ((next & 0xc0) != 0x80) return false;

                ++position;
                value = (value << 6) | (next & 0x3f);
            }

            return value >= minimum && scalar(value);
        }
    }

    Lexer::Lexer(
        const SourceFile& source,
        Diagnostics& diagnostics
    )
        : source_(source),
          diagnostics_(diagnostics)
    {
    }

    char Lexer::peek(std::size_t ahead) const
    {
        return ahead < source_.text.size() - position_
                   ? source_.text[position_ + ahead]
                   : '\0';
    }

    SourceRange Lexer::range(
        std::size_t begin,
        std::size_t end
    ) const
    {
        return {
            {source_.id, static_cast<std::uint64_t>(begin)},
            {source_.id, static_cast<std::uint64_t>(end)}
        };
    }

    void Lexer::error(
        std::size_t begin,
        std::size_t end,
        std::string message
    )
    {
        diagnostics_.push_back({
            range(begin, end),
            std::move(message)
        });
    }

    Token Lexer::token(TokenKind kind, std::size_t begin)
    {
        Token result{
            kind,
            std::string_view(source_.text).substr(
                begin,
                position_ - begin
            ),
            range(begin, position_),
            std::move(comments_)
        };

        comments_.clear();
        return result;
    }

    void Lexer::trivia()
    {
        const auto commentCharacter = [&]
        {
            if (static_cast<unsigned char>(peek()) < 0x80)
            {
                ++position_;
            }
            else
            {
                const auto begin = position_;

                if (!utf8(source_.text, position_))
                    error(
                        begin,
                        position_,
                        "Invalid UTF-8 byte sequence in comment."
                    );
            }
        };

        while (position_ < source_.text.size())
        {
            if (peek() == ' ' || peek() == '\t'
                || peek() == '\r' || peek() == '\n')
            {
                ++position_;
                continue;
            }

            if (peek() != '/' || (peek(1) != '/' && peek(1) != '*'))
                return;

            const std::size_t begin = position_;
            const bool block = peek(1) == '*';
            position_ += 2;

            if (!block)
            {
                while (position_ < source_.text.size()
                    && peek() != '\n' && peek() != '\r')
                    commentCharacter();
            }
            else
            {
                while (position_ < source_.text.size()
                    && !(peek() == '*' && peek(1) == '/'))
                    commentCharacter();

                if (position_ == source_.text.size())
                {
                    error(begin, position_, "Unterminated block comment.");
                }
                else
                {
                    position_ += 2;
                }
            }

            comments_.push_back(
                std::string_view(source_.text).substr(
                    begin,
                    position_ - begin
                )
            );
        }
    }

    Token Lexer::number(bool missingInteger)
    {
        const std::size_t begin = position_;
        bool valid = !missingInteger;
        bool floating = missingInteger;
        int base = 10;

        if (missingInteger)
        {
            ++position_;
        }
        else if (peek() == '0')
        {
            const char prefix = peek(1);

            if (prefix == 'x' || prefix == 'X') base = 16;
            else if (prefix == 'b' || prefix == 'B') base = 2;
            else if (prefix == 'o' || prefix == 'O') base = 8;

            if (base != 10) position_ += 2;
        }

        // 밑줄은 유효한 두 숫자 사이에서만 허용한다.
        // Underscores are allowed only between two valid digits.
        const auto digits = [&](int radix)
        {
            bool any = false;
            bool previous = false;

            while (position_ < source_.text.size())
            {
                const int value = digitValue(peek());

                if (value >= 0 && value < radix)
                {
                    any = true;
                    previous = true;
                    ++position_;
                }
                else if (peek() == '_')
                {
                    const int next = digitValue(peek(1));

                    if (!previous || next < 0 || next >= radix)
                        valid = false;

                    previous = false;
                    ++position_;
                }
                else
                {
                    break;
                }
            }

            if (!any) valid = false;
        };

        digits(base);

        if (base == 10)
        {
            if (!missingInteger && peek() == '.' && peek(1) != '.')
            {
                floating = true;
                ++position_;
                digits(10);
            }

            if (peek() == 'e' || peek() == 'E')
            {
                floating = true;
                ++position_;

                if (peek() == '+' || peek() == '-')
                    ++position_;

                digits(10);
            }
        }

        if (identifier(peek()))
        {
            const auto suffixStart = position_;
            while (identifier(peek())) ++position_;
            const auto suffix = std::string_view(source_.text).substr(
                suffixStart, position_ - suffixStart);
            const bool integerSuffix = suffix == "i8" || suffix == "i16"
                || suffix == "i32" || suffix == "i64" || suffix == "i128" || suffix == "u8"
                || suffix == "u16" || suffix == "u32" || suffix == "u64" || suffix == "u128"
                || suffix == "isize" || suffix == "usize";
            const bool floatSuffix = suffix == "f16" || suffix == "bf16"
                || suffix == "f32" || suffix == "f64";

            // 접미사는 숫자 철자에 포함하며 범위 검사는 타입 검사 단계에 맡긴다.
            // Keep the suffix in the token spelling; type checking validates its range.
            if (floatSuffix && base == 10) floating = true;
            else if (!integerSuffix || floating) valid = false;
        }

        if (!valid)
            error(begin, position_, "Invalid numeric literal or type suffix.");

        return token(
            valid
                ? (floating ? TokenKind::Float : TokenKind::Integer)
                : TokenKind::Invalid,
            begin
        );
    }

    Token Lexer::quoted()
    {
        const std::size_t begin = position_;
        const char delimiter = source_.text[position_++];

        bool valid = true;
        bool closed = false;
        std::size_t characters = 0;

        while (position_ < source_.text.size())
        {
            if (peek() == delimiter)
            {
                ++position_;
                closed = true;
                break;
            }

            if (peek() == '\n' || peek() == '\r')
                break;

            if (peek() == '\\')
            {
                ++position_;

                if (position_ == source_.text.size()
                    || peek() == '\n' || peek() == '\r')
                    break;

                const char escape = source_.text[position_++];

                if (escape == 'u')
                {
                    if (peek() != '{')
                    {
                        valid = false;
                        continue;
                    }

                    ++position_;

                    std::uint32_t value = 0;
                    unsigned count = 0;

                    while (digitValue(peek()) >= 0)
                    {
                        if (count < 6)
                            value = (value << 4)
                                | static_cast<unsigned>(digitValue(peek()));

                        ++count;
                        ++position_;
                    }

                    if (peek() == '}')
                    {
                        ++position_;
                    }
                    else
                    {
                        valid = false;
                    }

                    if (count == 0 || count > 6 || !scalar(value))
                        valid = false;
                }
                else if (
                    std::string_view("\\\"'nrt0").find(escape)
                    == std::string_view::npos
                )
                {
                    valid = false;
                }
            }
            else
            {
                if (static_cast<unsigned char>(peek()) < 0x20)
                    valid = false;

                if (!utf8(source_.text, position_))
                    valid = false;
            }

            ++characters;
        }

        if (!closed)
        {
            valid = false;
            error(begin, position_, "Unterminated quoted literal.");
        }
        else if (delimiter == '\'' && characters != 1)
        {
            valid = false;
            error(
                begin,
                position_,
                "Character literal must contain exactly one Unicode scalar."
            );
        }
        else if (!valid)
        {
            error(
                begin,
                position_,
                "Invalid escape, Unicode scalar, or control byte in literal."
            );
        }

        return token(
            valid
                ? (delimiter == '\''
                       ? TokenKind::Character
                       : TokenKind::String)
                : TokenKind::Invalid,
            begin
        );
    }

    std::vector<Token> Lexer::tokenize()
    {
        std::vector<Token> tokens;

        while (true)
        {
            trivia();

            const std::size_t begin = position_;

            if (position_ == source_.text.size())
            {
                tokens.push_back(token(TokenKind::End, begin));
                return tokens;
            }

            if (letter(peek()))
            {
                do
                {
                    ++position_;
                } while (identifier(peek()));

                tokens.push_back(token(TokenKind::Identifier, begin));
            }
            else if (
                digit(peek())
                 || (peek() == '.' && digit(peek(1)))
            )
            {
                tokens.push_back(number(peek() == '.'));
            }
            else if (peek() == '\'' || peek() == '"')
            {
                tokens.push_back(quoted());
            }
            else
            {
                // 각괄호는 합치지 않아 중첩 제네릭의 닫는 괄호를 보존한다.
                // Keep angle brackets separate to preserve nested generic closers.
                bool compound = false;

                for (const std::string_view symbol : {
                    "..=", "..", "++", "--", "->",
                    "+=", "-=", "*=", "/=", "%=",
                    "&=", "|=", "^=", "==", "!=", "&&", "||"
                })
                {
                    if (std::string_view(source_.text)
                            .substr(position_)
                            .starts_with(symbol))
                    {
                        position_ += symbol.size();
                        compound = true;
                        break;
                    }
                }

                if (
                    compound
                    || std::string_view("(){}[];,.?:~!+-*/%&|^=<>@")
                        .find(peek()) != std::string_view::npos
                )
                {
                    if (!compound) ++position_;
                    tokens.push_back(token(TokenKind::Symbol, begin));
                }
                else
                {
                    const bool validUtf8 = utf8(source_.text, position_);

                    error(
                        begin,
                        position_,
                        validUtf8
                            ? "Unsupported source character; identifiers currently use ASCII."
                            : "Invalid UTF-8 byte sequence."
                    );

                    tokens.push_back(token(TokenKind::Invalid, begin));
                }
            }
        }
    }
}
