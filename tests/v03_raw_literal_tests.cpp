// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_raw_literal_tests.cpp
//
// Description :
//     원시 문자열의 구분자, 원본 바이트, UTF-8 진단 및 일반 토큰 경계를 검사한다.
//     Check raw string delimiters, exact source bytes, UTF-8 diagnostics, and ordinary token boundaries.
//
// ========================

#include "aevix/Common/SourceManager.hpp"
#include "aevix/Frontend/Lexer/Lexer.hpp"
#include "aevix/Frontend/Lexer/LiteralText.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    unsigned checks = 0;
    unsigned failures = 0;

    void expect(bool condition, std::string_view description)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::cerr << "FAIL: " << description << '\n';
        }
    }

    struct Fixture final
    {
        aevix::SourceManager sources;
        aevix::Diagnostics diagnostics;
        aevix::SourceId source;
        std::vector<aevix::Token> tokens;

        explicit Fixture(std::string text)
            : source(sources.add("raw.avx", std::move(text))),
              tokens(aevix::Lexer(sources.get(source), diagnostics).tokenize())
        {
        }

        bool has(std::string_view message) const
        {
            for (const auto& diagnostic : diagnostics)
                if (diagnostic.message.find(message) != std::string::npos)
                    return true;
            return false;
        }

        bool validRanges() const
        {
            for (const auto& diagnostic : diagnostics)
                if (!sources.contains(diagnostic.range)) return false;
            for (const auto& token : tokens)
                if (!sources.contains(token.range)) return false;
            return true;
        }
    };

    void spellingTests()
    {
        for (const auto& [spelling, contents] : std::vector<std::pair<std::string, std::string>>{
            {"r\"\"", ""},
            {"r\"C:\\temp\\main.avx\"", "C:\\temp\\main.avx"},
            {"r#\"say \"hello\"\"#", "say \"hello\""},
            {"r##\"a \"# marker\"##", "a \"# marker"},
            {"r#\"\\n \\t \\u{41} ${x} // text /* text */\"#", "\\n \\t \\u{41} ${x} // text /* text */"},
            {"r\"하늘 😀 é\"", "하늘 😀 é"},
            {"r\"one\r\ntwo\nthree\rfour\t\"", "one\r\ntwo\nthree\rfour\t"},
            {std::string("r\"a\0b\"", 6), std::string("a\0b", 3)}})
        {
            Fixture fixture(spelling + ";");
            expect(fixture.diagnostics.empty(), "valid raw literal has no diagnostics");
            expect(fixture.tokens.size() == 3
                && fixture.tokens[0].kind == aevix::TokenKind::String
                && fixture.tokens[0].text == spelling
                && fixture.tokens[1].text == ";", "raw literal stays one exact source token");
            expect(fixture.validRanges() && fixture.tokens[0].range.begin.offset == 0
                && fixture.tokens[0].range.end.offset == spelling.size(), "raw source span uses exact byte offsets");
            const auto view = aevix::rawStringContents(fixture.tokens[0].text);
            expect(view && *view == contents, "raw contents preserve bytes without processing escapes");
            expect(view && view->data() >= fixture.sources.get(fixture.source).text.data()
                && view->data() <= fixture.sources.get(fixture.source).text.data() + spelling.size(),
                "raw contents borrow their source buffer");
            expect(aevix::isStringLiteralSpelling(spelling)
                && aevix::isRawStringLiteral(spelling), "raw spellings share string classification");
        }
    }

    void delimiterTests()
    {
        for (const std::size_t count : {std::size_t{1}, std::size_t{2}, std::size_t{254}, std::size_t{255}})
        {
            const std::string hashes(count, '#');
            const std::string contents = "before\"" + std::string(count - 1, '#') + "inside";
            const std::string spelling = "r" + hashes + "\"" + contents + "\"" + hashes;
            Fixture fixture(spelling);
            expect(fixture.diagnostics.empty() && fixture.tokens.size() == 2
                && fixture.tokens[0].kind == aevix::TokenKind::String,
                "matching delimiters through 255 hashes are accepted");
            expect(aevix::rawStringContents(fixture.tokens[0].text) == contents,
                "quotes with insufficient trailing hashes remain raw contents");
        }

        {
            const std::string hashes(256, '#');
            Fixture fixture("r" + hashes + "\"text\"" + hashes + ";next");
            expect(fixture.has("exceeds 255") && fixture.tokens.size() == 4
                && fixture.tokens[0].kind == aevix::TokenKind::Invalid
                && fixture.tokens[1].text == ";" && fixture.tokens[2].text == "next",
                "excessive delimiters diagnose and recover after the complete literal");
            expect(fixture.validRanges(), "excessive delimiter diagnostics retain valid source ranges");
        }
        for (const auto* source : {"r\"", "r#\"missing\"", "r##\"missing\"#", "r#\"line\nnext"})
        {
            Fixture fixture(source);
            expect(fixture.has("Unterminated raw string") && fixture.tokens.size() == 2
                && fixture.tokens[0].kind == aevix::TokenKind::Invalid
                && fixture.tokens.back().kind == aevix::TokenKind::End,
                "unterminated and mismatched raw strings consume to EOF");
            expect(fixture.validRanges(), "unterminated raw literal diagnostic stays inside its source");
        }
        {
            Fixture fixture("r#\"value\"##;");
            expect(fixture.tokens[0].text == "r#\"value\"#"
                && fixture.tokens[1].text == "#" && fixture.tokens[1].kind == aevix::TokenKind::Invalid,
                "hashes beyond the closing delimiter remain separate source tokens");
        }
    }

    void unicodeAndBoundaryTests()
    {
        for (const auto& bytes : std::vector<std::string>{
            std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
            std::string("\xf4\x90\x80\x80", 4), std::string("\x80", 1),
            std::string("\xe2\x82", 2)})
        {
            Fixture fixture("r\"" + bytes + "\"; next");
            expect(fixture.has("Invalid UTF-8 byte sequence in raw string")
                && fixture.tokens[0].kind == aevix::TokenKind::Invalid
                && fixture.tokens[1].text == ";" && fixture.tokens[2].text == "next",
                "invalid Unicode scalars reject the raw token while preserving following tokens");
            expect(fixture.validRanges(), "invalid UTF-8 diagnostic covers existing bytes");
        }
        {
            Fixture fixture("/* lead */r\"a\r\nb\";\r\nnext");
            expect(fixture.diagnostics.empty() && fixture.tokens[0].comments.size() == 1,
                "leading comments attach to raw literals");
            const auto location = fixture.sources.locate(fixture.tokens[2].range.begin);
            expect(location.line == 3 && location.column == 1,
                "multiline raw literals preserve subsequent source line locations");
        }
        {
            Fixture fixture("r reader r0 ar\"ordinary\" \"escaped\\n\" 'r' r\"raw\\n\"");
            expect(fixture.diagnostics.empty() && fixture.tokens.size() == 9
                && fixture.tokens[0].text == "r" && fixture.tokens[1].text == "reader"
                && fixture.tokens[2].text == "r0" && fixture.tokens[3].text == "ar"
                && fixture.tokens[4].text == "\"ordinary\""
                && fixture.tokens[5].kind == aevix::TokenKind::String
                && fixture.tokens[6].kind == aevix::TokenKind::Character
                && fixture.tokens[7].text == "r\"raw\\n\"",
                "raw prefixes do not change identifiers ordinary strings or character literals");
        }
        {
            Fixture fixture("r#\"a\"#r\"b\"");
            expect(fixture.diagnostics.empty() && fixture.tokens.size() == 3
                && fixture.tokens[0].text == "r#\"a\"#" && fixture.tokens[1].text == "r\"b\"",
                "adjacent raw literals retain independent boundaries");
        }
    }

    void helperTests()
    {
        expect(aevix::isStringLiteralSpelling("\"ordinary\"")
            && !aevix::isRawStringLiteral("\"ordinary\""), "ordinary literals share only string classification");
        for (const auto* spelling : {"", "r", "reader", "r#", "r#value#", "r\"", "r#\"x\"", "r##\"x\"#"})
            expect(!aevix::rawStringContents(spelling), "raw content helper rejects malformed delimiters");
        const std::string hashes(256, '#');
        expect(!aevix::rawStringContents("r" + hashes + "\"x\"" + hashes),
            "raw content helper enforces the delimiter limit");
    }

    // CMake에서 표현할 수 없는 NUL과 잘못된 UTF-8을 실행 검사 소스에 그대로 쓴다.
    // Write NUL and malformed UTF-8 bytes, which CMake cannot represent, into execution fixtures.
    bool writeCliFixtures(const std::filesystem::path& directory)
    {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return false;
        const auto write = [&](std::string_view name, const std::string& source)
        {
            std::ofstream output(directory / name, std::ios::binary);
            output.write(source.data(), static_cast<std::streamsize>(source.size()));
            return static_cast<bool>(output);
        };

        const std::string zero(1, '\0');
        const std::string nulSource = "const string_view global_raw = r\"a" + zero + "b\";\n"
            "i32 main() {\n"
            "    string_view local_raw = r#\"a" + zero + "b\"#;\n"
            "    if (string.byte_len(local_raw) != 3 || string.scalar_count(local_raw) != 3) return 1;\n"
            "    if (!string.equals(local_raw, \"a\\0b\") || !string.equals(global_raw, local_raw)) return 2;\n"
            "    mut i32 zeros = 0;\n"
            "    for (char scalar : local_raw) { if (scalar == '\\0') zeros++; }\n"
            "    return zeros == 1 ? 0 : 3;\n"
            "}\n";
        if (!write("embedded_nul.avx", nulSource)) return false;

        unsigned sequence = 0;
        for (const auto& bytes : std::vector<std::string>{
            std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
            std::string("\xf4\x90\x80\x80", 4), std::string("\x80", 1),
            std::string("\xe2\x82", 2)})
        {
            if (!write("invalid_utf8_" + std::to_string(sequence++) + ".avx",
                "i32 main() { string_view invalid = r\"" + bytes + "\"; return 0; }\n"))
                return false;
        }
        return true;
    }
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string_view(argv[1]) == "--write-cli-fixtures")
        return writeCliFixtures(argv[2]) ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc != 1) return EXIT_FAILURE;
    spellingTests();
    delimiterTests();
    unicodeAndBoundaryTests();
    helperTests();
    std::cout << checks << " raw literal checks, " << failures << " failures.\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
