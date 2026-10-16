// ========================
//
// Created By : miruku_sora2026
//
// File name : Parser.hpp
//
// Description :
//     토큰을 모듈 구문 트리로 변환하고 구문 진단을 수집하는 파서 인터페이스를 선언한다.
//     Declare the parser interface for converting tokens into a module syntax tree with diagnostics.
//
// ========================

#pragma once

#include "aevix/Common/Diagnostic.hpp"
#include "aevix/Frontend/Lexer/Token.hpp"
#include "aevix/Frontend/Parser/AST.hpp"

#include <vector>

namespace aevix {
    /// 구문 분석은 이름 연결 및 타입 검사와 분리한다.
    /// Parsing is separate from name resolution and type checking.
    class Parser final
    {
    public:
        Parser(std::vector<Token> tokens, Diagnostics& diagnostics);

        [[nodiscard]] NodePtr parseModule();

    private:
        std::vector<Token> tokens_;
        Diagnostics& diagnostics_;
    };
}
