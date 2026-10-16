// ========================
//
// Created By : miruku_sora2026
//
// File name : TokenKind.hpp
//
// Description :
//     식별자, 리터럴, 기호, 잘못된 입력 및 입력 끝을 구분하는 토큰 종류를 정의한다.
//     Define token categories for identifiers, literals, symbols, invalid input, and end of input.
//
// ========================

#pragma once

namespace aevix
{
    /// 키워드는 파서가 철자로 구분하므로 렉서는 식별자로 보존한다.
    /// The parser recognizes keyword spellings, so the lexer preserves them as identifiers.
    enum class TokenKind
    {
        Identifier,
        Integer,
        Float,
        String,
        Character,
        Symbol,
        Invalid,
        End
    };
}
