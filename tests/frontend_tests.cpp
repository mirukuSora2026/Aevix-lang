// ========================
//
// Created By : miruku_sora2026
//
// File name : frontend_tests.cpp
//
// Description :
//     소스 위치, 토큰 및 언어 구문에 대한 프런트엔드 회귀 검사를 실행한다.
//     Run frontend regression checks for source locations, tokens, and language syntax.
//
// ========================

#include "aevix/Common/SourceManager.hpp"
#include "aevix/Common/Diagnostic.hpp"
#include "aevix/Frontend/Lexer/Lexer.hpp"
#include "aevix/Frontend/Parser/Parser.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int checks = 0;
int failures = 0;

// 실패한 조건과 검사 이름을 출력한다.
// Print a failed condition and the test name.
void expect(bool condition, std::string_view name) {
    ++checks;

    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}

// 토큰과 AST를 검사하는 동안 임시 소스가 살아 있도록 유지한다.
// Keep temporary source storage alive while inspecting tokens and the AST.
struct Fixture {
    aevix::SourceManager sources;
    aevix::Diagnostics diagnostics;
    std::vector<aevix::Token> tokens;
    aevix::NodePtr tree;

    explicit Fixture(std::string text, bool parse = true) {
        const auto id = sources.add(
            "fixture.avx",
            std::move(text)
        );

        tokens = aevix::Lexer(
            sources.get(id),
            diagnostics
        ).tokenize();

        if (parse)
            tree = aevix::Parser(
                tokens,
                diagnostics
            ).parseModule();
    }
};

bool validRanges(
    const aevix::Node& node,
    const aevix::SourceManager& sources
) {
    if (!sources.contains(node.range)) return false;

    for (const auto& child : node.children) {
        if (
            !child
            || child->range.begin.source != node.range.begin.source
            || child->range.begin.offset < node.range.begin.offset
            || child->range.end.offset > node.range.end.offset
            || !validRanges(*child, sources)
        ) {
            return false;
        }
    }

    return true;
}

void valid(std::string_view name, std::string source) {
    Fixture fixture(std::move(source));

    if (!fixture.diagnostics.empty()) {
        for (const auto& diagnostic : fixture.diagnostics)
            std::cerr << name << ": " << diagnostic.message << '\n';
    }

    expect(
        fixture.tree != nullptr && fixture.diagnostics.empty(),
        name
    );

    if (fixture.tree && fixture.diagnostics.empty())
        expect(
            validRanges(*fixture.tree, fixture.sources),
            "AST source ranges enclose children"
        );
}

void invalid(std::string_view name, std::string source) {
    Fixture fixture(source);

    if (fixture.diagnostics.empty())
        std::cerr << "UNEXPECTED ACCEPT: " << source << '\n';

    expect(!fixture.diagnostics.empty(), name);
}

bool hasComment(
    const aevix::Node& node,
    std::string_view needle
) {
    for (const auto& comment : node.comments)
        if (comment.find(needle) != std::string::npos)
            return true;

    for (const auto& child : node.children)
        if (child && hasComment(*child, needle))
            return true;

    return false;
}

void sourceTests() {
    aevix::SourceManager manager;

    const auto original = manager.add(
        "first.avx",
        "ab\n한글"
    );

    const auto* first = &manager.get(original);

    for (int index = 0; index < 100; ++index)
        manager.add("extra.avx", "i32 x;");

    expect(
        first == &manager.get(original),
        "source addresses remain stable"
    );

    expect(
        manager.contains({{original, 0}, {original, 9}}),
        "registered range accepted"
    );

    expect(
        !manager.contains({{original, 0}, {original, 10}}),
        "out-of-bounds range rejected"
    );

    expect(
        !manager.contains({{0, 0}, {0, 0}}),
        "invalid source ID rejected"
    );

    const auto mixed = manager.add("newlines.avx", "a\rb\r\nc\nd");
    for (const auto offset : {2u, 5u, 7u}) {
        const auto location = manager.locate({mixed, offset});
        expect(location.column == 1 && location.line == (offset == 2 ? 2u : offset == 5 ? 3u : 4u),
               "CR CRLF and LF line locations");
    }
    bool invalidOffset = false;
    try { (void)manager.locate({original, 10}); }
    catch (const std::out_of_range&) { invalidOffset = true; }
    expect(invalidOffset, "out-of-bounds location throws");

    const auto position = manager.locate({original, 6});

    expect(
        position.line == 2 && position.column == 4,
        "source locations use byte columns"
    );
}

