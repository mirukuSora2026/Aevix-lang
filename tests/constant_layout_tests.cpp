// ========================
//
// Created By : miruku_sora2026
//
// File name : constant_layout_tests.cpp
//
// Description :
//     상수 연산 경계, 배열 범위와 대상 저장 배치의 회귀를 검사한다.
//     Check regressions in constant arithmetic boundaries, array bounds, and target storage layout.
//
// ========================

#include "aevix/Frontend/Modules/ModuleLoader.hpp"
#include "aevix/Frontend/Sema/NameResolver.hpp"
#include "aevix/Frontend/Sema/TypeChecker.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
    unsigned checks = 0, failures = 0;
    void expect(bool condition, const char* message)
    {
        ++checks;
        if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    }
    struct Fixture
    {
        std::filesystem::path path;
        aevix::SourceManager sources;
        aevix::Diagnostics diagnostics;
        std::optional<aevix::ModuleGraph> graph;
        aevix::NameResolution names;
        aevix::TypeCheckResult types;
        Fixture()
        {
            static unsigned serial = 0;
            path = std::filesystem::temp_directory_path() / ("aevix-constant-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
            std::filesystem::create_directories(path);
        }
        ~Fixture() { std::error_code error; std::filesystem::remove_all(path, error); }
        void write(const char* file, std::string_view text)
        {
            std::ofstream output(path / file); output << text;
            if (!output) throw std::runtime_error("cannot write constant test source");
        }
        bool run(std::string_view text)
        {
            write("main.avx", text);
            aevix::ModuleOptions options; options.sourceRoot = path;
            graph = aevix::ModuleLoader(sources, diagnostics).load(path / "main.avx", options);
            if (!graph || !diagnostics.empty()) return false;
            names = aevix::NameResolver(diagnostics).resolve(*graph);
            if (!names.succeeded || !diagnostics.empty()) return false;
            types = aevix::TypeChecker(diagnostics).check(*graph, names);
            return types.succeeded && diagnostics.empty();
        }
        bool contains(std::string_view text)
        {
            for (const auto& diagnostic : diagnostics) if (diagnostic.message.find(text) != std::string::npos) return true;
            return false;
        }
        const aevix::ConstantValue* constant(std::string_view name)
        {
            for (const auto& symbol : names.symbols)
                if (symbol.name == name && symbol.declaration)
                    for (const auto& item : symbol.declaration->children)
                    {
                        const auto found = types.constants.find(item.get());
                        if (found != types.constants.end()) return &found->second;
                    }
            return nullptr;
        }
        aevix::TypeId type(std::string_view name)
        {
            for (const auto& symbol : names.symbols)
                if (symbol.name == name && types.symbolTypes.contains(symbol.id)) return types.symbolTypes.at(symbol.id);
            return aevix::InvalidTypeId;
        }
    };
    void accepts(std::string_view text, const char* message)
    {
        Fixture fixture;
        const bool passed = fixture.run(text);
        if (!passed) for (const auto& error : fixture.diagnostics) std::cerr << error.message << '\n';
        expect(passed, message);
    }
    void rejects(std::string_view text, std::string_view diagnostic, const char* message)
    {
        Fixture fixture;
        expect(!fixture.run(text) && fixture.contains(diagnostic), message);
        for (const auto& error : fixture.diagnostics) expect(fixture.sources.contains(error.range), "constant failure has positioned diagnostic");
    }
}

