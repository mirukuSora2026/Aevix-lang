// ========================
//
// Created By : miruku_sora2026
//
// File name : type_checker_tests.cpp
//
// Description :
//     실제 모듈을 사용해 기본 타입, 함수 호출 및 제어 흐름 검사를 검증한다.
//     Verify scalar types, function calls, and control-flow checks using real modules.
//
// ========================

#include "aevix/Frontend/Modules/ModuleLoader.hpp"
#include "aevix/Frontend/Sema/ControlFlowChecker.hpp"
#include "aevix/Frontend/Sema/NameResolver.hpp"
#include "aevix/Frontend/Sema/TypeChecker.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
    namespace fs = std::filesystem;

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

    enum class Stage
    {
        Load,
        Names,
        Types,
        Flow,
        Passed
    };

    // 앞 단계의 오류를 뒤 단계의 성공적인 거부로 오인하지 않는다.
    // Do not mistake an earlier-stage failure for a successful later-stage rejection.
    struct Fixture final
    {
        fs::path directory;
        aevix::SourceManager sources;
        aevix::Diagnostics diagnostics;
        std::optional<aevix::ModuleGraph> graph;
        std::optional<aevix::NameResolution> names;
        std::optional<aevix::TypeCheckResult> types;
        Stage stage = Stage::Load;

        Fixture()
        {
            static unsigned serial = 0;
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            directory = fs::temp_directory_path()
                / ("aevix-type-tests-" + std::to_string(stamp)
                    + "-" + std::to_string(++serial));
            fs::create_directories(directory / "src");
        }

        ~Fixture()
        {
            std::error_code error;
            fs::remove_all(directory, error);
        }

        Fixture(const Fixture&) = delete;
        Fixture& operator=(const Fixture&) = delete;

        void write(std::string_view relative, std::string_view text)
        {
            const auto path = directory / relative;
            fs::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary);
            output << text;
            if (!output) throw std::runtime_error("cannot create type-check source");
        }

        bool run()
        {
            aevix::ModuleOptions options;
            options.sourceRoot = directory / "src";
            graph = aevix::ModuleLoader(sources, diagnostics).load(
                directory / "src/main.avx", options
            );
            if (!graph || !diagnostics.empty()) return false;
            stage = Stage::Names;
            names = aevix::NameResolver(diagnostics).resolve(*graph);
            if (!names->succeeded || !diagnostics.empty()) return false;
            stage = Stage::Types;
            types = aevix::TypeChecker(diagnostics).check(*graph, *names);
            if (!types->succeeded || !diagnostics.empty()) return false;
            stage = Stage::Flow;
            if (!aevix::ControlFlowChecker(diagnostics).check(*graph, *names, *types)
                || !diagnostics.empty()) return false;
            stage = Stage::Passed;
            return true;
        }

        void validateDiagnostics()
        {
            for (const auto& diagnostic : diagnostics)
            {
                expect(diagnostic.range.begin.isValid(), "semantic diagnostic has source location");
                if (diagnostic.range.begin.isValid())
                    expect(sources.contains(diagnostic.range), "semantic diagnostic stays within source");
            }
        }

        void requireSuccess(std::string_view label)
        {
            const bool result = run();
            if (!result)
                for (const auto& diagnostic : diagnostics)
                    std::cerr << label << ": " << diagnostic.message << '\n';
            expect(result, label);
            validateDiagnostics();
        }

        void requireFailure(std::string_view label, Stage expected = Stage::Types)
        {
            const bool result = run();
            if (result || stage != expected)
            {
                std::cerr << label << ": unexpected pipeline stage "
                    << static_cast<int>(stage) << '\n';
                for (const auto& diagnostic : diagnostics)
                    std::cerr << "  " << diagnostic.message << '\n';
            }
            expect(!result && stage == expected && !diagnostics.empty(), label);
            validateDiagnostics();
        }
    };

    void valid(std::string_view label, std::string_view source)
    {
        Fixture test;
        test.write("src/main.avx", source);
        test.requireSuccess(label);
    }

    void invalid(std::string_view label, std::string_view source,
        Stage expected = Stage::Types)
    {
        Fixture test;
        test.write("src/main.avx", source);
        test.requireFailure(label, expected);
    }

    void typeContextTests()
    {
        aevix::TypeContext types;
        const auto integer = types.builtin("i32");
        expect(integer == types.builtin("int"), "int aliases canonical i32");
        expect(types.builtin("float") == types.builtin("f64"), "float aliases canonical f64");
        expect(types.builtin("unknown") == types.error(), "unknown builtin yields error type");
        expect(types.get(integer).kind == aevix::TypeKind::Integer
            && types.get(integer).bits == 32 && types.get(integer).isSigned,
            "i32 records its logical width and signedness");
        expect(types.get(types.builtin("u8")).bits == 8
            && !types.get(types.builtin("u8")).isSigned,
            "u8 records its unsigned logical width");
        const auto shared = types.reference(integer, false);
        expect(shared == types.reference(integer, false), "shared reference is interned");
        expect(shared != types.reference(integer, true), "reference mutability is part of identity");
        const auto array = types.array(integer, 3);
        expect(array == types.array(integer, 3), "same static array type is interned");
        expect(array != types.array(integer, 4), "static array extent is part of identity");
        const auto function = types.function(integer, {integer, shared});
        expect(function == types.function(integer, {integer, shared}), "function signature is interned");
        expect(function != types.function(types.builtin("void"), {integer, shared}),
            "function return type is part of signature identity");
        expect(types.nominal(12, "Point") == types.nominal(12, "Point"),
            "nominal declaration has one canonical identity");
        expect(types.nominal(13, "Point") != types.nominal(12, "Point"),
            "same name from distinct declarations has distinct identity");
        const auto* stable = &types.get(integer);
        for (std::size_t extent = 0; extent < 256; ++extent)
            static_cast<void>(types.array(integer, extent));
        expect(stable == &types.get(integer), "adding types preserves existing references");
        expect(types.get(aevix::InvalidTypeId).kind == aevix::TypeKind::Error,
            "invalid type ID safely maps to error type");
    }

    void resultMappingTests()
    {
        Fixture test;
        test.write("src/main.avx", R"(
            i32 pick(i8 value) { return 1; }
            i32 pick(i32 value) { return 2; }
            i32 main() { rsn inferred = 3; return pick(3); }
        )");
        test.requireSuccess("type result records inferred declarations and selected overloads");
        if (!test.types || !test.names || test.stage != Stage::Passed) return;
        bool inferred = false;
        bool selected = false;
        const auto integer = test.types->types.builtin("i32");
        for (const auto& symbol : test.names->symbols)
        {
            if (symbol.name != "inferred") continue;
            const auto found = test.types->symbolTypes.find(symbol.id);
            inferred = found != test.types->symbolTypes.end() && found->second == integer;
        }
        for (const auto& [call, symbol] : test.types->calls)
        {
            const auto found = test.types->symbolTypes.find(symbol);
            if (test.names->symbols.at(symbol).name != "pick"
                || found == test.types->symbolTypes.end()) continue;
            const auto& function = test.types->types.get(found->second);
            const auto expressionType = test.types->nodeTypes.find(call);
            selected = function.kind == aevix::TypeKind::Function
                && function.parameters.size() == 1 && function.parameters.front() == integer
                && expressionType != test.types->nodeTypes.end() && expressionType->second == integer;
        }
        expect(inferred, "inference result stores the local symbol's concrete type");
        expect(selected, "calls map identifies the preferred i32 overload and expression type");
    }

    void scalarTests()
    {
        valid("scalar initialization, aliases, and inferred declarations", R"(
            i32 main() {
                int count = 4;
                i32 same = count;
                float ratio = 2.5;
                f64 precise = ratio;
                f32 smaller = 1.25;
                bool ready = true;
                char letter = 'a';
                string_view text = "hello";
                rsn inferred = same;
                rsn inferred_float = 3.5;
                return ready ? inferred : 0;
            }
        )");
        valid("integer literal boundaries include signed minima", R"(
            i32 main() {
                i8 small = -128;
                u8 octet = 255;
                i16 medium = -32768;
                i32 word = -2147483648;
                i64 large = -9223372036854775808;
                u64 largest = 18446744073709551615;
                return 0;
            }
        )");
        valid("parenthesized signed minimum is checked as one literal",
            "i32 main() { i8 value = -(128); return 0; }");
        invalid("nested unary signs cannot bypass a signed bound",
            "i32 main() { i8 value = -(-128); return 0; }");
        invalid("integer upper bound is checked", "i32 main() { i8 value = 128; return 0; }");
        invalid("integer lower bound is checked", "i32 main() { i8 value = -129; return 0; }");
        invalid("unsigned values reject negative literals", "i32 main() { u8 value = -1; return 0; }");
        invalid("u64 literals reject overflow", "i32 main() { u64 value = 18446744073709551616; return 0; }");
        invalid("integer initializer rejects string views", "i32 main() { i32 value = \"hello\"; return 0; }");
        invalid("intrinsic namespace cannot silently produce an error type",
            "i32 main() { return Integer; }");
        invalid("unimplemented intrinsic member reference fails explicitly",
            "i32 main() { rsn value = Integer.unknown_member; return 0; }");
        invalid("integer initializer rejects bool", "i32 main() { i32 value = true; return 0; }");
        invalid("owning string is not allocated implicitly", "i32 main() { string value = \"hello\"; return 0; }");
        invalid("different concrete integer types require an explicit conversion",
            "i32 main() { i8 small = 1; i32 value = small; return 0; }");
        invalid("inference requires an initializer", "i32 main() { rsn value; return 0; }", Stage::Load);
        invalid("fixed array elements cannot use void storage", "void items[2];");
        valid("compound constants are evaluated",
            "const i32 value = 1 + 2; i32 main() { return value; }");
        invalid("void is not a storage type", "i32 main() { void value; return 0; }");
        invalid("void call cannot initialize a value", "void run() { return; } i32 main() { rsn value = run(); return 0; }");
    }

    void importedExtentTests()
    {
        Fixture test;
        test.write("src/config.avx", "public const usize count = 2;");
        test.write("src/main.avx", R"(
            import config { count };
            i32 main() {
                Array<i32, config.count> values = [1, 2];
                return values[0];
            }
        )");
        test.requireSuccess("qualified imported constant determines an array extent");
    }

    void operatorTests()
    {
        valid("mutable operators and scalar conditions", R"(
            i32 main() {
                mut i32 count = 1;
                count += 2;
                ++count;
                count--;
                i32 mask = (count << 1) | 1;
                bool ready = count > 0 && mask != 0;
                return ready ? count : mask;
            }
        )");
        invalid("initialized immutable variable cannot be assigned", "i32 main() { i32 x = 1; x = 2; return x; }");
        invalid("immutable variable cannot be incremented", "i32 main() { i32 x = 1; x++; return x; }");
        invalid("assignment checks its right operand", "i32 main() { mut i32 x = 1; x = false; return x; }");
        invalid("arithmetic operands must have the same type", "i32 main() { i32 x = 1; f64 y = 2.0; rsn z = x + y; return x; }");
        invalid("logical operators reject integers", "i32 main() { bool ready = 1 && 2; return 0; }");
        invalid("bitwise operators reject floating point", "i32 main() { f64 value = 1.0 & 2.0; return 0; }");
        invalid("ternary condition is bool", "i32 main() { return 1 ? 2 : 3; }");
        invalid("ternary branches must agree", "i32 main() { rsn value = true ? 1 : false; return 0; }");
        invalid("if condition is bool", "i32 main() { if (1) return 1; return 0; }");
        invalid("while condition is bool", "i32 main() { while (1) {} return 0; }");
        valid("shared reference and dereference", "i32 read(&i32 value) { return *value; } i32 main() { i32 x = 7; return read(&x); }");
        valid("mutable reference permits a write", "void set(&mut i32 value) { *value = 3; return; } i32 main() { mut i32 x = 1; set(&mut x); return x; }");
        invalid("shared reference cannot write", "void set(&i32 value) { *value = 3; return; } i32 main() { return 0; }");
        invalid("mutable borrow needs a mutable value", "i32 main() { i32 x = 1; &mut i32 ref = &mut x; return x; }");
        invalid("dereference requires a reference", "i32 main() { i32 x = 1; return *x; }");
    }

    void callTests()
    {
        valid("named arguments and Copy constant defaults", R"(
            i32 scale(i32 value, i32 factor = 2) { return value * factor; }
            void accept(i32 value) { return; }
            i32 main() {
                i32 first = scale(4);
                i32 second = scale(factor = 3, value = first);
                accept(second);
                return scale(second, factor = 1);
            }
        )");
        valid("overload follows typed argument and integer literal default", R"(
            i32 pick(i8 value) { return 1; }
            i32 pick(i32 value) { return 2; }
            i32 main() { i8 small = 3; return pick(small) + pick(3); }
        )");
        valid("float literal chooses f64 overload", R"(
            i32 pick(f32 value) { return 1; }
            i32 pick(f64 value) { return 2; }
            i32 main() { return pick(1.5); }
        )");
        invalid("wrong return type is rejected", "i32 main() { return true; }");
        invalid("nonvoid return requires a value", "i32 main() { return; }");
        invalid("void return rejects a value", "void run() { return 1; }");
        invalid("missing required argument", "i32 f(i32 value) { return value; } i32 main() { return f(); }");
        invalid("extra argument", "i32 f(i32 value) { return value; } i32 main() { return f(1, 2); }");
        invalid("argument type mismatch", "i32 f(i32 value) { return value; } i32 main() { return f(true); }");
        invalid("unknown named parameter", "i32 f(i32 value) { return value; } i32 main() { return f(other = 1); }");
        invalid("same parameter cannot be filled twice", "i32 f(i32 value) { return value; } i32 main() { return f(1, value = 2); }");
        invalid("nondefault numeric overload remains ambiguous", "i32 f(i8 value) { return 1; } i32 f(i64 value) { return 2; } i32 main() { return f(1); }");
        invalid("fewer default arguments do not break a tie", "i32 f(i32 value) { return 1; } i32 f(i32 value, i32 extra = 2) { return 2; } i32 main() { return f(1); }");
        valid("compound constant defaults are evaluated",
            "i32 f(i32 value = 1 + 2) { return value; }");
        invalid("nonconstant parameter default is rejected", "i32 value() { return 2; } i32 f(i32 input = value()) { return input; } i32 main() { return f(); }");
        invalid("a default parameter cannot precede a required parameter", "i32 f(i32 first = 1, i32 second) { return second; } i32 main() { return 0; }", Stage::Load);
        invalid("generic support fails explicitly at the current milestone", "T identity<T>(T value) { return value; } i32 main() { return identity<i32>(1); }");
        invalid("dynamic interfaces fail explicitly at the current milestone", "interface Readable {}; i32 main() { &dyn Readable value; return 0; }");
    }

    void aggregateTests()
    {
        valid("fixed arrays use expected element types", "i32 main() { i8 values[3] = [1, 2, 3]; return 0; }");
        invalid("grouped negative array extent is rejected",
            "i32 main() { i32 data[(-1)] = [7]; return 0; }");
        valid("opposite unary signs in a positive array extent are preserved",
            "i32 main() { i32 data[-(-1)] = [7]; return 0; }");
        valid("fixed array indexing yields an element", "i32 main() { i32 values[3] = [1, 2, 3]; return values[1]; }");
        invalid("fixed array rejects wrong element type", "i32 main() { i32 values[2] = [1, false]; return 0; }");
        invalid("fixed array length must match", "i32 main() { i32 values[2] = [1, 2, 3]; return 0; }");
        invalid("array index must be integer", "i32 main() { i32 values[2] = [1, 2]; return values[true]; }");
        valid("struct factory and auto-borrowed method", R"(
            struct Counter {
                i32 value;
                public Counter new(i32 value) { return Counter { value = value }; }
                public i32 read(&Counter self) { return (*self).value; }
            };
            i32 main() { Counter counter = Counter.new(4); return counter.read(); }
        )");
        invalid("instance fields cannot be read through a type name",
            "struct P { public i32 x; }; i32 main() { return P.x; }");
        invalid("struct initializer checks field type", "struct Item { public i32 value; }; i32 main() { Item item = Item { value = false }; return 0; }");
        invalid("struct initializer requires every field", "struct Item { public i32 first; public i32 second; }; i32 main() { Item item = Item { first = 1 }; return 0; }");
        invalid("field read has its declared type", "struct Item { public bool value; }; i32 main() { Item item = Item { value = true }; return item.value; }");
        {
            Fixture test;
            test.write("src/math.avx", "public i32 add(i32 left, i32 right) { return left + right; }");
            test.write("src/main.avx", "import math { add }; i32 main() { return math.add(1, 2); }");
            test.requireSuccess("imported function participates in type checking");
        }
        {
            Fixture test;
            test.write("src/math.avx", "public i32 bad() { return false; }");
            test.write("src/main.avx", "import math { bad }; i32 main() { return bad(); }");
            test.requireFailure("reachable imported module is type checked");
            bool importedLocation = false;
            for (const auto& diagnostic : test.diagnostics)
                if (diagnostic.range.begin.isValid()
                    && fs::path(test.sources.get(diagnostic.range.begin.source).path).filename() == "math.avx")
                    importedLocation = true;
            expect(importedLocation, "imported type error retains original source file");
        }
    }

    void controlFlowTests()
    {
        valid("if else returns cover all paths", "i32 f(bool ready) { if (ready) return 1; else return 2; }");
        valid("mutable deferred initialization on both branches", "i32 f(bool ready) { mut i32 x; if (ready) { x = 1; } else { x = 2; } return x; }");
        valid("parenthesized immutable first write initializes its symbol",
            "i32 main() { i32 x; (x) = 1; return x; }");
        valid("immutable deferred initialization", "i32 main() { i32 x; x = 1; return x; }");
        valid("immutable deferred initialization on disjoint branches", "i32 f(bool ready) { i32 x; if (ready) { x = 1; } else { x = 2; } return x; }");
        valid("returning branch does not weaken initialization on live branch", "i32 f(bool ready) { i32 x; if (ready) return 1; else { x = 2; } return x; }");
        invalid("missing nonvoid return", "i32 main() {}", Stage::Flow);
        invalid("only one returning branch is insufficient", "i32 f(bool ready) { if (ready) return 1; }", Stage::Flow);
        invalid("uninitialized local read", "i32 main() { i32 x; return x; }", Stage::Flow);
        invalid("single-branch initialization is insufficient", "i32 f(bool ready) { mut i32 x; if (ready) { x = 1; } return x; }", Stage::Flow);
        invalid("while body may not execute", "i32 f(bool ready) { mut i32 x; while (ready) { x = 1; break; } return x; }", Stage::Flow);
        invalid("immutable deferred value is assigned once", "i32 main() { i32 x; x = 1; x = 2; return x; }", Stage::Flow);
        invalid("possibly assigned immutable value cannot be assigned again", "i32 f(bool ready) { i32 x; if (ready) { x = 1; } x = 2; return x; }", Stage::Flow);
        invalid("break requires loop or switch", "i32 main() { break; return 0; }", Stage::Flow);
        invalid("continue requires loop", "i32 main() { continue; return 0; }", Stage::Flow);
        invalid("immutable assignment cannot repeat on loop back edge",
            "i32 main() { i32 value; mut i32 index = 0; while (index < 2) { value = index; ++index; } return 0; }", Stage::Flow);
        invalid("for continue still evaluates the update expression",
            "i32 main() { mut i32 value; for (mut i32 index = 0; index < 2; ++value) { continue; } return 0; }", Stage::Flow);
        valid("switch and default returning paths cover the function", R"(
            i32 choose(i32 value) {
                switch (value) {
                    case 1: return 2; break;
                    default: return 3; break;
                }
            }
        )");
        invalid("switch without default retains an unmatched path", R"(
            i32 choose(i32 value) {
                switch (value) { case 1: return 2; break; }
            }
        )", Stage::Flow);
        invalid("function reads of uninitialized global are rejected",
            "i32 value; i32 main() { return value; }", Stage::Flow);
        valid("enhanced array loop initializes its binding", R"(
            i32 main() {
                i32 values[2] = [1, 2];
                mut i32 total = 0;
                for (i32 value : values) { total += value; }
                return total;
            }
        )");
        invalid("enhanced loop body checks uninitialized reads", R"(
            i32 main() {
                i32 values[2] = [1, 2];
                i32 uninitialized;
                for (i32 value : values) { i32 copy = uninitialized; }
                return 0;
            }
        )", Stage::Flow);
        valid("qualified global assignment initializes the original symbol",
            "mut i32 value; i32 main() { main.value = 1; return main.value; }");
        valid("qualified immutable global may receive its first write",
            "i32 value; i32 main() { main.value = 1; return main.value; }");
        valid("never argument prevents reading a later argument", R"(
            never halt() { while (true) {} }
            i32 consume(i32 first, i32 second) { return first + second; }
            i32 main() { i32 value; return consume(halt(), value); }
        )");
        invalid("short circuit prevents a skipped never call from terminating", R"(
            never halt() { while (true) {} }
            i32 main() { false && halt(); }
        )", Stage::Flow);
        valid("evaluated never call terminates a logical expression", R"(
            never halt() { while (true) {} }
            i32 main() { true && halt(); }
        )");
        invalid("switch labels cannot be runtime variables", R"(
            i32 main() {
                i32 key = 1;
                switch (key) { case key: break; default: break; }
                return 0;
            }
        )");
        invalid("duplicate scalar switch labels are rejected", R"(
            i32 main() {
                i32 key = 1;
                switch (key) { case 1: break; case 1: break; default: break; }
                return 0;
            }
        )");
        valid("literal const alias is a valid switch label", R"(
            i32 main() {
                const i32 tag = 1;
                i32 key = 1;
                switch (key) { case tag: break; default: break; }
                return 0;
            }
        )");
        valid("void function may fall through", "void run() {} i32 main() { run(); return 0; }");
    }
}

int main()
{
    typeContextTests();
    resultMappingTests();
    importedExtentTests();
    scalarTests();
    operatorTests();
    callTests();
    aggregateTests();
    controlFlowTests();
    std::cout << "Type checks=" << checks << ", failures=" << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
