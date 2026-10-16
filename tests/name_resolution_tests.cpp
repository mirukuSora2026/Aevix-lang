// ========================
//
// Created By : miruku_sora2026
//
// File name : name_resolution_tests.cpp
//
// Description :
//     격리된 소스 파일로 모듈 경로, 가져오기, 재공개, 접근 권한 및 지역 이름 해석을 검사한다.
//     Use isolated source files to test module paths, imports, re-exports, visibility, and lexical name resolution.
//
// ========================

#include "aevix/Frontend/Modules/ModuleLoader.hpp"
#include "aevix/Frontend/Sema/NameResolver.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;

unsigned checks = 0;
unsigned failures = 0;

void expect(bool condition, std::string_view name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}

// 각 시험은 실제 파일을 격리된 임시 디렉터리에 작성하고 자동 정리한다.
// Each test writes real files into an isolated temporary directory and cleans them up.
struct Fixture final {
    fs::path directory;
    fs::path sourceRoot;
    aevix::SourceManager sources;
    aevix::Diagnostics diagnostics;
    aevix::ModuleOptions options;
    std::optional<aevix::ModuleGraph> graph;
    std::optional<aevix::NameResolution> resolution;

    Fixture() {
        static unsigned serial = 0;
        const auto clock = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = fs::temp_directory_path()
            / ("aevix-name-tests-" + std::to_string(clock)
               + "-" + std::to_string(++serial));
        sourceRoot = directory / "src";
        fs::create_directories(sourceRoot);
        options.sourceRoot = sourceRoot;
    }

