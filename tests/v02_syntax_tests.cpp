// ========================
//
// Created By : miruku_sora2026
//
// File name : v02_syntax_tests.cpp
//
// Description :
//     숫자 접미사, 마지막 쉼표, 반복 배열과 투명 별칭의 구문 및 이름 연결을 검사한다.
//     Check numeric suffixes, trailing commas, repeated arrays, and transparent alias syntax and name resolution.
//
// ========================

#include "aevix/Common/SourceManager.hpp"
#include "aevix/Frontend/Lexer/Lexer.hpp"
#include "aevix/Frontend/Parser/Parser.hpp"
#include "aevix/Frontend/Sema/NameResolver.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    unsigned checks = 0;
    unsigned failures = 0;

    void expect(bool condition, std::string_view name)
    {
        ++checks;
        if (!condition) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
    }

    struct Fixture final
    {
        aevix::SourceManager sources;
        aevix::Diagnostics diagnostics;
        aevix::ModuleGraph graph;
        std::vector<aevix::Token> tokens;

        aevix::Node& add(std::string name, std::string source)
        {
            const auto id = sources.add(name + ".avx", std::move(source));
            tokens = aevix::Lexer(sources.get(id), diagnostics).tokenize();
            auto module = std::make_unique<aevix::SourceModule>();
            module->name = std::move(name);
            module->source = id;
            module->ast = aevix::Parser(tokens, diagnostics).parseModule();
            graph.modules.push_back(std::move(module));
            graph.entry = 0;
            return *graph.modules.back()->ast;
        }

        bool resolve()
        {
            if (!diagnostics.empty()) return false;
            return aevix::NameResolver(diagnostics).resolve(graph).succeeded;
        }

        bool has(std::string_view message) const
        {
            for (const auto& diagnostic : diagnostics)
                if (diagnostic.message.find(message) != std::string::npos) return true;
            return false;
        }
    };

    const aevix::Node* find(const aevix::Node& node, std::string_view kind)
    {
        if (node.kind == kind) return &node;
        for (const auto& child : node.children)
            if (const auto* result = find(*child, kind)) return result;
        return nullptr;
    }

    void literalTests()
    {
        Fixture test;
        const auto id = test.sources.add("numbers.avx",
            "42i64 255u8 0xffu8 0b101i16 0o77usize 1_024i32 "
            "1.25f32 2e-3f64 42f32 1bf16 -128i8 0i32..10i32 0xff32");
        test.tokens = aevix::Lexer(test.sources.get(id), test.diagnostics).tokenize();
        expect(test.diagnostics.empty(), "typed integer and floating literal spellings accepted");
        expect(test.tokens.size() == 17, "typed literals preserve prefix minus and range token boundaries");
        expect(test.tokens[6].kind == aevix::TokenKind::Float
            && test.tokens[8].kind == aevix::TokenKind::Float
            && test.tokens[9].kind == aevix::TokenKind::Float,
            "float suffix identifies integer-looking decimal float tokens");
        expect(test.tokens[12].text == "0i32" && test.tokens[13].text == ".."
            && test.tokens[14].text == "10i32", "range suffix operands are separate tokens");
        expect(test.tokens[15].text == "0xff32"
            && test.tokens[15].kind == aevix::TokenKind::Integer,
            "hexadecimal digits retain greedy interpretation");
        for (const auto* text : {"1wat", "1.5i32", "42_i32", "1u256", "0b1f32", "2e+f32"})
        {
            Fixture invalid;
            const auto source = invalid.sources.add("invalid.avx", text);
            const auto tokens = aevix::Lexer(invalid.sources.get(source), invalid.diagnostics).tokenize();
            expect(!invalid.diagnostics.empty() && tokens.back().kind == aevix::TokenKind::End,
                "invalid numeric suffix advances and diagnoses");
        }
    }

    void parserTests()
    {
        Fixture test;
        auto& ast = test.add("main", R"(
            type UserId = u64;
            public type Position = Array<i32, 2,>;
            @bench public T identity<T,>(T value,) { return value; }
            i32 main() {
                i32 zeros[3] = [0; 3];
                Array<i32, 2,> pair = [1, 2,];
                Tuple<> empty;
                return identity<i32,>(42,);
            }
        )");
        expect(test.diagnostics.empty(), "aliases repeat arrays and trailing comma lists parse");
        expect(find(ast, "Bench") != nullptr, "@bench attaches a marker to a concrete generic function");
        const auto* repeat = find(ast, "ArrayRepeat");
        expect(repeat && repeat->children.size() == 2
            && repeat->children[0]->kind == "Literal" && repeat->children[0]->value == "0"
            && repeat->children[1]->kind == "Literal" && repeat->children[1]->value == "3",
            "repeated array AST has element then count");
        expect(ast.children[0]->kind == "TypeAlias" && ast.children[0]->value == "UserId"
            && ast.children[0]->children[0]->kind == "Type"
            && ast.children[0]->children[0]->value == "u64", "alias AST retains its target syntax");
        for (const auto* source : {
            "type A = ;", "type A<T> = T;", "mut type A = i32;",
            "@bench i32 value = 1;", "@bench type A = i32;", "@bench struct A {};",
            "@bench @bench i32 f() { return 1; }", "@unknown i32 f() { return 1; }",
            "@bench extern \"C\" i32 f();", "interface A { @bench i32 f(); };",
            "i32 main() { type A = i32; return 0; }",
            "i32 f(,);", "i32 main() { return f(,); }",
            "i32 main() { i32 a[2] = [1;]; return 0; }",
            "i32 main() { i32 a[2] = [1; 2,]; return 0; }"})
        {
            Fixture invalid;
            invalid.add("main", source);
            expect(!invalid.diagnostics.empty(), "malformed new syntax is rejected");
        }
    }

    void aliasTests()
    {
        {
            Fixture test;
            test.add("main", "import ids { UserId }; UserId copy(UserId value) { return value; }");
            test.add("ids", "public type UserId = Storage; private type Storage = u64;");
            expect(test.resolve(), "public alias resolves private underlying alias without import leakage");
        }
        {
            Fixture test;
            test.add("main", "import facade { PublicId }; PublicId copy(PublicId value) { return value; }");
            test.add("facade", "public import ids { PublicId };");
            test.add("ids", "public type PublicId = u64;");
            expect(test.resolve(), "transparent aliases support explicit reexport");
        }
        {
            Fixture test;
            test.add("main", "import ids { HiddenId }; i32 main() { return 0; }");
            test.add("ids", "private type HiddenId = u64;");
            expect(!test.resolve() && test.has("private"), "private alias imports are rejected");
        }
        {
            Fixture test;
            test.add("main", "type Id = i32; i32 choose(Id value) { return value; } i32 choose(i32 other) { return other; }");
            expect(!test.resolve() && test.has("transparent type alias"), "aliases cannot create distinct overload parameter types");
        }
        {
            Fixture test;
            test.add("main", "type A = B; type B = &A;");
            expect(!test.resolve() && test.has("Cyclic type alias"), "reference indirection does not hide an alias cycle");
        }
        {
            Fixture test;
            test.add("main", "import other { B }; public type A = B;");
            test.add("other", "import main { A }; public type B = Array<A, 2>;");
            expect(!test.resolve() && test.has("Cyclic type alias"), "alias cycles across imports are diagnosed");
        }
        {
            Fixture test;
            test.add("main", R"(
                type Alias = Target;
                struct Target { public i32 value; private i32 hidden; public i32 make() { return 1; } };
                i32 main() { Alias a = Alias { value = 1 }; return Alias.make(); }
            )");
            expect(test.resolve(), "alias constructors and associated members resolve to their nominal target");
        }
        {
            Fixture test;
            test.add("main", "type Alias = Target; struct Target { private i32 hidden; }; i32 main() { Alias a = Alias { hidden = 1 }; return 0; }");
            expect(!test.resolve() && test.has("private field"), "aliases preserve private initializer access checks");
        }
        {
            Fixture test;
            test.add("main", "type Id = i32; struct Id {}; ");
            expect(!test.resolve() && test.has("Duplicate declaration"), "aliases share the declaration namespace");
        }
    }
}

int main()
{
    literalTests();
    parserTests();
    aliasTests();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