void lexicalTests() {
    Fixture range("0..10 0..=10 1.25 2e3", false);

    const std::vector<std::string_view> expected = {
        "0", "..", "10",
        "0", "..=", "10",
        "1.25", "2e3", ""
    };

    expect(
        range.diagnostics.empty(),
        "range and float lexing succeeds"
    );

    expect(
        range.tokens.size() == expected.size(),
        "range and float token count"
    );

    for (
        std::size_t i = 0;
        i < range.tokens.size() && i < expected.size();
        ++i
    ) {
        expect(
            range.tokens[i].text == expected[i],
            "range and float token spelling"
        );
    }

    expect(
        range.tokens[0].range.begin.offset == 0
            && range.tokens[0].range.end.offset == 1,
        "half-open token byte range"
    );

    expect(
        range.tokens.back().kind == aevix::TokenKind::End,
        "lexer always appends EOF"
    );

    Fixture numbers("0xff 0xABCD 0b101 0o755 1_024 1.5 1e+3 1e-3", false);
    expect(numbers.diagnostics.empty() && numbers.tokens.size() == 9,
           "numeric bases separators and exponents");
    Fixture comments("// first\ri32 x;\r\n// second\ni32 y;", false);
    unsigned declarations = 0;
    for (const auto& token : comments.tokens)
        if (token.text == "i32") ++declarations;
    expect(comments.diagnostics.empty() && declarations == 2,
           "line comments terminate at CR and LF");
    Fixture malformedNumber("123abc", false);
    expect(!malformedNumber.diagnostics.empty()
               && malformedNumber.tokens.back().kind == aevix::TokenKind::End,
           "malformed numeric suffix advances to EOF");

    Fixture quotes(
        R"("// not a comment" '\n' /* block */ "/* literal */")",
        false
    );

    expect(
        quotes.diagnostics.empty(),
        "quoted comment delimiters stay literal"
    );

    expect(
        quotes.tokens.size() == 4,
        "comments are absent from executable tokens"
    );

    Fixture unicode("\"한글\" '😀'", false);

    expect(
        unicode.diagnostics.empty(),
        "UTF-8 string and Unicode scalar character"
    );

    expect(
        unicode.tokens[0].range.end.offset == 8,
        "UTF-8 range counts bytes"
    );

    for (const auto source : {
        "/* unfinished",
        "\"unfinished",
        "'ab'",
        "'\\q'",
        "\"\\q\"",
        ".5",
        "1.",
        "0x",
        "1__2"
    }) {
        Fixture broken(source, false);

        expect(
            !broken.diagnostics.empty(),
            "invalid literal or comment diagnosed"
        );

        expect(
            broken.tokens.back().kind == aevix::TokenKind::End,
            "invalid literal reaches EOF"
        );
    }

    std::string malformed = "\"";
    malformed += static_cast<char>(0xc0);
    malformed += static_cast<char>(0xaf);
    malformed += "\"";

    Fixture badUtf8(std::move(malformed), false);

    expect(
        !badUtf8.diagnostics.empty(),
        "overlong UTF-8 encoding rejected"
    );

    std::string malformedComment = "// ";
    malformedComment += static_cast<char>(0xff);

    Fixture badComment(std::move(malformedComment), false);

    expect(
        !badComment.diagnostics.empty(),
        "invalid UTF-8 in comments rejected"
    );
}

void declarationTests() {
    valid("compound intrinsic type operands", R"(
        usize inspect() {
            usize bytes = Box.payload_size(Array<i32, 4>);
            Vector<&dyn Named> views = Vector.new(&dyn Named);
            return bytes;
        }
    )");

    valid("all import forms", R"(
        import math;
        import math.vector { Point, dot };
        import tensor { Tensor };
        import collections { * };
        public import geometry { Point };
        i32 main() { return 0; }
    )");

    valid("variable declaration forms", R"(
        const i32 limit = 3;
        persist i32 saved = 7;

        i32 main() {
            i32 uninitialized;
            mut i32 counter = 0;
            rsn inferred = 42;
            &i32 view = &counter;
            &mut i32 writable = &mut counter;

            counter += 1;
            ++counter;
            counter--;

            return counter;
        }
    )");

    valid("arrays and nested generic types", R"(
        i32 main() {
            i32 fixed[3] = [1, 2, 3];
            i32 grid[2][3] = [[1, 2, 3], [4, 5, 6]];
            i32 dynamic[] = [1, 2, 3]?;
            i32 rows[][] = [[1], [2, 3]]?;
            i32 rank[](2) = [[1], [2, 3]]?;
            i32 mixed[2][] = [[1], [2, 3]]?;

            Vector<Vector<i32>> nested = make();

            math.vector.Pair<i32> qualified =
                math.vector.Pair<i32> { first = 1, second = 2 };

            Array<i32, 4> sized = Array.from_copy(i32, 4, &fixed);

            return 0;
        }
    )");

    valid("generics named calls defaults and packs", R"(
        T identity<T>(T value) where T implements Copy {
            return value;
        }

        i32 scale(i32 value, i32 factor = 2) {
            return value * factor;
        }

        void receive(bool enabled, i32 values[]) {
            return;
        }

        void inspect(&string values[]) {
            return;
        }

        Array<i32, 3> relay(i32 values[3]) {
            return values;
        }

        i32 main() {
            i32 value = identity<i32>(10);
            i32 chosen = scale(factor = 3, value = 10);
            receive(true, 1, 2, 3);
            return chosen;
        }
    )");

    valid("struct interface enum declarations", R"(
        interface Named {
            public i32 level(&Self self);

            public bool active(&Self self) {
                return self.level() > 0;
            }
        };

        struct Point<T> where T implements Copy {
            public T x;
            public T y;
        };

        struct User implements Named {
            i32 value;

            public User new(i32 value) {
                return User { value = value };
            }

            public i32 level(&User self) {
                return (*self).value;
            }

            public override bool active(&User self) {
                return true;
            }
        };

        enum Event {
            Quit;

            Move {
                public i32 x;
                public i32 y;
            };

            private Hidden;

            public Event make(i32 x) {
                return Event.Move { x = x, y = 0 };
            }
        };

        Point<i32> point = Point<i32> { x = 1, y = 2 };
        &dyn Named borrowed = &user;
        Box<dyn Named> owned = Box.into_dyn(concrete);
    )");

    Fixture documented(R"(/* @parameter value input;
        @return copied input;
        @version 0.1;
        @deprecated migrate;
        @deprecated_at 1.0;
        */
        i32 identity(i32 value) { return value; })");

    expect(
        documented.diagnostics.empty()
            && documented.tree
            && hasComment(*documented.tree, "@parameter"),
        "documentation is attached to an AST declaration"
    );
}