    ~Fixture() {
        std::error_code error;
        fs::remove_all(directory, error);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void write(std::string_view relative, std::string_view text) {
        const auto path = directory / relative;
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << text;
        if (!output) throw std::runtime_error("cannot create test source");
    }

    bool load(std::string_view entry = "src/main.avx") {
        graph = aevix::ModuleLoader(sources, diagnostics).load(
            directory / entry, options
        );
        return graph.has_value() && diagnostics.empty();
    }

    bool resolve() {
        if (!load()) return false;
        resolution = aevix::NameResolver(diagnostics).resolve(*graph);
        return resolution->succeeded && diagnostics.empty();
    }

    void requireSuccess(std::string_view name) {
        const bool succeeded = resolve();
        if (!succeeded) {
            for (const auto& diagnostic : diagnostics)
                std::cerr << name << ": " << diagnostic.message << '\n';
        }
        expect(succeeded, name);
        validateDiagnostics();
    }

    void requireFailure(std::string_view name) {
        expect(!resolve() && !diagnostics.empty(), name);
        validateDiagnostics();
    }

    void validateDiagnostics() {
        for (const auto& diagnostic : diagnostics) {
            // 파일을 열기 전의 경로 오류는 유효한 소스 위치가 없을 수 있다.
            // Path errors before a file is opened may not have a valid source location.
            if (diagnostic.range.begin.isValid())
                expect(sources.contains(diagnostic.range), "diagnostic source bounds");
        }
    }
};

void preludeTests() {
    {
        Fixture test;
        test.write("src/main.avx", R"(
            i32 identity(i32 input) { return input; }
            i32 main() {
                i32 count = 1;
                bool ready = true;
                char letter = 'a';
                byte data;
                string text;
                string_view view;
                Array<i32, 2> fixed;
                Vector<i32> values;
                Slice<i32> slice;
                MutSlice<i32> writable;
                Tuple<i32, bool> pair;
                Range<i32> range;
                Optional<i32> optional;
                Result<i32, i32> result;
                return identity(count);
            }
        )");
        test.requireSuccess("primitive and common types are available without imports");
    }
    {
        Fixture test;
        test.write("src/main.avx", "i32 main() { Tensor tensor; return 0; }");
        test.requireFailure("specialized Tensor requires an import");
    }
    {
        Fixture test;
        test.write("src/main.avx", "i32 main() { Box<i32> owner; return 0; }");
        test.requireFailure("Box is not part of the automatic prelude");
    }
    {
        Fixture test;
        test.write("src/main.avx", "import tensor { Tensor }; i32 main() { Tensor value; return 0; }");
        test.write("src/tensor.avx", "public struct Tensor { public i32 size; };");
        test.requireSuccess("imported specialized type becomes available");
        bool linked = false;
        if (test.resolution && test.graph) {
            const auto module = test.graph->find("tensor");
            for (const auto& symbol : test.resolution->symbols) {
                if (symbol.name != "Tensor" || symbol.module != module || !symbol.isPublic)
                    continue;
                for (const auto& [node, candidates] : test.resolution->references) {
                    if (node->kind == "Type" && node->value == "Tensor"
                        && candidates.size() == 1 && candidates.front() == symbol.id)
                        linked = true;
                }
            }
        }
        expect(linked, "type reference links to original exported Tensor symbol");
    }
}

void importTests() {
    {
        Fixture test;
        test.write("src/math/vector.avx", "public struct Point {}; public i32 dot() { return 1; }");
        test.write("src/main.avx", "import math.vector { Point }; Point p; math.vector.Point q; i32 main() { return 0; }");
        test.requireSuccess("selected import allows short and imported qualified paths");
    }
    {
        Fixture test;
        test.write("src/math/vector.avx", "public struct Point {}; public i32 dot() { return 1; }");
        test.write("src/main.avx", "import math.vector { Point }; i32 main() { return math.vector.dot(); }");
        test.requireFailure("selective import does not grant other qualified names");
    }
    {
        Fixture test;
        test.write("src/math/vector.avx", "public struct Point {};");
        test.write("src/main.avx", "import math.vector; math.vector.Point point; i32 main() { return 0; }");
        test.requireSuccess("whole-module import permits qualified public names");
    }
    {
        Fixture test;
        test.write("src/math/vector.avx", "public struct Point {};");
        test.write("src/main.avx", "import math.vector; Point point; i32 main() { return 0; }");
        test.requireFailure("whole-module import does not create short names");
    }
    {
        Fixture test;
        test.write("src/math.avx", "public i32 one() { return 1; }");
        test.write("src/math/vector.avx", "public struct Point {};");
        test.write("src/main.avx", "import math; math.vector.Point point; i32 main() { return 0; }");
        test.requireFailure("parent-module import does not expose child modules");
    }
    {
        Fixture test;
        test.write("src/geometry.avx", "public struct Point {}; private struct Hidden {};");
        test.write("src/main.avx", "import geometry { * }; Point point; i32 main() { return 0; }");
        test.requireSuccess("wildcard imports public declarations");
    }
    {
        Fixture test;
        test.write("src/geometry.avx", "private struct Hidden {};");
        test.write("src/main.avx", "import geometry { Hidden }; i32 main() { return 0; }");
        test.requireFailure("selective import cannot import private declarations");
    }
    {
        Fixture test;
        test.write("src/geometry.avx", "private struct Hidden {};");
        test.write("src/main.avx", "import geometry; geometry.Hidden hidden; i32 main() { return 0; }");
        test.requireFailure("qualified module access still obeys visibility");
    }
    for (const bool useShortName : {false, true}) {
        Fixture test;
        test.write("src/left.avx", "public struct Point {};");
        test.write("src/right.avx", "public struct Point {};");
        test.write("src/main.avx", useShortName
            ? "import left { Point }; import right { Point }; Point point; i32 main() { return 0; }"
            : "import left { Point }; import right { Point }; left.Point first; right.Point second; i32 main() { return 0; }");
        if (useShortName)
            test.requireFailure("different origins are ambiguous only when short name is used");
        else
            test.requireSuccess("unused imported name collisions permit qualified access");
    }
    {
        Fixture test;
        test.write("src/geometry.avx", "public struct Point {};");
        test.write("src/main.avx", "import geometry { Point }; import geometry { Point }; Point point; i32 main() { return 0; }");
        test.requireSuccess("repeated import reuses one module and one declaration origin");
        expect(test.graph && test.graph->modules.size() == 2, "repeated imports load exactly two modules");
    }
}

void reexportTests() {
    {
        Fixture test;
        test.write("src/origin.avx", "public struct Point {};");
        test.write("src/geometry.avx", "public import origin { Point };");
        test.write("src/main.avx", "import geometry { Point }; import origin { Point }; Point point; geometry.Point other; i32 main() { return 0; }");
        test.requireSuccess("re-exported and direct imports deduplicate by original declaration");
        unsigned origins = 0;
        if (test.resolution && test.graph) {
            const auto origin = test.graph->find("origin");
            for (const auto& symbol : test.resolution->symbols)
                if (symbol.name == "Point" && symbol.module == origin)
                    ++origins;
        }
        expect(origins == 1, "re-export paths preserve one original symbol ID");
    }
    {
        Fixture test;
        test.write("src/origin.avx", "public struct Point {};");
        test.write("src/geometry.avx", "public import origin { Point }; geometry.Point local;");
        test.write("src/main.avx", "import geometry; i32 main() { return 0; }");
        test.requireSuccess("a module can qualify its own re-exported declaration");
    }
    {
        Fixture test;
        test.write("src/origin.avx", "public struct Point {};");
        test.write("src/geometry.avx", "public import origin { Point };");
        test.write("src/main.avx", "import geometry { Point }; origin.Point point; i32 main() { return 0; }");
        test.requireFailure("re-export does not grant access to its original module path");
    }
    {
        Fixture test;
        test.write("src/origin.avx", "struct Hidden {};");
        test.write("src/geometry.avx", "public import origin { Hidden };");
        test.write("src/main.avx", "import geometry; i32 main() { return 0; }");
        test.requireFailure("private declarations cannot be re-exported");
    }
    {
        Fixture test;
        test.write("src/a.avx", "public import b { Missing };");
        test.write("src/b.avx", "public import a { Missing };");
        test.write("src/main.avx", "import a; i32 main() { return 0; }");
        test.requireFailure("re-export cycles without a defining declaration diagnose");
    }
    {
        Fixture test;
        test.write("src/a.avx", "public import b { Point };");
        test.write("src/b.avx", "import a; public struct Point {};");
        test.write("src/main.avx", "import a { Point }; Point point; i32 main() { return 0; }");
        test.requireSuccess("cyclic modules can re-export an actual declaration");
    }
}

void loaderTests() {
    {
        Fixture test;
        test.write("src/a.avx", "import b { B }; public struct A { public &B other; };");
        test.write("src/b.avx", "import a { A }; public struct B { public &A other; };");
        test.write("src/main.avx", "import a { A }; A item; i32 main() { return 0; }");
        test.requireSuccess("same-package module declaration cycles are allowed");
        expect(test.graph && test.graph->modules.size() == 3, "module cycle loads each file once");
    }
    {
        Fixture test;
        test.write("src/main.avx", "import first.api; i32 main() { return 0; }");
        test.write("first/api.avx", "import second.api; public struct A {};");
        test.write("second/api.avx", "import first.api; public struct B {};");
        test.options.searchRoots.push_back({test.directory / "first", "first"});
        test.options.searchRoots.push_back({test.directory / "second", "second"});
        test.requireFailure("package dependency cycles are forbidden");
    }
    {
        Fixture test;
        test.write("src/main.avx", "import pixel.filters { Filter }; Filter item; i32 main() { return 0; }");
        test.write("package/filters.avx", "public struct Filter {};");
        test.options.searchRoots.push_back({test.directory / "package", "pixel"});
        test.requireSuccess("external packages use their explicit prefix");
    }
    {
        Fixture test;
        test.write("src/main.avx", "import filters; i32 main() { return 0; }");
        test.write("package/filters.avx", "public struct Filter {};");
        test.options.searchRoots.push_back({test.directory / "package", "pixel"});
        test.requireFailure("package modules cannot omit the package prefix");
    }
    {
        Fixture test;
        test.write("src/main.avx", "import math; i32 main() { return 0; }");
        test.write("src/math.avx", "public i32 value = 1;");
        test.write("standard/math.avx", "public i32 value = 2;");
        test.options.searchRoots.push_back({test.directory / "standard", ""});
        test.requireFailure("same module path in different roots is ambiguous");
    }
    {
        Fixture test;
        test.write("src/main.avx", "import math.Vector; i32 main() { return 0; }");
        test.write("first/math/Vector.avx", "public struct First {};");
        test.write("second/math/vector.avx", "public struct Second {};");
        test.options.searchRoots.push_back({test.directory / "first", ""});
        test.options.searchRoots.push_back({test.directory / "second", ""});
        test.requireFailure("case-only project modules cannot coexist across separate roots");
    }
    {
        Fixture test;
        test.write("src/main.avx", "import pixel.Vector; i32 main() { return 0; }");
        test.write("first/Vector.avx", "public struct First {};");
        test.write("second/vector.avx", "public struct Second {};");
        test.options.searchRoots.push_back({test.directory / "first", "pixel"});
        test.options.searchRoots.push_back({test.directory / "second", "pixel"});
        test.requireFailure("case-only modules cannot coexist across roots of one external package");
    }
    {
        Fixture test;
        test.write("src/main.avx", "import first.Vector; import second.vector; i32 main() { return 0; }");
        test.write("first/Vector.avx", "public struct First {};");
        test.write("second/vector.avx", "public struct Second {};");
        test.options.searchRoots.push_back({test.directory / "first", "first"});
        test.options.searchRoots.push_back({test.directory / "second", "second"});
        test.requireSuccess("distinct package prefixes keep otherwise case-only module names separate");
    }
    {
        Fixture test;
        test.write("src/main.avx", "import Math.vector; i32 main() { return 0; }");
        test.write("src/math/vector.avx", "public struct Point {};");
        test.requireFailure("imports require exact path letter case");
    }
    {
        Fixture test;
        test.write("src/main.avx", "import math.vector; i32 main() { return 0; }");
        test.write("src/math/vector.avx", "public struct Point {};");
        test.write("src/math/Vector.avx", "public struct Other {};");
        std::error_code error;
        const bool sameFile = fs::equivalent(
            test.sourceRoot / "math/vector.avx",
            test.sourceRoot / "math/Vector.avx", error
        );
        // 대소문자를 구분하지 않는 파일 시스템에서는 두 파일을 표현할 수 없다.
        // Case-insensitive filesystems cannot represent these two distinct files.
        if (!error && !sameFile)
            test.requireFailure("case-only module names cannot coexist within one root");
        else
            std::cout << "SKIP: case-only coexistence requires a case-sensitive filesystem\n";
    }
    {
        Fixture test;
        test.write("src/main.avx", "import math.vector; import math.vector.ops; i32 main() { return math.vector.ops.one(); }");
        test.write("src/math/vector.avx", "public struct Point {};");
        test.write("src/math/vector/ops.avx", "public i32 one() { return 1; }");
        test.requireSuccess("module file and child-module folder can coexist");
    }
    {
        Fixture test;
        test.write("src/main.avx", "i32 main() { return 0; }");
        test.write("src/unreachable.avx", "this is deliberately malformed ???");
        test.requireSuccess("unreachable modules are not parsed");
        expect(test.graph && test.graph->modules.size() == 1, "only reachable modules enter graph");
    }
    {
        Fixture test;
        test.write("src/main.avx", "import broken; i32 main() { return 0; }");
        test.write("src/broken.avx", "i32 broken() { return 0 }");
        test.requireFailure("syntax errors in imported modules diagnose");
    }
    {
        Fixture test;
        test.write("src/main.avx", "import broken; i32 main() { return 0; }");
        std::string invalid = "// ";
        invalid += static_cast<char>(0xff);
        test.write("src/broken.avx", invalid);
        test.requireFailure("invalid UTF-8 in imported source diagnoses without crashing");
    }
    {
        Fixture test;
        test.write("src/main.avx", "import missing; i32 main() { return 0; }");
        test.requireFailure("missing imported module diagnoses");
    }
}

void scopeTests() {
    {
        Fixture test;
        test.write("src/main.avx", R"(
            interface Named { public i32 value(&Self self); };
            struct User implements Named {
                i32 field;
                public i32 value(&User self) { return self.field; }
            };
            T identity<T>(T input) where T implements Copy { return input; }
            i32 main() { return identity<i32>(1); }
        )");
        test.requireSuccess("generic constraints Self and receiver member access resolve");
    }
    {
        Fixture test;
        test.write("src/main.avx", R"(
            struct Point { public i32 x; };
            i32 scale(i32 value, i32 factor = 2) { return value * factor; }
            i32 main() {
                Point point = Point { x = 1 };
                i32 result = scale(factor = 3, value = 10);
                return result;
            }
        )");
        test.requireSuccess("field initializer and named argument labels are not value references");
    }
    {
        Fixture test;
        test.write("src/main.avx", "i32 main() { for (i32 item in 0..3) { i32 copy = item; } return item; }");
        test.requireFailure("for-loop bindings do not escape the loop");
    }
    {
        Fixture test;
        test.write("src/main.avx", R"(
            const usize count = 3;
            i32 main() {
                Array<i32, count> values;
                i32 fixed[count];
                usize bytes = Array.element_size(Array<i32, count>);
                return 0;
            }
        )");
        test.requireSuccess("named compile-time constants can provide Array extents");
    }
    {
        Fixture test;
        test.write("src/sizes.avx", "public const usize width = 4;");
        test.write("src/main.avx", R"(
            import sizes { width };
            Array<i32, width> values;
            Array<i32, sizes.width> qualified;
            i32 main() { return 0; }
        )");
        test.requireSuccess("imported compile-time constants can provide Array extents");
    }
    {
        Fixture test;
        test.write("src/main.avx", "Array<i32, missing> values; i32 main() { return 0; }");
        test.requireFailure("unknown Array extent constants diagnose");
    }
    {
        Fixture test;
        test.write("src/main.avx", R"(
            enum Event { Message { i32 code; }; };
            i32 inspect(Event event) {
                switch (event) {
                    case Event.Message { i32 value = code }: return value; break;
                }
                return 0;
            }
        )");
        test.requireFailure("outside enum patterns cannot extract private payload fields");
    }
    {
        Fixture test;
        test.write("src/main.avx", R"(
            enum Event { Message { public i32 code; }; };
            i32 inspect(Event event) {
                switch (event) {
                    case Event.Message { i32 value = missing }: return value; break;
                }
                return 0;
            }
        )");
        test.requireFailure("unknown enum payload pattern field names diagnose");
    }
    {
        Fixture test;
        test.write("src/main.avx", "i32 main() { return absent; }");
        test.requireFailure("unknown local value diagnoses");
    }
    {
        Fixture test;
        test.write("src/main.avx", "i32 main() { i32 value = 1; { i32 value = 2; } return value; }");
        test.requireFailure("nested local declarations cannot shadow active variables");
    }
    {
        Fixture test;
        test.write("src/main.avx", "i32 main() { { i32 value = 1; } { i32 value = 2; } return 0; }");
        test.requireSuccess("sibling scopes may reuse a variable name");
    }
    {
        Fixture test;
        test.write("src/main.avx", "i32 main() { { i32 value = 1; } return value; }");
        test.requireFailure("local values cannot escape their lexical block");
    }
    {
        Fixture test;
        test.write("src/main.avx", R"(
            enum Event { Move { public i32 x; }; Quit; };
            i32 inspect(Event event) {
                switch (event) {
                    case Event.Move { i32 delta = x }: return delta; break;
                    case Event.Quit: i32 delta = 0; return delta; break;
                }
                return 0;
            }
        )");
        test.requireSuccess("enum bindings are scoped separately in each case");
    }
    {
        Fixture test;
        test.write("src/main.avx", R"(
            enum Event { Move { public i32 x; }; Quit; };
            i32 inspect(Event event) {
                switch (event) {
                    case Event.Move { i32 delta = x }: break;
                    case Event.Quit: return delta; break;
                }
                return 0;
            }
        )");
        test.requireFailure("enum pattern bindings do not leak into another case");
    }
    {
        Fixture test;
        test.write("src/main.avx", "i32 choose(i32 input) { return input; } f64 choose(f64 input) { return input; } i32 main() { return choose(1); }");
        test.requireSuccess("same-origin overload group is preserved for later type selection");
        bool grouped = false;
        if (test.resolution) {
            for (const auto& [node, candidates] : test.resolution->references)
                if (node->kind == "Name" && node->value == "choose" && candidates.size() == 2)
                    grouped = true;
        }
        expect(grouped, "overloaded call retains both same-module candidates");
    }
    {
        Fixture test;
        test.write("src/left.avx", "public i32 choose(i32 input) { return input; }");
        test.write("src/right.avx", "public f64 choose(f64 input) { return input; }");
        test.write("src/main.avx", "import left { choose }; import right { choose }; i32 main() { return choose(1); }");
        test.requireFailure("overloads from different modules are not merged");
    }
    {
        Fixture test;
        test.write("src/main.avx", R"(
            void consume(i32 input) { return; }
            i32 main() { consume(i32); return 0; }
        )");
        test.requireFailure("a type cannot be passed as an ordinary runtime argument");
    }
    {
        Fixture test;
        test.write("src/main.avx", R"(
            i32 main() {
                i32 input = 1;
                i64 widened = Integer.convert(i64, input);
                Vector<i32> values = Vector.new(i32);
                Vector<i32> other = Vector<i32>.new();
                return input;
            }
        )");
        test.requireSuccess("documented intrinsics accept compile-time type arguments");
    }
    {
        Fixture test;
        test.write("src/main.avx", "i32 same(i32 input) { return input; } f64 same(i32 input) { return 1.0; }");
        test.requireFailure("return type alone does not distinguish overloads");
    }
    {
        Fixture test;
        test.write("src/main.avx", "i32 same(int input) { return input; } i32 same(i32 other) { return other; }");
        test.requireFailure("integer aliases do not create distinct overload signatures");
    }
    {
        Fixture test;
        test.write("src/main.avx", "struct Secret { i32 value; }; Secret item = Secret { value = 1 };");
        test.requireFailure("external named initialization cannot write private fields");
    }
    {
        Fixture test;
        test.write("src/main.avx", R"(
            enum Event {
                Message { i32 code; };
                public Event make(i32 input) {
                    return Event.Message { code = input };
                }
            };
            Event event = Event.make(1);
        )");
        test.requireSuccess("enum factories can initialize their own private payload fields");
    }
    {
        Fixture test;
        test.write("src/math/vector.avx", "public struct Pair<T> { public T value; };");
        test.write("src/main.avx", "import math.vector { Pair }; math.vector.Pair<i32> item; i32 main() { return 0; }");
        test.requireSuccess("qualified generic types resolve full module paths");
    }
    {
        Fixture test;
        test.write("src/main.avx", "struct Config {}; i32 Config = 1; i32 main() { return 0; }");
        test.requireFailure("direct type and value declarations share a namespace");
    }
}

}

int main() {
    try {
        preludeTests();
        importTests();
        reexportTests();
        loaderTests();
        scopeTests();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected test exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "name resolution checks=" << checks
              << " failures=" << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
