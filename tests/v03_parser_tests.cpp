// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_parser_tests.cpp
//
// Description :
//     필드 초기화 축약의 구문 확장과 이름 검사 및 잘못된 override 위치를 검사한다.
//     Check field initializer shorthand expansion, name checks, and invalid override contexts.
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
        if (!condition)
        {
            ++failures;
            std::cerr << "FAIL: " << name << '\n';
        }
    }

    struct Fixture final
    {
        aevix::SourceManager sources;
        aevix::Diagnostics diagnostics;
        aevix::ModuleGraph graph;

        aevix::Node& parse(std::string source)
        {
            const auto id = sources.add("main.avx", std::move(source));
            auto tokens = aevix::Lexer(sources.get(id), diagnostics).tokenize();
            auto module = std::make_unique<aevix::SourceModule>();
            module->name = "main";
            module->source = id;
            module->ast = aevix::Parser(std::move(tokens), diagnostics).parseModule();
            graph.modules.push_back(std::move(module));
            graph.entry = 0;
            return *graph.modules.back()->ast;
        }

        bool resolve()
        {
            if (!diagnostics.empty()) return false;
            return aevix::NameResolver(diagnostics).resolve(graph).succeeded;
        }

        bool has(std::string_view text) const
        {
            for (const auto& diagnostic : diagnostics)
                if (diagnostic.message.find(text) != std::string::npos) return true;
            return false;
        }

        bool validDiagnostics() const
        {
            for (const auto& diagnostic : diagnostics)
                if (!sources.contains(diagnostic.range)) return false;
            return true;
        }
    };

    const aevix::Node* find(const aevix::Node& root, std::string_view kind)
    {
        if (root.kind == kind) return &root;
        for (const auto& child : root.children)
            if (const auto* found = find(*child, kind)) return found;
        return nullptr;
    }

    bool equivalent(const aevix::Node& left, const aevix::Node& right)
    {
        if (left.kind != right.kind || left.value != right.value
            || left.children.size() != right.children.size()) return false;
        for (std::size_t i = 0; i < left.children.size(); ++i)
            if (!equivalent(*left.children[i], *right.children[i])) return false;
        return true;
    }

    void shorthandExpansion()
    {
        Fixture shorthand;
        Fixture explicitFields;
        auto& abbreviated = shorthand.parse(R"(
            struct Point { public i32 x; public i32 y; };
            enum Event { Move { public i32 x; public i32 y; }; Quit; };
            i32 main() {
                i32 x = 1;
                i32 y = 2;
                Point point = Point { x, y = y + 1, };
                Event event = Event.Move { y, x, };
                return point.x;
            }
        )");
        auto& expanded = explicitFields.parse(R"(
            struct Point { public i32 x; public i32 y; };
            enum Event { Move { public i32 x; public i32 y; }; Quit; };
            i32 main() {
                i32 x = 1;
                i32 y = 2;
                Point point = Point { x = x, y = y + 1, };
                Event event = Event.Move { y = y, x = x, };
                return point.x;
            }
        )");
        expect(shorthand.diagnostics.empty() && explicitFields.diagnostics.empty(),
            "mixed struct and enum shorthand accepts trailing commas");
        expect(equivalent(abbreviated, expanded),
            "shorthand produces the same ordinary AST and field order as explicit initialization");
        expect(shorthand.resolve() && explicitFields.resolve(),
            "expanded struct and enum shorthand resolves existing lexical values");
        const auto* field = find(abbreviated, "FieldInitializer");
        expect(field && field->children.size() == 1
            && field->children[0]->kind == "Name" && field->children[0]->value == "x"
            && field->children[0]->range.begin.source == field->range.begin.source
            && field->children[0]->range.begin.offset == field->range.begin.offset
            && field->children[0]->range.end.source == field->range.end.source
            && field->children[0]->range.end.offset == field->range.end.offset,
            "shorthand records one name expression at the original field token");

        Fixture nested;
        nested.parse(R"(
            struct Point { public i32 x; };
            struct Shape { public Point point; };
            i32 main() {
                i32 x = 4;
                Shape shape = Shape { point = Point { x } };
                return shape.point.x;
            }
        )");
        expect(nested.resolve(), "nested explicit initializers permit inner shorthand");
    }

    void preservedNameChecks()
    {
        for (const auto& [source, diagnostic] : std::vector<std::pair<std::string, std::string>>{
            {"struct Point { public i32 x; }; i32 main() { Point p = Point { x }; return 0; }", "Unknown name 'x'"},
            {"struct Point { public i32 x; }; i32 main() { i32 x = 1; Point p = Point { x, x = 2 }; return 0; }", "Repeated field initializer"},
            {"struct Point { public i32 x; }; i32 main() { i32 y = 1; Point p = Point { y }; return 0; }", "Unknown initializer field"},
            {"struct Point { private i32 x; }; i32 main() { i32 x = 1; Point p = Point { x }; return 0; }", "private field"},
            {"struct Point { public i32 x; public Point make(&Point self) { return Point { x }; } };", "Unknown name 'x'"},
            {"enum Event { Move { public i32 x; }; }; i32 main() { Event e = Event.Move { x }; return 0; }", "Unknown name 'x'"}})
        {
            Fixture fixture;
            fixture.parse(source);
            expect(fixture.diagnostics.empty(), "invalid shorthand values still parse as ordinary names");
            expect(!fixture.resolve() && fixture.has(diagnostic) && fixture.validDiagnostics(),
                "ordinary name resolution rejects invalid shorthand with a valid source range");
        }
    }

    void patternAndMalformedSyntax()
    {
        Fixture pattern;
        auto& ast = pattern.parse(R"(
            enum Event { Move { public i32 x; }; Quit; };
            i32 read(&Event event) {
                switch (event) {
                    case Event.Move { i32 dx = x }: return dx; break;
                    case Event.Quit: return 0; break;
                }
                return 0;
            }
        )");
        expect(pattern.diagnostics.empty() && find(ast, "PatternBinding") != nullptr,
            "explicit case pattern bindings retain their existing grammar");
        for (const auto* source : {
            "i32 main() { Point p = Point { x + 1 }; return 0; }",
            "i32 main() { Point p = Point { x.member }; return 0; }",
            "i32 main() { Point p = Point { x y }; return 0; }",
            "i32 main() { Point p = Point { x = }; return 0; }",
            "i32 main() { Point p = Point { , }; return 0; }",
            "i32 main() { switch (event) { case Event.Move { x }: break; } return 0; }"})
        {
            Fixture fixture;
            fixture.parse(source);
            expect(!fixture.diagnostics.empty() && fixture.validDiagnostics(),
                "malformed shorthand and case shorthand are rejected without invalid ranges");
        }
    }

    void overrideContexts()
    {
        Fixture valid;
        valid.parse(R"(
            interface Scored { public i32 score(&Self self) { return 1; } };
            struct Player implements Scored {
                public override i32 score(&Player self) { return 2; }
            };
            enum Event implements Scored {
                Quit;
                public override i32 score(&Event self) { return 3; }
            };
        )");
        expect(valid.diagnostics.empty(), "concrete struct and enum override methods still parse");
        for (const auto* source : {
            "override i32 score() { return 1; }",
            "override i32 value = 1;",
            "override struct Value {};",
            "struct Value { override i32 field; };",
            "struct Value { override type Field = i32; };",
            "struct Value { override struct Nested {}; };",
            "interface Value { override i32 score(&Self self); };",
            "interface Value { override i32 score(&Self self) { return 1; } };",
            "enum Value { override Item; };",
            "enum Value { Item { override i32 field; }; };",
            "i32 main() { override i32 value = 1; return 0; }"})
        {
            Fixture fixture;
            fixture.parse(source);
            expect(!fixture.diagnostics.empty() && fixture.has("override")
                && fixture.validDiagnostics(), "stray override markers are rejected in declaration context");
        }
    }
}

int main()
{
    shorthandExpansion();
    preservedNameChecks();
    patternAndMalformedSyntax();
    overrideContexts();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
