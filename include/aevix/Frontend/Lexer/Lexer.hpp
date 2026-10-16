// ========================
//
// Created By : miruku_sora2026
//
// File name : Lexer.hpp
//
// Description :
//     소스를 토큰으로 나누고 주석과 진단을 수집하는 렉서 인터페이스를 선언한다.
//     Declare the lexer interface for tokenizing source and collecting comments and diagnostics.
//
// ========================

#pragma once

#include "aevix/Common/Diagnostic.hpp"
#include "aevix/Common/SourceManager.hpp"
#include "aevix/Frontend/Lexer/Token.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace aevix
{
    /// 주석은 다음 토큰에 붙이며 잘못된 입력에서도 EOF까지 전진한다.
    /// Comments attach to the next token, and malformed input still advances to EOF.
    class Lexer final
    {
    public:
        Lexer(const SourceFile& source, Diagnostics& diagnostics);

        [[nodiscard]] std::vector<Token> tokenize();

    private:
        const SourceFile& source_;
        Diagnostics& diagnostics_;
        std::size_t position_ = 0;
        std::vector<std::string_view> comments_;

        [[nodiscard]] char peek(std::size_t ahead = 0) const;

        [[nodiscard]] SourceRange range(
            std::size_t begin,
            std::size_t end
        ) const;

        void error(
            std::size_t begin,
            std::size_t end,
            std::string message
        );

        void trivia();

        [[nodiscard]] Token token(TokenKind kind, std::size_t begin);
        [[nodiscard]] Token number(bool missingInteger = false);
        [[nodiscard]] Token quoted();
    };
}
