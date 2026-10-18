// ========================
//
// Created By : miruku_sora2026
//
// File name : dynamic_array_tests.cpp
//
// Description :
//     동적 배열의 실제 실행, 실패 정리와 대여 검사를 검증한다.
//     Verify actual execution, failure cleanup and borrow checks of dynamic arrays.
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
#include "aevix/JIT/JITEngine.hpp"
#include <array>
#include <cstdint>
#include <cstddef>
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
    alignas(std::max_align_t) std::array<std::byte, 65536> arena{};
    std::size_t cursor = 0, attempts = 0, allocated = 0, released = 0, failAttempt = 0;
    extern "C" void* testAllocate(std::size_t size)
    {
        if (++attempts == failAttempt) return nullptr;
        cursor = (cursor + 15) & ~std::size_t{15};
        if (size > arena.size() - cursor) return nullptr;
        auto* address = arena.data() + cursor; cursor += size; ++allocated; return address;
    }
    extern "C" void testFree(void* pointer) { if (pointer) ++released; }
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
        bool jit(int expected, std::size_t fail = 0, bool hooked = false)
        {
            if (!llvm) { for (const auto& error : diagnostics) std::cerr << error.message << '\n'; return false; }
            if (!aevix::JITEngine::available()) return true;
            aevix::JITOptions options; std::string error;
            auto engine = aevix::JITEngine::create(options, error);
            if (!engine) { std::cerr << error << '\n'; return false; }
            if (hooked)
            {
                cursor = attempts = allocated = released = 0; failAttempt = fail;
                if (!engine->defineSymbol("malloc", reinterpret_cast<std::uintptr_t>(&testAllocate), error)
                    || !engine->defineSymbol("free", reinterpret_cast<std::uintptr_t>(&testFree), error)) return false;
            }
            if (!engine->addModule(*llvm, error)) { std::cerr << error << '\n'; return false; }
            const auto result = engine->invokeI32();
            engine.reset();
            if (!result.succeeded || result.value != expected) std::cerr << "JIT: " << result.message << " actual=" << result.value << " expected=" << expected << '\n';
            return result.succeeded && result.value == expected;
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
    const std::string end = "i32 main() { Result<i32,CollectionError> result = run(); switch (move result) { case Result<i32,CollectionError>.Ok { i32 number = value }: return number; break; case Result<i32,CollectionError>.Err: return 99; break; } }";
    const auto source = [&](const std::string& body) { return "Result<i32,CollectionError> run() { " + body + " } " + end; };
    const std::vector<std::pair<std::string, int>> cases = {
        {"mut i32 values[] = [1,2,3]?; values[1] = 5; if (Vector.len(&values) != 3) return Result.ok(90); return Result.ok(values[0] + values[1] + values[2]);", 9},
        {"mut i32 grid[][] = [[1,2],[3,4,5]]?; grid[1][2] = 7; return Result.ok(grid[0][1] + grid[1][2]);", 9},
        {"const usize rank = 2; i32 grid[](rank) = [[2],[40,3]]?; return Result.ok(grid[0][0] + grid[1][0]);", 42},
        {"i32 rows[2][] = [[1],[40,3]]?; return Result.ok(rows[0][0] + rows[1][0]);", 41},
        {"i32 rows[][2] = [[1,2],[40,3]]?; return Result.ok(rows[0][1] + rows[1][0]);", 42},
        {"i32 values[] = []?; return Result.ok(Vector.len(&values) == 0 ? 42 : 9);", 42},
        {"i32 values[] = [42]?; Vector<i32> moved = values; return Result.ok(moved[0]);", 42},
        {"Result<Vector<i32>,CollectionError> stored = [1,2,3]; return Result.ok(Result.is_ok(&stored) ? 42 : 9);", 42},
        {"unit values[] = [Unit.value(),Unit.value()]?; return Result.ok(Vector.len(&values) == 2 ? 42 : 9);", 42}
    };
    for (const auto& [body, expected] : cases)
    {
        Fixture fixture(source(body)); expect(fixture.run(clang, expected), "collection AOT"); expect(fixture.jit(expected), "collection JIT");
    }
    {
        Fixture fixture(source("i32 grid[][] = [[1],[2,3]]?; return Result.ok(grid[1][1]);"));
        expect(fixture.jit(3, 0, true), "recursive ownership success");
        if (aevix::JITEngine::available()) expect(allocated == 3 && released == 3, "all successful nested allocations freed once");
        expect(fixture.jit(99, 1, true), "outer allocation failure returns Err");
        if (aevix::JITEngine::available()) expect(allocated == 0 && released == 0, "failed first allocation never publishes or frees a value");
        expect(fixture.jit(99, 3, true), "late row allocation failure returns Err");
        if (aevix::JITEngine::available()) expect(allocated == 2 && released == 2, "partial rows and outer buffer freed once");
    }
    {
        Fixture fixture(source("i32 values[] = []?; return Result.ok(42);"));
        expect(fixture.jit(42, 1, true), "empty construction needs no allocation");
        if (aevix::JITEngine::available()) expect(attempts == 0, "no allocator call for empty literal");
    }
    {
        const std::string body = "mut i32 values[] = []?; Vector.reserve(&mut values, 1)?; Result<unit,InputError<i32>> first = Vector.push(&mut values, 20); Result<unit,InputError<i32>> second = Vector.push(&mut values, 22); if (Result.is_err(&first) || Result.is_err(&second)) return Result.ok(90); return Result.ok(Vector.len(&values) == 2 && Vector.capacity(&values) >= 2 ? values[0] + values[1] : 91);";
        Fixture fixture(source(body)); expect(fixture.run(clang, 42), "reserve and growing push AOT"); expect(fixture.jit(42, 0, true), "reserve and growing push JIT");
        if (aevix::JITEngine::available()) expect(allocated == 2 && released == 2, "relocation frees old buffer without dropping moved elements");
    }
    {
        const std::string body = "mut i32 values[] = [42]?; usize enormous = 18446744073709551615; Result<unit,CollectionError> reserved = Vector.reserve(&mut values, enormous); return Result.ok(Result.is_err(&reserved) && Vector.len(&values) == 1 ? values[0] : 9);";
        Fixture fixture(source(body)); expect(fixture.run(clang, 42), "checked reserve size overflow AOT"); expect(fixture.jit(42, 0, true), "checked reserve size overflow preserves source");
        if (aevix::JITEngine::available()) expect(attempts == 1 && released == 1, "size overflow performs no allocation");
    }
    {
        const std::string body = "mut i32 values[] = [42]?; Result<unit,CollectionError> reserved = Vector.reserve(&mut values, 10); return Result.ok(Result.is_err(&reserved) && Vector.len(&values) == 1 ? values[0] : 9);";
        Fixture fixture(source(body)); expect(fixture.jit(42, 2, true), "failed reserve preserves original buffer and length");
        if (aevix::JITEngine::available()) expect(allocated == 1 && released == 1, "failed reserve original buffer freed once");
    }
    {
        const std::string body = "mut i32 rows[][] = []?; i32 row[] = [42]?; Result<unit,InputError<Vector<i32>>> added = Vector.push(&mut rows,row); switch (move added) { case Result<unit,InputError<Vector<i32>>>.Ok: return Result.ok(9); break; case Result<unit,InputError<Vector<i32>>>.Err { InputError<Vector<i32>> reason = error }: switch (move reason) { case InputError<Vector<i32>>.AllocationFailure { Vector<i32> returned = input }: return Result.ok(Vector.len(&rows) == 0 ? returned[0] : 8); break; case InputError<Vector<i32>>.SizeOverflow: return Result.ok(7); break; case InputError<Vector<i32>>.OutOfBounds: Never.unreachable(); break; } break; }";
        Fixture fixture(source(body)); expect(fixture.jit(42, 2, true), "failed push returns owning input and leaves destination unchanged");
        if (aevix::JITEngine::available()) expect(allocated == 1 && released == 1, "returned failed push owner freed exactly once");
    }
    {
        const std::string body = "mut i32 rows[][] = []?; i32 row[] = [42]?; Vector.push(&mut rows,row); return Result.ok(42);";
        Fixture fixture(source(body)); expect(fixture.jit(42, 2, true), "discarding failure result cleans returned owning input");
        if (aevix::JITEngine::available()) expect(allocated == 1 && released == 1, "discarded InputError payload freed once");
        expect(fixture.jit(42, 0, true), "successful push transfers owning input to destination");
        if (aevix::JITEngine::available()) expect(allocated == 2 && released == 2, "successful nested push buffers freed once");
    }
    {
        const std::string body = "mut i32 rows[][] = []?; i32 row[] = [42]?; Result<unit,InputError<Vector<i32>>> added = Vector.insert(&mut rows,0,row); switch (move added) { case Result<unit,InputError<Vector<i32>>>.Ok: return Result.ok(9); break; case Result<unit,InputError<Vector<i32>>>.Err { InputError<Vector<i32>> reason = error }: switch (move reason) { case InputError<Vector<i32>>.AllocationFailure { Vector<i32> returned = input }: return Result.ok(Vector.len(&rows) == 0 ? returned[0] : 8); break; case InputError<Vector<i32>>.SizeOverflow: return Result.ok(7); break; case InputError<Vector<i32>>.OutOfBounds: return Result.ok(6); break; } break; }";
        Fixture fixture(source(body)); expect(fixture.jit(42, 2, true), "failed insert returns owning input and leaves destination unchanged");
        if (aevix::JITEngine::available()) expect(attempts == 2 && allocated == 1 && released == 1, "failed insertion frees returned input exactly once");
    }
    {
        const std::string body = "mut i32 rows[][] = []?; i32 row[] = [42]?; Result<unit,InputError<Vector<i32>>> added = Vector.insert(&mut rows,1,row); switch (move added) { case Result<unit,InputError<Vector<i32>>>.Ok: return Result.ok(9); break; case Result<unit,InputError<Vector<i32>>>.Err { InputError<Vector<i32>> reason = error }: switch (move reason) { case InputError<Vector<i32>>.OutOfBounds { Vector<i32> returned = input }: return Result.ok(Vector.len(&rows) == 0 ? returned[0] : 8); break; case InputError<Vector<i32>>.SizeOverflow: return Result.ok(7); break; case InputError<Vector<i32>>.AllocationFailure: return Result.ok(6); break; } break; }";
        Fixture fixture(source(body)); expect(fixture.jit(42, 2, true), "invalid insertion index preserves owning input before any growth");
        if (aevix::JITEngine::available()) expect(attempts == 1 && allocated == 1 && released == 1, "bounds rejection never attempts buffer growth");
    }
    const auto rejects = [&](const std::string& input, const std::string& label)
    { Fixture fixture(input); if (fixture.llvm) std::cerr << "Unexpected acceptance: " << input << '\n'; expect(!fixture.llvm && !fixture.diagnostics.empty(), label); };
    rejects(source("mut i32 values[] = [1]?; &i32 loan = &values[0]; Vector.reserve(&mut values,3); return Result.ok(*loan);"), "live element loan prevents reallocation");
    rejects(source("mut i32 rows[][] = []?; i32 row[] = [1]?; Vector.push(&mut rows,row); return Result.ok(row[0]);"), "push moves owning input");
    rejects(source("i32 values[] = [1]?; Vector<i32> moved = values; return Result.ok(values[0]);"), "moved owner reuse");
    rejects(source("i32 values[] = [1]?; values[0] = 2; return Result.ok(values[0]);"), "immutable element write");
    rejects(source("mut i32 values[] = [1]?; &i32 loan = &values[0]; values[0] = 2; return Result.ok(*loan);"), "live element loan prevents conflicting write");
    rejects(source("i32 grid[](0) = []?; return Result.ok(0);"), "zero rank");
    rejects(source("mut usize rank = 2; i32 grid[](rank) = [[1]]?; return Result.ok(0);"), "runtime rank");
    rejects(source("i32 values[] = [true]?; return Result.ok(0);"), "wrong element type");
    rejects(source("i32 rows[][2] = [[1]]?; return Result.ok(0);"), "mixed fixed extent mismatch");
    rejects(source("i32 values[] = [1]; return Result.ok(0);"), "fallible literal requires handling");
    rejects("Result<&i32,CollectionError> escape() { i32 values[] = [1]?; return Result.ok(&values[0]); }", "element borrow cannot escape owner");
    {
        Fixture fixture(source("i32 values[] = [1]?; usize index = 1; return Result.ok(values[index]);"));
        expect(fixture.run(clang, 0, true), "dynamic out of bounds traps");
    }
    std::cout << checks << " dynamic array checks, " << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
