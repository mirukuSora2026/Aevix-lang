// ========================
//
// Created By : miruku_sora2026
//
// File name : fuzz_frontend.cpp
//
// Description :
//     재현 가능한 무작위 토큰 입력으로 렉서와 파서를 실행하고 진단 범위의 유효성을 검사한다.
//     Exercise the lexer and parser with reproducible random token input and validate diagnostic ranges.
//
// ========================

#include "aevix/Common/SourceManager.hpp"
#include "aevix/Frontend/Lexer/Lexer.hpp"
#include "aevix/Frontend/Parser/Parser.hpp"

#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

int main() {
    std::mt19937 random(20261015);

    const std::vector<std::string_view> pieces = {
        "i32", "f", "a", "return", "for", "while",
        "case", "break", "switch", "struct",
        "interface", "enum", "mut", "import", "public", "in",
        "0", "1", "[]", "(", ")", "{", "}", "[", "]",
        "?", ":", ";", ",", ".", "=", "+", "-", "*", "&", "<", ">"
    };

    for (unsigned test = 0; test < 6000; ++test) {
        std::string input;
        const unsigned count = random() % 100;

        for (unsigned n = 0; n < count; ++n) {
            input += pieces[random() % pieces.size()];
            input += ' ';
        }

        aevix::SourceManager sources;

        const auto id = sources.add(
            "fuzz.avx",
            std::move(input)
        );

        aevix::Diagnostics diagnostics;

        auto tokens = aevix::Lexer(
            sources.get(id),
            diagnostics
        ).tokenize();

        auto tree = aevix::Parser(
            std::move(tokens),
            diagnostics
        ).parseModule();

        (void)tree;

        for (const auto& diagnostic : diagnostics) {
            if (!sources.contains(diagnostic.range)) {
                std::cerr
                    << "out-of-bounds diagnostic at case "
                    << test << '\n';

                return 1;
            }
        }
    }

    std::cout << "random token cases=6000 passed\n";
}