void controlFlowTests() {
    valid("conditionals loops and expression precedence", R"(
        i32 main() {
            mut i32 total = 0;

            if (true) total = 1; else total = 2;
            while (total < 3) ++total;

            for (i32 item in 0..10) {
                total += item;
            }

            for (mut i32 index = 0; index < 4; index++)
                total += index;

            for (char scalar : text) {
                continue;
            }

            total = (total << 1) | (3 & 1) ^ 2;
            total >>= 1;

            bool comparison = total >= 0 && total <= 100;

            total = true ? 1 : false ? 2 : 3;
            return total;
        }
    )");

    valid("enum switches and explicit terminal break", R"(
        i32 inspect(Event event) {
            switch (event) {
                case Event.Move { i32 dx = x, i32 dy = y }:
                    return dx + dy;
                    break;

                case Event.Message { &string message = &text }:
                    consume(message);
                    break;

                default:
                    break;
            }

            switch (&mut event) {
                case Event.Move { &mut i32 dx = &mut x }:
                    *dx += 1;
                    break;

                default:
                    break;
            }

            switch (move event) {
                case Event.Message { string message = text }:
                    consume(message);
                    break;

                default:
                    break;
            }

            return 0;
        }
    )");

    for (const auto expression : {
        "checked(a + b)? + c",
        "result? ? first : second",
        "flag ? make_result()? : fallback",
        "flag ? +x : y"
    }) {
        valid(
            "unique question-mark expression",
            "i32 f() { return " + std::string(expression) + "; }"
        );
    }

    invalid(
        "ambiguous question-mark expression",
        "i32 f() { return a ? + b ? + c : d; }"
    );

    invalid(
        "chained comparison",
        "bool f() { return a < b == true; }"
    );

    invalid(
        "chained range",
        "void f() { rsn range = 0..10..20; return; }"
    );

    invalid(
        "angle operators require adjacency",
        "bool f() { return a > = b; }"
    );

    valid(
        "generic type member access",
        "Vector<i32> f() { return Vector<i32>.new(); }"
    );

    valid(
        "nested propagation",
        "i32 f() { return call(checked(a + b)?, flag ? x : y); }"
    );

    valid(
        "parenthesized comparison",
        "bool f() { return (a < b) == true; }"
    );
}

void invalidSyntaxTests() {
    // 긴 비재귀 연산자 체인도 진단으로 끝나며 트리 소멸 시 넘치지 않는다.
    // Long iterative operator chains diagnose cleanly without overflowing destruction.
    for (const auto suffix : {" + a", ".field"}) {
        std::string source = "i32 f() { return a";

        for (unsigned i = 0; i < 20000; ++i)
            source += suffix;

        invalid("AST depth limit", source + "; }");
    }

    std::string genericChain = "i32 f() { return a";

    for (unsigned i = 0; i < 255; ++i)
        genericChain += ".field";

    invalid(
        "generic speculation preserves ownership",
        genericChain + "<i32>(); }"
    );

    for (const auto source : {
        "i32 main() { return 0 }",
        "i32 main() { if true return 0; return 1; }",
        "i32 main() { if (true) i32 value = 1; return 0; }",
        "i32 main() { return x = 1; }",
        "i32 main() { switch (1) { case 1: return 2; } return 0; }",
        "void f(i32 values[], bool enabled) { return; }",
        "void f(i32 first[], i32 second[]) { return; }",
        "void f(mut i32 values[]) { return; }",
        "void f(i32 value = 0, i32 values[]) { return; }",
        "public import math { * };",
        "i32 value = 1; import math;",
        "import math as m;",
        "i32 main() { call(value = 1, 2); return 0; }",
        "i32 main() { call(value = 1, value = 2); return 0; }",
        "struct Point { public i32 x; }"
    }) {
        invalid("malformed syntax diagnosed", source);
    }
}

}

int main() {
    sourceTests();
    lexicalTests();
    declarationTests();
    controlFlowTests();
    invalidSyntaxTests();

    std::cout << "checks=" << checks
              << " failures=" << failures << '\n';

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
