// ========================
//
// Created By : miruku_sora2026
//
// File name : error_handling_tests.cpp
//
// Description :
//     내장 오류 운반 타입과 오류 전파의 실제 실행 및 소유권 검사를 검증한다.
//     Verify actual execution and ownership checks of builtin error carriers and propagation.
//
// ========================
#include "aevix/Frontend/Sema/NameResolver.hpp"
#include "aevix/Frontend/Sema/CoreErrorTypes.hpp"
#include "aevix/Frontend/Sema/GenericResolver.hpp"
#include "aevix/Frontend/Sema/ControlFlowChecker.hpp"
#include "aevix/Frontend/Ownership/BorrowChecker.hpp"
#include "aevix/IR/Lowering/ASTLowering.hpp"
#include "aevix/Backend/LLVM/LLVMCodeGen.hpp"
#include "aevix/Backend/AOT/AOTCompiler.hpp"
#include "ProcessTestSupport.hpp"
#include <chrono>
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
        explicit Fixture(const std::string& source)
        {
            static unsigned sequence = 0;
            directory = std::filesystem::temp_directory_path() / ("aevix-air-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++sequence));
            std::filesystem::create_directories(directory);
            { std::ofstream file(directory / "main.avx"); file << source; }
            graph = aevix::ModuleLoader(sources, diagnostics).load(directory / "main.avx", {});
            if (!graph || !diagnostics.empty()) return;
            if (!aevix::installCoreErrorTypes(*graph, sources, diagnostics)) return;
            names = aevix::NameResolver(diagnostics).resolve(*graph);
            if (!names.succeeded) return;
            auto concrete = aevix::GenericResolver(diagnostics).resolve(*graph, names);
            if (!concrete) return;
            names = std::move(*concrete);
            types = aevix::TypeChecker(diagnostics).check(*graph, names);
            if (!diagnostics.empty()) return;
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
            const auto process = aevix::runProcess({aevix::utf8Path(options.output)}, execution);
            return trap ? aevix::test::arithmeticTrap(process) : aevix::test::exitedWith(process, expected);
        }
    };
}
int main(int argc, char** argv)
{
    const std::filesystem::path clang = argc > 1 ? argv[1] : "/opt/homebrew/opt/llvm/bin/clang";
    if (!std::filesystem::exists(clang)) return 77;
    expect(Fixture("Result<i32,i32> value() { return Result.ok(42); } i32 main() { Result<i32,i32> result = value(); switch (result) { case Result<i32,i32>.Ok { i32 item = value }: return item; break; case Result<i32,i32>.Err { i32 code = error }: return code; break; } }").run(clang,42), "Result context constructor and switch");
    expect(Fixture("Result<i32,i32> value() { return Result.ok(42); } Result<i32,i32> forward() { i32 item = value()?; return Result.ok(item); } i32 main() { Result<i32,i32> result = forward(); return Result.is_ok(&result) ? 42 : 0; }").run(clang,42), "Result success propagation");
    expect(Fixture("Result<i32,i32> value() { return Result.err(7); } Result<bool,i32> forward() { i32 item = value()?; return Result.ok(item > 0); } i32 main() { Result<bool,i32> result = forward(); return Result.is_err(&result) ? 7 : 0; }").run(clang,7), "Result error propagation with different success type");
    expect(Fixture("Optional<i32> value() { return Optional.some(42); } Optional<i32> forward() { i32 item = value()?; return Optional.some(item); } i32 main() { Optional<i32> result = forward(); return Optional.is_some(&result) ? 42 : 0; }").run(clang,42), "Optional success propagation");
    expect(Fixture("Optional<i32> value() { return Optional.none(); } Optional<bool> forward() { i32 item = value()?; return Optional.some(item > 0); } i32 main() { Optional<bool> result = forward(); return Optional.is_none(&result) ? 7 : 0; }").run(clang,7), "Optional none propagation with different success type");
    expect(Fixture("i32 main() { rsn result = Result.ok<i32,i32>(42); rsn optional = Optional.some(3); rsn absent = Optional.none<i32>(); return Result.is_ok(&result) && Optional.is_some(&optional) && Optional.is_none(&absent) ? 42 : 0; }").run(clang,42), "Explicit Result and inferred Optional constructors");
    expect(Fixture("struct Token { public i32 value; }; Result<Token,i32> value() { return Result.ok(Token { value = 42 }); } Result<i32,i32> forward() { Token item = value()?; return Result.ok(item.value); } i32 main() { Result<i32,i32> result = forward(); switch (result) { case Result<i32,i32>.Ok { i32 item = value }: return item; break; case Result<i32,i32>.Err: return 1; break; } }").run(clang,42), "owned struct payload extraction");
    expect(Fixture("Result<&i32,i32> identity(&i32 value) { return Result.ok(value); } Result<i32,i32> forward() { i32 item = 42; &i32 borrowed = identity(&item)?; return Result.ok(*borrowed); } i32 main() { Result<i32,i32> result = forward(); return Result.is_ok(&result) ? 42 : 0; }").run(clang,42), "borrowed payload retains source lifetime");
    expect(Fixture("Result<i32,i32> next(&mut i32 count, bool fail) { (*count)++; if (fail) return Result.err(*count); return Result.ok(*count); } Result<i32,i32> forward(&mut i32 count) { i32 first = next(&mut *count, false)?; i32 second = next(&mut *count, true)?; i32 third = next(&mut *count, false)?; return Result.ok(first + second + third); } i32 main() { mut i32 count = 0; Result<i32,i32> result = forward(&mut count); return Result.is_err(&result) ? count : 99; }").run(clang,2), "propagation evaluates once and skips later side effects");
    {
        Fixture test("struct Token { public i32 value; }; i32 consume(Token item, i32 count) { return item.value + count; } Result<i32,i32> fail() { return Result.err(7); } Result<i32,i32> forward() { Token item = Token { value = 42 }; i32 value = consume(item, fail()?); return Result.ok(value); } i32 main() { Result<i32,i32> result = forward(); return Result.is_err(&result) ? 7 : 0; }");
        expect(test.run(clang,7), "failure during later argument returns error");
        bool dropped = false;
        if (test.air) for (const auto& function : test.air->functions) for (const auto& block : function.blocks)
            if (block.name == "propagate.error") for (const auto& instruction : block.instructions)
                if (instruction.opcode == aevix::AIROpcode::Drop && instruction.place.local < function.locals.size() && function.locals[instruction.place.local].name == "$argument") dropped = true;
        expect(dropped, "earlier owning argument is cleaned on propagation failure");
    }
    expect(Fixture("Result<Result<i32,bool>,i32> nested() { return Result.ok(Result.ok(42)); } i32 main() { Result<Result<i32,bool>,i32> result = nested(); return Result.is_ok(&result) ? 42 : 0; }").run(clang,42), "nested Result constructors receive payload context");
    expect(Fixture("i32 inspect(Result<i32,i32> result) { return Result.is_ok(&result) ? 42 : 0; } i32 main() { return inspect(Result.ok(7)); }").run(clang,42), "function parameter provides constructor context");
    {
        Fixture test("struct Token { public i32 value; }; struct Group { public Token token; public i32 count; }; Result<i32,i32> fail() { return Result.err(7); } Result<i32,i32> forward() { Group value = Group { token = Token { value = 42 }, count = fail()? }; return Result.ok(value.count); } i32 main() { Result<i32,i32> result = forward(); return Result.is_err(&result) ? 7 : 0; }");
        expect(test.run(clang,7), "later aggregate field failure returns error");
        bool dropped = false;
        if (test.air) for (const auto& function : test.air->functions) for (const auto& block : function.blocks)
            if (block.name == "propagate.error") for (const auto& instruction : block.instructions)
                if (instruction.opcode == aevix::AIROpcode::Drop && instruction.place.local < function.locals.size() && function.locals[instruction.place.local].name == "$aggregatefield") dropped = true;
        expect(dropped, "earlier owning aggregate field is cleaned on propagation failure");
    }
    expect(Fixture("Result<unit,i32> complete() { return Result.ok(Unit.value()); } Result<i32,i32> forward() { complete()?; return Result.ok(42); } i32 main() { Result<i32,i32> result = forward(); return Result.is_ok(&result) ? 42 : 0; }").run(clang,42), "unit-valued success propagation");
    const auto rejects = [&](const std::string& source, const std::string& diagnostic, const std::string& label)
    {
        Fixture test(source); bool found = false;
        for (const auto& error : test.diagnostics) if (error.message.find(diagnostic) != std::string::npos) found = true;
        if (!found) for (const auto& error : test.diagnostics) std::cerr << error.message << '\n';
        expect(!test.llvm && found, label);
    };
    rejects("i32 main() { i32 Unit = 7; unit value = Unit.value(); return 0; }", "matching overload", "local Unit variable does not resolve as intrinsic namespace");
    rejects("Result<i32,i32> source() { return Result.ok(1); } Result<i32,bool> target() { return Result.ok(source()?); }", "same error type", "mismatched Result error type rejected");
    rejects("Result<i32,i32> source() { return Result.ok(1); } i32 main() { return source()?; }", "same carrier kind", "propagation requires carrier-returning function");
    rejects("Result<i32,i32> target() { Result<i32,i32> item = Result.ok(1); i32 value = (&item)?; return Result.ok(value); }", "owned Result", "borrowed Result propagation rejected");
    rejects("Optional<i32> source() { return Optional.none(); } Result<i32,i32> target() { return Result.ok(source()?); }", "same carrier kind", "Optional cannot propagate into Result");
    rejects("i32 main() { rsn item = Result.ok(1); return 0; }", "complete contextual", "Result constructor cannot invent missing error type");
    rejects("i32 main() { rsn item = Optional.none(); return 0; }", "complete contextual", "Optional none needs context or explicit type");
    rejects("Result<i32,i32> target() { i32 item = 7?; return Result.ok(item); }", "owned Result", "ordinary scalar propagation rejected");
    rejects("Result<i32,i32> target() { Result<i32,i32> item = Result.ok(7); i32 value = item?; bool again = Result.is_ok(&item); return Result.ok(value); }", "moved", "propagation consumes original carrier");
    rejects("Result<&i32,i32> target() { i32 item = 7; return Result.ok(&item); }", "local", "Result cannot return dangling borrowed payload");
    rejects("Result<i32,&i32> target() { i32 item = 7; return Result.err(&item); }", "local", "Result cannot return dangling error borrow");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