int main()
{
    accepts("const i32 count = (2 + 3) * 4; i32 values[count] = [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20];", "computed fixed array extent");
    accepts("const bool enabled = 3 < 4 && !(2 == 3); const i32 selected = enabled ? 7 : 1 / 0;", "boolean and lazy ternary constant");
    accepts("const bool enabled = false && (1 / 0 == 0);", "logical short circuit");
    accepts("const i8 low = -128; const i64 low64 = -9223372036854775808; const u64 high = 18446744073709551615;", "signed minima and unsigned maximum");
    accepts("const i32 shifted = -3 >> 1; const i32 bits = (~0 & 7) ^ 2;", "signed arithmetic shift and bitwise operations");
    accepts("const f64 number = (1.5 + 2.5) / 2.0; const f32 narrow = 3.0 * 2.0; const f16 half = 1.0 / 2.0; const bf16 brain = 1.0 + 1.0;", "floating arithmetic");
    accepts("i32 f(i32 value = (2 + 3) * 4) { return value; }", "computed default argument");
    {
        Fixture fixture;
        fixture.write("sizes.avx", "public const i32 WIDTH = 2 + 1;");
        expect(fixture.run("import sizes { WIDTH }; const i32 total = sizes.WIDTH * 2; i32 values[total];"), "qualified imported constant alias");
        const auto* total = fixture.constant("total");
        expect(total && total->magnitude == 6 && !total->negative, "constant values exported for lowering");
    }
    accepts("const f32 value = 3.0e38 * 3.0e38; const f64 infinity = 1.0 / 0.0; const f64 nan = 0.0 / 0.0;", "computed floating infinity and NaN follow IEEE");
    rejects("const i8 bad = 127 + 1;", "overflow", "narrow addition overflow");
    rejects("const u64 bad = 18446744073709551615 + 1;", "overflow", "unsigned addition overflow");
    rejects("const i64 bad = -9223372036854775808 / -1;", "overflow", "signed division overflow");
    rejects("const i32 bad = 9 / 0;", "zero", "constant division zero");
    rejects("const i32 bad = 9 % 0;", "zero", "constant remainder zero");
    rejects("const i32 bad = 1 << 32;", "shift", "shift equal to width");
    rejects("const i32 bad = 1 << -1;", "shift", "negative shift");
    accepts("const i32 shifted = 1073741824 << 1; const u8 narrowed = 255 << 1;", "count-checked left shifts discard high bits");
    rejects("const i32 a = b; const i32 b = a;", "Cyclic", "constant dependency cycle");
    rejects("i32 source() { return 2; } const i32 bad = source();", "compile-time", "ordinary function is not const evaluator");
    rejects("i32 count = 3; i32 values[count];", "compile-time", "nonconstant fixed extent");
    rejects("const i32 count = -1; i32 values[count];", "nonnegative", "negative array extent");
    rejects("i32 values[2] = [1,2]; i32 f() { return values[1 + 1]; }", "bounds", "computed array index bounds");
    rejects("i32 f(i32 value) { switch (value) { case 1 + 2: break; case 3: break; default: break; } return 0; }", "Duplicate", "equivalent computed switch cases");
    rejects("i32 f(char value) { switch (value) { case 'a': break; case '\\u{61}': break; default: break; } return 0; }", "Duplicate", "equivalent escaped character switch cases");
    rejects("struct Loop { Loop next; };", "Recursive", "direct recursive value layout");
    rejects("struct Left { Right right; }; struct Right { Left left; };", "Recursive", "mutual recursive value layout");
    accepts("struct Node { &Node next; };", "recursive reference layout terminates");
    {
        Fixture fixture;
        expect(fixture.run("struct Mixed { u8 first; u64 second; u16 third; }; Mixed value; i32 array[2][3];"), "mixed struct and multidimensional layout");
        for (const auto target : {aevix::LayoutTarget::AArch64, aevix::LayoutTarget::X86_64})
        {
            const auto layout = aevix::TypeLayout(fixture.types.types, fixture.names, fixture.types.symbolTypes, target).layout(fixture.type("Mixed"));
            expect(layout.succeeded && layout.layout.size == 24 && layout.layout.alignment == 8, "64-bit target struct padding");
            expect(layout.layout.fields.size() == 3 && layout.layout.fields[0].offset == 0 && layout.layout.fields[1].offset == 8 && layout.layout.fields[2].offset == 16, "field offsets preserve declaration order");
        }
        const auto layout = aevix::TypeLayout(fixture.types.types, fixture.names, fixture.types.symbolTypes).layout(fixture.type("array"));
        expect(layout.succeeded && layout.layout.size == 24 && layout.layout.alignment == 4, "multidimensional fixed storage");
    }
    {
        Fixture fixture;
        fixture.write("main.avx", "enum Event { None; Count { public u8 small; public u64 large; }; }; ");
        aevix::ModuleOptions options; options.sourceRoot = fixture.path;
        fixture.graph = aevix::ModuleLoader(fixture.sources, fixture.diagnostics).load(fixture.path / "main.avx", options);
        expect(fixture.graph.has_value() && fixture.diagnostics.empty(), "enum layout fixture parses");
        if (fixture.graph)
        {
            fixture.names = aevix::NameResolver(fixture.diagnostics).resolve(*fixture.graph);
            aevix::TypeContext context;
            std::unordered_map<aevix::SymbolId, aevix::TypeId> symbols;
            aevix::TypeId enumType = aevix::InvalidTypeId;
            for (const auto& symbol : fixture.names.symbols)
            {
                if (symbol.name == "Event") enumType = symbols[symbol.id] = context.nominal(symbol.id, "Event");
                else if (symbol.declaration && symbol.declaration->kind == "Variable")
                    symbols[symbol.id] = context.builtin(symbol.declaration->children[0]->value);
            }
            const auto layout = aevix::TypeLayout(context, fixture.names, symbols).layout(enumType);
            expect(layout.succeeded && layout.layout.size == 24 && layout.layout.alignment == 8, "enum tag and maximum variant payload layout");
            expect(layout.layout.fields.size() == 2 && layout.layout.fields[0].offset == 8 && layout.layout.fields[1].offset == 16, "enum payload field offsets");
        }
    }
    {
        Fixture fixture;
        expect(fixture.run("enum Empty { None; }; enum Zero { Value { unit item; }; }; enum Aligned { Value { u64 words[0]; }; }; enum Nested { Value { i32 words[2][3]; }; };"), "enum layout corner cases type check");
        const auto empty = aevix::TypeLayout(fixture.types.types, fixture.names, fixture.types.symbolTypes).layout(fixture.type("Empty"));
        const auto zero = aevix::TypeLayout(fixture.types.types, fixture.names, fixture.types.symbolTypes).layout(fixture.type("Zero"));
        const auto aligned = aevix::TypeLayout(fixture.types.types, fixture.names, fixture.types.symbolTypes).layout(fixture.type("Aligned"));
        const auto nested = aevix::TypeLayout(fixture.types.types, fixture.names, fixture.types.symbolTypes).layout(fixture.type("Nested"));
        expect(empty.succeeded && empty.layout.size == 4 && empty.layout.alignment == 4, "tag-only enum layout");
        expect(zero.succeeded && zero.layout.size == 4 && zero.layout.alignment == 4, "zero-size unit payload layout");
        expect(aligned.succeeded && aligned.layout.size == 8 && aligned.layout.alignment == 8, "zero-length aligned payload preserves alignment");
        expect(nested.succeeded && nested.layout.size == 28 && nested.layout.alignment == 4, "nested fixed array enum payload layout");
    }
    rejects("enum Loop { Next { Loop next; }; };", "Recursive", "recursive enum value layout rejected");
    {
        Fixture fixture;
        expect(fixture.run("const i32 shifted = -3 >> 1; const i32 bits = (~0 & 7) ^ 2; const f16 half = 1.0 / 2.0;"), "constant results fixture checks");
        const auto* shifted = fixture.constant("shifted");
        const auto* bits = fixture.constant("bits");
        const auto* half = fixture.constant("half");
        expect(shifted && shifted->negative && shifted->magnitude == 2, "negative right shift rounds toward negative infinity");
        expect(bits && !bits->negative && bits->magnitude == 5, "bitwise constant result");
        expect(half && half->floating == 0.5L, "half precision constant result");
    }
    {
        Fixture fixture;
        expect(fixture.run("const f32 infinity = 3.0e38 * 3.0e38; const f64 negative = -1.0 / 0.0; const f64 nan = 0.0 / 0.0; const bool equal = nan == nan; const bool unequal = nan != nan; const bool less = nan < nan; const i32 shifted = 1073741824 << 1; const u8 narrowed = 255 << 1;"), "IEEE values and shift discard results fixture");
        const auto* infinity = fixture.constant("infinity"); const auto* negative = fixture.constant("negative"); const auto* nan = fixture.constant("nan");
        const auto* equal = fixture.constant("equal"); const auto* unequal = fixture.constant("unequal"); const auto* less = fixture.constant("less");
        const auto* shifted = fixture.constant("shifted"); const auto* narrowed = fixture.constant("narrowed");
        expect(infinity && std::isinf(infinity->floating) && !std::signbit(infinity->floating), "finite f32 overflow produces positive infinity");
        expect(negative && std::isinf(negative->floating) && std::signbit(negative->floating), "negative floating infinity");
        expect(nan && std::isnan(nan->floating), "floating zero divided by zero produces NaN");
        expect(equal && !equal->boolean && unequal && unequal->boolean && less && !less->boolean, "NaN comparisons follow IEEE");
        expect(shifted && shifted->negative && shifted->magnitude == 2147483648ULL, "signed left shift reinterprets remaining bits");
        expect(narrowed && !narrowed->negative && narrowed->magnitude == 254, "unsigned left shift discards high bits");
    }
    rejects("u64 enormous[18446744073709551615];", "overflows", "array storage size overflow");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
