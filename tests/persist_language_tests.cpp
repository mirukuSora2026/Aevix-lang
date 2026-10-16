// ========================
//
// Created By : miruku_sora2026
//
// File name : persist_language_tests.cpp
//
// Description :
//     실제 소스로 persist 초기화, 저장소 공유, 의존 순서와 대여 안전성을 검증한다.
//     Verify persist initialization, shared storage, dependency order, and borrowing from real sources.
//
// ========================
#include "aevix/Frontend/Sema/NameResolver.hpp"
#include "aevix/Frontend/Sema/ForeignValidator.hpp"
#include "aevix/JIT/JITEngine.hpp"
#include "aevix/Frontend/Sema/CoreErrorTypes.hpp"
#include "aevix/Frontend/Sema/GenericResolver.hpp"
#include "aevix/Frontend/Sema/ControlFlowChecker.hpp"
#include "aevix/Frontend/Ownership/BorrowChecker.hpp"
#include "aevix/IR/Lowering/ASTLowering.hpp"
#include "aevix/Backend/LLVM/LLVMCodeGen.hpp"
#include "aevix/Backend/AOT/AOTCompiler.hpp"
#include "ProcessTestSupport.hpp"
#include <chrono>
#include <map>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    unsigned checks = 0, failures = 0;
    void expect(bool condition, const std::string& name)
    {
        ++checks;
        if (!condition) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
    }
    struct Fixture final
    {
        std::filesystem::path directory;
        aevix::SourceManager sources;
        aevix::Diagnostics diagnostics;
        std::optional<aevix::ModuleGraph> graph;
        aevix::NameResolution names;
        aevix::TypeCheckResult types;
        std::optional<aevix::AIRModule> air;
        std::optional<std::string> llvm;
        explicit Fixture(const std::string& source, const std::map<std::string, std::string>& files = {})
        {
            static unsigned sequence = 0;
            directory = std::filesystem::temp_directory_path() / ("aevix-air-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++sequence));
            std::filesystem::create_directories(directory);
            { std::ofstream file(directory / "main.avx"); file << source; }
            for (const auto& [relative, content] : files)
            {
                std::filesystem::create_directories((directory / relative).parent_path());
                std::ofstream file(directory / relative); file << content;
            }
            graph = aevix::ModuleLoader(sources, diagnostics).load(directory / "main.avx", {});
            if (!graph || !diagnostics.empty()) return;
            if (!aevix::installCoreErrorTypes(*graph, sources, diagnostics)) return;
            names = aevix::NameResolver(diagnostics).resolve(*graph);
            if (!names.succeeded) return;
            auto concrete = aevix::GenericResolver(diagnostics).resolve(*graph, names);
            if (!concrete) return;
            names = std::move(*concrete);
            types = aevix::TypeChecker(diagnostics).check(*graph, names);
            if (!diagnostics.empty() || !aevix::validateForeignDeclarations(names, types, diagnostics)) return;
            if (!aevix::ControlFlowChecker(diagnostics).check(*graph, names, types)) return;
            air = aevix::lowerToAIR(*graph, names, types, diagnostics);
            if (air && aevix::BorrowChecker(diagnostics).check(*air, names, types).succeeded)
                llvm = aevix::emitLLVM(*air, types, names, diagnostics);
        }
        ~Fixture() { std::error_code ec; std::filesystem::remove_all(directory, ec); }
        bool run(const std::filesystem::path& clang, int expected, bool trap = false, unsigned optimization = 0)
        {
            if (!llvm)
            {
                for (const auto& diagnostic : diagnostics) std::cerr << diagnostic.message << '\n';
                return false;
            }
            aevix::AOTOptions options;
            options.clang = clang;
            options.output = aevix::test::executablePath(directory, "program");
            options.optimization = optimization;
#ifdef AEVIX_TEST_RUNTIME_ARCHIVE
            options.runtimeObjects.push_back(AEVIX_TEST_RUNTIME_ARCHIVE);
#endif
            const auto compiled = aevix::compileAOT(*llvm, options);
            if (!compiled.succeeded) { std::cerr << compiled.message << '\n' << *llvm; return false; }
            aevix::ProcessOptions execution;
            execution.timeout = std::chrono::seconds(5);
            execution.stderrFile = directory / "program.stderr";
            const auto process = aevix::runProcess({aevix::utf8Path(options.output)}, execution);
            return trap ? aevix::test::persistAbort(process, execution.stderrFile)
                        : aevix::test::exitedWith(process, expected);
        }
    };
    void rejects(const std::string& source, const std::string& fragment, const std::string& label)
    {
        Fixture fixture(source);
        bool found = false;
        for (const auto& diagnostic : fixture.diagnostics) found = found || diagnostic.message.find(fragment) != std::string::npos;
        if (!found) for (const auto& diagnostic : fixture.diagnostics) std::cerr << diagnostic.message << '\n';
        expect(!fixture.llvm && found, label);
    }
}
int main(int argc, char** argv)
{
    const std::filesystem::path clang = argc > 1 ? argv[1] : "/opt/homebrew/opt/llvm/bin/clang";
    if (!std::filesystem::exists(clang)) return 77;
    expect(Fixture("persist i32 number = 42; i32 main() { return number; }").run(clang, 42), "top-level persist before main");
    expect(Fixture("persist i32 later = 9; persist i32 first = later + 1; i32 main() { return first; }").run(clang, 10), "global initializer reads another managed value");
    expect(Fixture("persist i32 first = read_later(); persist i32 later = 9; i32 read_later() { return later; } i32 main() { return first; }").run(clang, 9), "function-hidden global initialization dependency");
    expect(Fixture("mut i32 trace = 0; i32 mark(i32 value) { trace = trace * 10 + value; return value; } persist i32 first = mark(1); persist i32 second = mark(2); i32 main() { return trace; }").run(clang, 12), "unrelated initialization follows source order");
    expect(Fixture("mut i32 trace = 0; i32 mark() { trace++; return 7; } i32 cached() { persist i32 value = mark(); return value; } i32 main() { i32 left = cached(); i32 right = cached(); return trace == 1 && left == 7 && right == 7 ? 7 : 99; }").run(clang, 7), "local persist initializes once across calls");
    expect(Fixture("mut i32 trace = 0; i32 mark() { trace++; return 7; } i32 unused() { persist i32 value = mark(); return value; } i32 main() { return trace; }").run(clang, 0), "unreached local persist stays uninitialized");
    expect(Fixture("mut i32 trace = 0; i32 mark() { trace++; return trace; } i32 cached<T>() { persist i32 value = mark(); return value; } i32 main() { i32 first = cached<i32>(); i32 repeat = cached<i32>(); i32 other = cached<f64>(); return first == 1 && repeat == 1 && other == 2 && trace == 2 ? 7 : 99; }").run(clang, 7), "generic argument combinations own separate persist storage");
    expect(Fixture("i32 cached() { const i32 seed = 7; persist i32 value = seed + 2; return value; } i32 main() { return cached(); }").run(clang, 9), "compile-time local constant may seed persist");
    expect(Fixture("struct Point { public i32 x; public i32 y; }; Point make() { return Point { x = 7, y = 9 }; } persist Point origin = make(); i32 main() { return origin.x + origin.y; }").run(clang, 16), "owning struct global initializer uses AIR");
    expect(Fixture("persist i32 values[3] = [2, 3, 5]; i32 main() { return values[1]; }").run(clang, 3), "static array persist initializer");
    expect(Fixture("enum Choice { Empty; Number { public i32 value; }; }; persist Choice selected = Choice.Number { value = 7 }; i32 main() { switch (selected) { case Choice.Number { i32 number = value }: return number; break; case Choice.Empty: return 0; break; } }").run(clang, 7), "enum union persist initializer");
    expect(Fixture("persist Result<i32,i32> status = Result.ok(7); i32 main() { return Result.is_ok(&status) ? 7 : 99; }").run(clang, 7), "core Result global uses managed enum initialization");
    expect(Fixture("extern \"C\" i32 abs(i32 value); i32 main() { return abs(-7); }").run(clang, 7), "scalar C external call lowers to real C symbol");
    expect(Fixture("import zeta { z }; import alpha { a }; import common { trace }; i32 main() { return trace; }", {
        {"common.avx", "public mut i32 trace = 0; public i32 mark(i32 value) { trace = trace * 10 + value; return value; }"},
        {"alpha.avx", "import common { mark }; public persist i32 a = mark(1);"},
        {"zeta.avx", "import common { mark }; public persist i32 z = mark(2);"}
    }).run(clang, 12), "module-path order precedes import source order");
    expect(Fixture("import helper { exposed }; import common { trace }; i32 main() { return trace; }", {
        {"common.avx", "public mut i32 trace = 0; public i32 mark() { trace++; return 7; }"},
        {"helper.avx", "import common { mark }; public const i32 exposed = 1; persist i32 unused = mark();"}
    }).run(clang, 1), "all connected modules initialize unselected top-level persist");
    expect(Fixture("mut i32 target = 1; i32 modify() { target = 2; return 7; } i32 main() { persist i32 cached = modify(); &i32 loan = &target; return *loan + cached; }").run(clang, 9), "initialized local persist access does not rerun effects under a loan");
    expect(Fixture("i32 recursive() { persist i32 cached = recursive(); return cached; } i32 main() { return recursive(); }").run(clang, 0, true), "local recursive initialization stops with a runtime diagnostic");
    rejects("i32 cached(i32 input) { persist i32 value = input; return value; } i32 main() { return cached(1); }", "cannot depend directly", "parameter capture is rejected");
    rejects("i32 cached() { i32 input = 1; persist i32 value = input; return value; } i32 main() { return cached(); }", "cannot depend directly", "ordinary local capture is rejected");
    rejects("i32 main() { persist i32 value; return 0; }", "initializer", "persist requires a declaration initializer");
    rejects("i32 main() { persist mut i32 value = 1; return value; }", "synchronization", "plain shared mutation is rejected");
    rejects("persist i32 first = read_second(); persist i32 second = read_first(); i32 read_first() { return first; } i32 read_second() { return second; } i32 main() { return first; }", "cyclic persist", "known top-level initialization cycles are rejected");
    rejects("mut i32 target = 1; i32 modify() { target = 2; return 7; } i32 cached() { persist i32 value = modify(); return value; } i32 main() { &i32 loan = &target; i32 result = cached(); return *loan + result; }", "borrow", "transitive initializer write cannot invalidate a live loan");
    rejects("mut i32 target = 1; i32 modify() { target = 2; return 7; } i32 main() { &i32 loan = &target; persist i32 value = modify(); return *loan + value; }", "borrow", "direct local initializer write cannot invalidate a live loan");
    rejects("Result<i32,i32> load() { return Result.ok(7); } Result<i32,i32> cached() { persist i32 value = load()?; return Result.ok(value); } i32 main() { return 0; }", "persist initialization cannot propagate", "persist initializer cannot return through its enclosing function");
    rejects("extern \"C\" i32 abs(i32 value); mut i32 target = 1; i32 main() { &i32 loan = &target; persist i32 value = abs(-7); return *loan + value; }", "external function while loans", "opaque native initializer cannot run through an active loan");
    rejects("extern \"C\" i32 abs(i32 value); mut i32 target = 1; i32 hidden() { return abs(-7); } i32 main() { &i32 loan = &target; return hidden() + *loan; }", "unknown external effects", "ordinary wrapper cannot hide external effects from a loan");
    if (aevix::JITEngine::available())
    {
        Fixture fixture("mut i32 trace = 0; i32 mark() { trace++; return trace; } i32 probe() { persist i32 value = mark(); return value; }");
        std::string error;
        auto engine = aevix::JITEngine::create({}, error);
        bool loaded = engine && fixture.llvm && engine->addModule(*fixture.llvm, error);
        expect(loaded, "compiled persist LLVM loads into native JIT");
        if (loaded)
        {
            aevix::SymbolId symbol = aevix::InvalidSymbolId;
            for (const auto& function : fixture.air->functions) if (function.name == "probe") symbol = function.symbol;
            const auto first = engine->invokeI32("aevix.fn." + std::to_string(symbol));
            const auto second = engine->invokeI32("aevix.fn." + std::to_string(symbol));
            expect(first.succeeded && first.value == 1 && second.succeeded && second.value == 1, "compiled local persist storage is shared across JIT calls");
        }
        else
        {
            std::cerr << error << '\n';
            for (const auto& diagnostic : fixture.diagnostics) std::cerr << diagnostic.message << '\n';
        }
    }
    std::cout << checks << " persist language checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
