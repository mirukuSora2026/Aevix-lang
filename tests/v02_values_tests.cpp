// ========================
//
// Created By : miruku_sora2026
//
// File name : v02_values_tests.cpp
//
// Description :
//     문자열·튜플·반복 배열의 실행 결과와 대여·이동 경계를 검증한다.
//     Verify text, tuples, and repeated arrays including loan and move boundaries.
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
#include "aevix/JIT/JITEngine.hpp"
#include "aevix/Runtime/TextRuntime.hpp"
#include "ProcessTestSupport.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    unsigned checks = 0, failures = 0;
    void expect(bool condition, const std::string& name) { ++checks; if (!condition) { ++failures; std::cerr << "FAIL: " << name << '\n'; } }
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
            if (air && aevix::BorrowChecker(diagnostics).check(*air, names, types).succeeded
                && aevix::verifyAIR(*air, types.types, diagnostics))
                llvm = aevix::emitLLVM(*air, types, names, diagnostics);
        }
        bool contains(std::string_view text) const
        {
            for (const auto& item : diagnostics) if (item.message.find(text) != std::string::npos) return true;
            return false;
        }
        bool jit(int expected)
        {
            if (!llvm) { for (const auto& item : diagnostics) std::cerr << item.message << '\n'; return false; }
            std::string error; auto engine = aevix::JITEngine::create({}, error);
            if (!engine || !engine->addModule(*llvm, error)) { std::cerr << error << '\n'; return false; }
            const auto result = engine->invokeI32();
            if (!result.succeeded) std::cerr << result.message << '\n';
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
    const auto run = [&](const std::string& source, const std::string& name)
    {
        Fixture fixture(source);
        expect(fixture.run(clang, 0), "AOT " + name);
        if (aevix::JITEngine::available()) expect(fixture.jit(0), "JIT " + name);
    };
    const std::string entry = R"(
        i32 main() {
            rsn result = run();
            switch (move result) {
                case Result<i32, TextError>.Ok { i32 status = value }: return status; break;
                default: return 99; break;
            }
        }
    )";
    run(R"(
        Result<i32, TextError> run() {
            string_view literal = "Aé한🙂\n\0";
            if (string.byte_len(literal) != 12 || string.scalar_count(literal) != 6) return Result.ok(1);
            if (!string.equals(literal, "\u{41}é한🙂\n\0")) return Result.ok(2);
            mut string owner = string.from_view(literal)?;
            string_view original = string.view(&owner);
            if (!string.equals(original, literal)) return Result.ok(3);
            string.append(&mut owner, "!")?;
            string.push_char(&mut owner, 'é')?;
            if (string.byte_len(&owner) != 15 || string.scalar_count(&owner) != 8) return Result.ok(4);
            string_view part = string.slice_bytes(&owner, 1, 6)?;
            if (!string.equals(part, "é한")) return Result.ok(5);
            rsn invalid_boundary = string.slice_bytes(&owner, 2, 6);
            if (Result.is_ok(&invalid_boundary)) return Result.ok(6);
            rsn reversed = string.slice_bytes(&owner, 6, 1);
            if (Result.is_ok(&reversed)) return Result.ok(7);
            rsn past_end = string.slice_bytes(&owner, 0, 1000);
            if (Result.is_ok(&past_end)) return Result.ok(8);
            string.clear(&mut owner);
            if (!string.is_empty(&owner) || string.byte_len(&owner) != 0) return Result.ok(9);
            return Result.ok(0);
        }
    )" + entry, "UTF8 owners views mutation and byte boundaries");
    run(R"(
        Result<i32, TextError> run() {
            byte valid[3] = [Byte.from_u8(0x41u8), Byte.from_u8(0xc3u8), Byte.from_u8(0xa9u8)];
            Slice<byte> valid_view = Slice.from_array(&valid);
            string text = string.from_utf8(valid_view)?;
            if (!string.equals(&text, "Aé")) return Result.ok(1);
            if (Byte.to_u8(valid[0]) != 0x41u8) return Result.ok(2);
            byte invalid[2] = [Byte.from_u8(0xc0u8), Byte.from_u8(0xafu8)];
            Slice<byte> invalid_view = Slice.from_array(&invalid);
            rsn rejected = string.from_utf8(invalid_view);
            if (Result.is_ok(&rejected)) return Result.ok(3);
            return Result.ok(0);
        }
    )" + entry, "explicit byte conversion and UTF8 validation");
    run(R"(
        i32 main() {
            mut Tuple<i32, bool> pair = Tuple.make(3, true);
            if (Tuple.len(&pair) != 2 || Tuple.is_empty(&pair)) return 1;
            if (*Tuple.get<0>(&pair) != 3 || !*Tuple.get<1>(&pair)) return 2;
            {
                &mut i32 first = Tuple.get_mut<0>(&mut pair);
                *first = 12;
            }
            Tuple<i32, bool> copied = Tuple.copy(&pair);
            if (*Tuple.get<0>(&copied) != 12 || *Tuple.get<0>(&pair) != 12) return 3;
            Tuple<> empty = Tuple.make();
            if (!Tuple.is_empty(&empty) || Tuple.len(&empty) != 0) return 4;
            return 0;
        }
    )", "tuple explicit copy constant indexing and mutable field loans");
    run(R"(
        Result<i32, TextError> run() {
            string first = string.from_view("owned")?;
            Tuple<string, i32> pair = Tuple.make(first, 7);
            Tuple<string, i32> moved = pair;
            &string view = Tuple.get<0>(&moved);
            if (!string.equals(view, "owned") || *Tuple.get<1>(&moved) != 7) return Result.ok(1);
            return Result.ok(0);
        }
    )" + entry, "owned tuple move and nested string cleanup");
    run(R"(
        i32 once(&mut i32 count) { (*count)++; return 7; }
        struct Point implements Copy { public i32 value; };
        i32 main() {
            mut i32 count = 0;
            i32 values[3] = [once(&mut count); 3];
            i32 empty[0] = [once(&mut count); 0];
            Point points[2] = [Point { value = 5 }; 2];
            if (count != 2 || values[0] != 7 || values[2] != 7 || points[1].value != 5) return 1;
            mut i32 total = 0;
            for (i32 value : &values) total += value;
            return total == 21 ? 0 : 2;
        }
    )", "repeated arrays evaluate once including zero length");
    run(R"(
        Result<i32, TextError> run() {
            string owner = string.from_view("Aé한🙂")?;
            mut usize count = 0;
            mut bool korean = false;
            for (char scalar : &owner) {
                if (scalar == 'é') continue;
                count++;
                if (scalar == '한') korean = true;
            }
            if (count != 3 || !korean) return Result.ok(1);
            string_view view = "é🙂";
            mut usize seen = 0;
            for (char scalar : view) seen++;
            if (seen != 2) return Result.ok(2);
            return Result.ok(0);
        }
    )" + entry, "enhanced UTF8 loops iterate scalars and continue correctly");
    run(R"(
        const string_view greeting = "Aé한🙂";
        const string_view alias = greeting;
        string_view read_static() { return alias; }
        i32 main() {
            string_view value = read_static();
            return string.byte_len(value) == 10 && string.scalar_count(value) == 4 ? 0 : 1;
        }
    )", "static UTF8 constants and aliases retain static provenance");
    for (const auto& source : {
        "Result<string_view, TextError> escape() { string local = string.from_view(\"owner\")?; return Result.ok(string.view(&local)); } i32 main() { return 0; }",
        "&i32 escape() { Tuple<i32> local = Tuple.make(7); return Tuple.get<0>(&local); } i32 main() { return 0; }",
        "Tuple<&i32> escape() { i32 local = 7; return Tuple.make(&local); } i32 main() { return 0; }",
        "i32 main() { mut Tuple<i32> pair = Tuple.make(7); &i32 first = Tuple.get<0>(&pair); &mut i32 second = Tuple.get_mut<0>(&mut pair); *second = 1; return *first; }",
        "Result<i32, TextError> test() { string owner = string.from_view(\"a\")?; Tuple<string> pair = Tuple.make(owner); string moved = *Tuple.get<0>(&pair); return Result.ok(0); } i32 main() { return 0; }",
        "i32 main() { Tuple<i32> first = Tuple.make(7); Tuple<i32> second = first; return *Tuple.get<0>(&first); }",
        "i32 main() { Tuple<i32> pair = Tuple.make(7); return *Tuple.get<1>(&pair); }",
        "i32 main() { Tuple<i32> pair = Tuple.make(7); &mut i32 wrong = Tuple.get_mut<0>(&pair); return 0; }",
        "i32 main() { i32 wrong[3] = [0; 2]; return 0; }",
        "i32 main() { i32 wrong[3] = [0; -1]; return 0; }",
        "i32 main() { i32 wrong = Byte.from_u8(1u8); return 0; }",
        "i32 main() { u8 wrong = Byte.to_u8(1u8); return 0; }",
        "i32 main() { string owner; string duplicate[0] = [owner; 0]; return 0; }",
        "Result<i32, TextError> test() { mut string owner = string.from_view(\"a\")?; string_view view = string.view(&owner); string.append(&mut owner, \"b\")?; return Result.ok(string.is_empty(view) ? 0 : 1); } i32 main() { return 0; }",
        "Result<i32, TextError> test() { string owner = string.from_view(\"a\")?; Tuple<string, i32> pair = Tuple.make(owner, 1); Tuple<string, i32> copy = Tuple.copy(&pair); return Result.ok(0); } i32 main() { return 0; }"
    })
    {
        Fixture fixture(source); expect(!fixture.llvm && !fixture.diagnostics.empty(), "reject invalid type, move, index, repeat, or escaping loan");
    }
    std::cout << checks << " value checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
