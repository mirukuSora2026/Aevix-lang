// ========================
//
// Created By : miruku_sora2026
//
// File name : Token.hpp
//
// Description :
//     원본 소스를 대여하는 토큰의 종류, 텍스트, 범위 및 연결된 주석을 정의한다.
//     Define tokens with a kind, borrowed source text, source range, and attached comments.
//
// ========================

#pragma once

#include "aevix/Common/SourceLocation.hpp"
#include "aevix/Frontend/Lexer/TokenKind.hpp"

#include <string_view>
#include <vector>

namespace aevix
{
    /// 토큰과 주석은 원본 소스를 빌리므로 소스가 더 오래 살아 있어야 한다.
    /// Tokens and comments borrow the original source, which must outlive them.
    struct Token final
    {
        TokenKind kind = TokenKind::End;
        std::string_view text;
        SourceRange range;
        std::vector<std::string_view> comments;
    };
}
