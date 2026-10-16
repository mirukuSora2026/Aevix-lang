// ========================
//
// Created By : miruku_sora2026
//
// File name : v02_numeric_tests.cpp
//
// Description :
//     숫자 접미사, 검사된 변환과 이식 가능한 16비트 부동소수 연산을 검증한다.
//     Verify numeric suffixes, checked conversions, and portable 16-bit floating arithmetic.
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
    run(R"(
        T identity<T>(T value) { return value; }
        i32 pick(i8 value) { return 1; }
        i32 pick(i32 value) { return 2; }
        i32 main() {
            const i8 minimum = -128i8;
            const u64 maximum = 18446744073709551615u64;
            rsn typed = identity(7i16);
            i16 other = typed;
            if (minimum != -128i8 || maximum != 0xffffffffffffffffu64) return 1;
            if (pick(7i8) != 1 || pick(7) != 2 || other != 7i16) return 2;
            f32 integer_form = 42f32;
            return integer_form == 42.0f32 ? 0 : 3;
        }
    )", "typed suffixes and signed minimum");
    run(R"(
        i32 main() {
            f16 a = 1.5f16;
            f16 b = 2.0f16;
            if (a + b != 3.5f16 || a * b != 3.0f16 || b - a != 0.5f16 || a / b != 0.75f16) return 1;
            if (-a != -1.5f16 || !(a < b) || !(b >= a)) return 2;
            bf16 x = 1.5bf16;
            bf16 y = 2.0bf16;
            if (x + y != 3.5bf16 || x / y != 0.75bf16 || -x != -1.5bf16) return 3;
            f16 sub = 0.000000059604644775390625f16;
            if (sub + sub != 0.00000011920928955078125f16) return 4;
            f16 zero = 0.0f16;
            f16 nan = zero / zero;
            f16 inf = 1.0f16 / zero;
            if (nan == nan || !(nan != nan) || !(inf > 65504.0f16)) return 5;
            return 0;
        }
    )", "f16 bf16 arithmetic subnormals infinity NaN");
    run(R"(
        i32 main() {
            Result<i8, IntegerError> good = Integer.convert<i8>(-128i64);
            Result<i8, IntegerError> bad = Integer.convert<i8>(128i32);
            Result<u64, IntegerError> negative = Integer.convert<u64>(-1i64);
            Result<i64, IntegerError> too_big = Integer.convert<i64>(18446744073709551615u64);
            Result<u64, IntegerError> full = Integer.convert<u64>(18446744073709551615u64);
            if (Result.is_err(&good) || Result.is_ok(&bad) || Result.is_ok(&negative) || Result.is_ok(&too_big) || Result.is_err(&full)) return 1;
            Result<f16, FloatError> rounded = Float.convert<f16>(1.0001f64);
            Result<f16, FloatError> precise = Float.convert_exact<f16>(1.0001f64);
            Result<bf16, FloatError> exact = Float.convert_exact<bf16>(1.5f64);
            Result<f16, FloatError> overflow = Float.convert<f16>(70000.0f64);
            if (Result.is_err(&rounded) || Result.is_ok(&precise) || Result.is_err(&exact) || Result.is_ok(&overflow)) return 2;
            Result<i32, FloatError> number = Float.to_integer_exact<i32>(42.0f64);
            Result<i32, FloatError> fraction = Float.to_integer_exact<i32>(42.5f64);
            Result<u64, FloatError> limit = Float.to_integer_exact<u64>(18446744073709551616.0f64);
            if (Result.is_err(&number) || Result.is_ok(&fraction) || Result.is_ok(&limit)) return 3;
            f64 zero = 0.0f64;
            Result<f32, FloatError> inf = Float.convert<f32>(1.0f64 / zero);
            Result<i32, FloatError> nan = Float.to_integer_exact<i32>(zero / zero);
            if (Result.is_ok(&inf) || Result.is_ok(&nan)) return 4;
            Result<f64, FloatError> inexact_int = Float.convert_exact<f64>(9007199254740993u64);
            if (Result.is_ok(&inexact_int)) return 5;
            return 0;
        }
    )", "checked numeric conversions boundary rejection");
    run(R"(
        bool half_equal(f64 input, f16 expected) {
            rsn result = Float.convert<f16>(input);
            switch (result) {
                case Result<f16, FloatError>.Ok { f16 converted = value }: return converted == expected; break;
                default: return false; break;
            }
        }
        bool bfloat_equal(f64 input, bf16 expected) {
            rsn result = Float.convert<bf16>(input);
            switch (result) {
                case Result<bf16, FloatError>.Ok { bf16 converted = value }: return converted == expected; break;
                default: return false; break;
            }
        }
        i32 main() {
            if (!half_equal(1.00048828125f64, 1.0f16)) return 1;
            if (!half_equal(1.00146484375f64, 1.001953125f16)) return 2;
            if (!half_equal(65519.0f64, 65504.0f16)) return 3;
            if (!half_equal(0.0000000298023223876953125f64, 0.0f16)) return 4;
            if (!half_equal(0.0000000894069671630859375f64, 0.00000011920928955078125f16)) return 5;
            if (!bfloat_equal(1.00390625f64, 1.0bf16)) return 6;
            if (!bfloat_equal(1.01171875f64, 1.015625bf16)) return 7;
            rsn maximum_finite = Float.to_integer_exact<u64>(18446744073709549568.0f64);
            if (Result.is_err(&maximum_finite)) return 8;
            rsn unsigned_double_round = Float.convert_exact<bf16>(9259400833873739777u64);
            if (Result.is_ok(&unsigned_double_round)) return 9;
            return 0;
        }
    )", "ties-even rounding and finite integer endpoints");
    run(R"(
        Result<i8, IntegerError> narrow(i64 value) { return Result.ok(Integer.convert<i8>(value)?); }
        i32 main() {
            Result<i8, IntegerError> good = narrow(12i64);
            Result<i8, IntegerError> bad = narrow(300i64);
            if (Result.is_err(&good) || Result.is_ok(&bad)) return 1;
            switch (move bad) {
                case Result<i8, IntegerError>.Err { IntegerError reason = error }:
                    switch (reason) { case IntegerError.OutOfRange: return 0; break; }
                    break;
                case Result<i8, IntegerError>.Ok: return 2; break;
            }
            return 3;
        }
    )", "Result propagation and concrete numeric error identity");
    for (const auto& source : {
        "i32 main() { i8 wrong = 128i8; return 0; }",
        "i32 main() { i8 wrong = -129i8; return 0; }",
        "i32 main() { i32 wrong = 7i8; return 0; }",
        "i32 main() { f64 wrong = 7.0f32; return 0; }",
        "i32 main() { rsn wrong = Integer.convert<f32>(1i32); return 0; }",
        "i32 main() { rsn wrong = Float.convert<i32>(1.0f64); return 0; }"})
    {
        Fixture fixture(source); expect(!fixture.llvm && !fixture.diagnostics.empty(), "reject invalid suffix range or conversion domain");
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
