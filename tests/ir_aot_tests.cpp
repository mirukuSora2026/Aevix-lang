// ========================
//
// Created By : miruku_sora2026
//
// File name : ir_aot_tests.cpp
//
// Description :
//     실제 AIR와 LLVM 객체·실행 파일을 생성하여 계산과 안전 정지를 검증한다.
//     Verify computation and safe stops by producing real AIR, LLVM objects, and executables.
//
// ========================
#include "aevix/Frontend/Sema/NameResolver.hpp"
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
    if (!std::filesystem::exists(clang)) { std::cout << "SKIP: LLVM clang unavailable\n"; return 77; }
    expect(Fixture("i32 main() { return 6 * 7; }").run(clang, 42), "integer arithmetic executable");
    expect(Fixture("i32 add(i32 x, i32 y = 4) { return x + y; } i32 main() { return add(y = 10, x = add(2)); }").run(clang, 16), "named arguments and defaults");
    expect(Fixture("i32 fact(i32 n) { if (n <= 1) return 1; return n * fact(n - 1); } i32 main() { return fact(5); }").run(clang, 120), "recursive call and branches");
    expect(Fixture("i32 main() { mut i32 sum = 0; for (mut i32 i = 0; i < 10; i++) { if (i == 3) continue; if (i == 8) break; sum += i; } return sum; }").run(clang, 25), "loop break and continue");
    expect(Fixture("i32 main() { mut i32 value = 4; while (value < 9) value++; return value; }").run(clang, 9), "while and increment");
    expect(Fixture("i32 main() { i32 values[3] = [3, 4, 5]; mut i32 sum = 0; for (i32 value : values) sum += value; return sum; }").run(clang, 12), "fixed array enhanced loop");
    expect(Fixture("i32 main() { mut i32 values[2][2] = [[1, 2], [3, 4]]; values[1][0] = 19; return values[1][0]; }").run(clang, 19), "nested array places");
    expect(Fixture("void inc(&mut i32 value) { (*value)++; return; } i32 main() { mut i32 x = 4; inc(&mut x); return x; }").run(clang, 5), "mutable reference call");
    expect(Fixture("struct Point { public i32 x; public i32 y; }; i32 main() { Point p = Point { y = 3, x = 8 }; return p.x + p.y; }").run(clang, 11), "struct field lowering");
    expect(Fixture("struct Counter { private i32 value; public Counter make(i32 value) { return Counter { value = value }; } public void inc(&mut Counter self) { (*self).value++; return; } public i32 get(&Counter self) { return (*self).value; } }; i32 main() { mut Counter c = Counter.make(8); c.inc(); return c.get(); }").run(clang, 9), "factory and automatic method borrowing");
    expect(Fixture("i32 main() { f32 x = 1.25; f32 y = -2.5; f64 z = 0.125; return x * y == -3.125 && z + 0.125 == 0.25 ? 3 : 9; }").run(clang, 3), "float32 and float64 computations");
    expect(Fixture("bool bad() { i32 z = 0; return 2 / z == 0; } i32 main() { bool a = false && bad(); bool b = true || bad(); return a || !b ? 9 : 7; }").run(clang, 7), "logical short circuit");
    expect(Fixture("i32 main() { i32 x = 0; return true ? 7 : 9 / x; }").run(clang, 7), "conditional skips inactive branch");
    expect(Fixture("i32 main() { mut i32 out = 0; switch (3) { case 2: out = 5; break; case 3: out = 8; break; default: out = 9; break; } return out; }").run(clang, 8), "scalar switch dispatch");
    expect(Fixture("const i32 BASE = 3; const i32 VALUE = BASE * 4; i32 main() { return VALUE; }").run(clang, 12), "folded global constants");
    expect(Fixture("i32 main() { i8 value = -128; return value == -128 ? 1 : 9; }").run(clang, 1), "signed minimum literal");
    expect(Fixture("i32 main() { i8 value = -(128); return value == -128 ? 1 : 9; }").run(clang, 1), "parenthesized signed minimum literal");
    expect(Fixture("i32 main() { char x = '가'; return x == '가' ? 1 : 9; }").run(clang, 1), "Unicode scalar literal");
    expect(Fixture("enum Event { Quit; Move { public i32 x; public i32 y; }; }; i32 main() { Event e = Event.Move { y = 4, x = 7 }; mut i32 out = 0; switch (e) { case Event.Move { i32 value = x }: out = value; break; case Event.Quit: out = 1; break; } return out; }").run(clang, 7), "enum tagged union and copied payload");
    expect(Fixture("enum Event { Quit; Move { public i32 x; }; }; i32 main() { mut Event e = Event.Move { x = 4 }; switch (&mut e) { case Event.Move { &mut i32 value = x }: (*value)++; break; case Event.Quit: break; } mut i32 out = 0; switch (e) { case Event.Move { i32 value = x }: out = value; break; case Event.Quit: break; } return out; }").run(clang, 5), "enum mutable payload loan");
    expect(Fixture("struct Owner { public i32 value; }; enum Event { None; Own { public Owner data; }; }; i32 main() { Event e = Event.Own { data = Owner { value = 12 } }; mut i32 out = 0; switch (move e) { case Event.Own { Owner payload = data }: out = payload.value; break; case Event.None: break; } return out; }").run(clang, 12), "consuming enum payload ownership");
    expect(Fixture("enum Value { Empty; Number { public f64 value; }; }; i32 main() { Value v = Value.Number { value = 1.25 }; mut i32 out = 1; switch (v) { case Value.Number { f64 x = value }: out = x == 1.25 ? 9 : 2; break; default: out = 3; break; } return out; }").run(clang, 9), "enum aligned floating payload union");
    expect(Fixture("enum Color implements Copy { Red; Blue; }; i32 main() { Color a = Color.Blue; Color b = a; mut i32 out = 0; switch (b) { case Color.Red: out = 1; break; case Color.Blue: out = 2; break; } return out; }").run(clang, 2), "Copy enum without payload");
    expect(Fixture("T identity<T>(T value) { return value; } i32 main() { return identity<i32>(7) + identity(9); }").run(clang, 16), "generic function specialization execution");
    expect(Fixture("struct Pair<T> { public T first; public T second; }; i32 main() { Pair<i32> pair = Pair<i32> { first = 2, second = 3 }; return pair.first + pair.second; }").run(clang, 5), "generic struct execution");
    expect(Fixture("enum Outcome<T> { Value { public T item; }; Empty; }; i32 main() { Outcome<i32> value = Outcome<i32>.Value { item = 7 }; switch (value) { case Outcome<i32>.Value { i32 item = item }: return item; break; case Outcome<i32>.Empty: return 0; break; } }").run(clang, 7), "generic enum execution");
    expect(Fixture("interface Named { public i32 get(&Self self); public bool active(&Self self) { return self.get() > 0; } }; struct Item implements Named { public i32 get(&Item self) { return 7; } }; i32 main() { Item item = Item {}; return item.active() ? 8 : 1; }").run(clang, 8), "static interface default method execution");
    expect(Fixture("interface Left { public i32 get(&Self self) { return 1; } }; interface Right { public i32 get(&Self self) { return 2; } }; struct Item implements Left, Right { public override i32 get(&Item self) { return 7; } }; i32 main() { Item item = Item {}; return item.get(); }").run(clang, 7), "interface override execution");
    expect(Fixture("const f64 INF = 1.0 / 0.0; const f64 NAN = 0.0 / 0.0; i32 main() { f64 zero = 0.0; f64 runtime_inf = 1.0 / zero; f64 runtime_nan = zero / zero; return INF == runtime_inf && NAN != NAN && runtime_nan != runtime_nan ? 7 : 9; }").run(clang, 7), "constant and runtime IEEE infinities and NaNs");
    expect(Fixture("const i8 SHIFTED = 64 << 1; i32 main() { i8 value = 64; i8 shifted = value << 1; return shifted == SHIFTED ? 7 : 9; }").run(clang, 7), "constant and runtime fixed-width shifts");
    {
        Fixture borrowedGlobal("i32 value = 1; &i32 reference = &value; i32 main() { return *reference; }");
        bool rejected = false;
        for (const auto& diagnostic : borrowedGlobal.diagnostics) rejected = rejected || diagnostic.message.find("reference-containing global storage") != std::string::npos;
        expect(!borrowedGlobal.air && rejected, "global reference storage requires initialization AIR");
    }
    {
        Fixture neverCall("never stop() { while (true) {} } i32 main() { stop(); return 0; }");
        expect(neverCall.types.succeeded && neverCall.air && neverCall.llvm
            && neverCall.llvm->find("noreturn") != std::string::npos,
            "never-returning calls lower to explicit unreachable control flow");
    }
    expect(Fixture("type Id = i32; T identity<T>(T value) where T implements Copy { return value; } i32 main() { Id value = 42; return identity(value); }").run(clang, 42), "transparent numeric aliases participate in generic inference");
    expect(Fixture("struct Point implements Copy { public i32 x; }; type Alias = Point; i32 main() { Alias value = Alias { x = 42 }; return value.x; }").run(clang, 42), "transparent nominal alias initializer executes");
    expect(Fixture("struct Pair<T> { public T first; public T second; }; type Alias = Pair<i32>; i32 main() { Alias a = Alias { first = 20, second = 22 }; return a.first + a.second; }").run(clang, 42), "alias of generic specialization initializes canonical storage");
    expect(Fixture("never stop() { Never.unreachable(); } i32 pick(bool alive) { return alive ? 42 : stop(); } i32 left(bool alive) { return alive ? stop() : 42; } i32 main() { return pick(true) + left(false) == 84 ? 42 : 1; }").run(clang, 42), "never branches coerce in both conditional positions");
    expect(Fixture("never stop() { Never.unreachable(); } i32 main() { bool a = false && stop(); bool b = true || stop(); return !a && b ? 42 : 1; }").run(clang, 42), "short circuit does not execute never branches");
    {
        Fixture both("never stop() { Never.unreachable(); } never choose(bool left) { return left ? stop() : stop(); } i32 main() { return 42; }");
        expect(both.llvm && both.llvm->find("alloca void") == std::string::npos,
            "two divergent conditional alternatives require no impossible value storage");
        Fixture storage("i32 main() { never value; return 0; }");
        expect(!storage.types.succeeded && !storage.diagnostics.empty(), "never cannot be stored as a local value");
        Fixture inferred("i32 main() { rsn value = Never.unreachable(); return 0; }");
        expect(!inferred.types.succeeded && !inferred.diagnostics.empty(), "inferred never storage is rejected");
        Fixture fallsThrough("never stop() {} i32 main() { return 0; }");
        expect(!fallsThrough.air && !fallsThrough.diagnostics.empty(), "never functions cannot fall through");
    }
    for (const auto level : {0u, 2u})
    {
        expect(Fixture("never stop() { Never.unreachable(); } i32 main() { stop(); return 42; }").run(clang, 0, true, level), "never function safely traps at O" + std::to_string(level));
        expect(Fixture("never stop() { Never.unreachable(); } i32 main() { i32 value = 1 + stop(); return value; }").run(clang, 0, true, level), "nested never arithmetic safely traps at O" + std::to_string(level));
    }
    const std::vector<std::pair<std::string, std::string>> traps{
        {"signed addition overflow", "i32 main() { i32 a = 2147483647; return a + 1; }"},
        {"unsigned subtraction overflow", "i32 main() { u8 a = 0; u8 b = a - 1; return b == 0 ? 0 : 1; }"},
        {"multiplication overflow", "i32 main() { i32 a = 65536; return a * a; }"},
        {"division zero", "i32 main() { i32 a = 0; return 5 / a; }"},
        {"division signed minimum", "i32 main() { i32 a = -2147483648; return a / -1; }"},
        {"remainder signed minimum", "i32 main() { i32 a = -2147483648; return a % -1; }"},
        {"negative shift", "i32 main() { i32 a = -1; return 1 << a; }"},
        {"oversized shift", "i32 main() { i32 a = 32; return 1 >> a; }"},
        {"array bound", "i32 main() { i32 a[2] = [1, 2]; usize i = 2; return a[i]; }"},
        {"negative array index", "i32 main() { i32 a[2] = [1, 2]; i32 i = -1; return a[i]; }"},
        {"unary negation overflow", "i32 main() { i32 a = -2147483648; return -a; }"}
    };
    for (const auto& [name, source] : traps)
        for (const auto level : {0u, 2u}) expect(Fixture(source).run(clang, 0, true, level), name + " O" + std::to_string(level));
    {
        Fixture fixture("i32 main() { return 42; }");
        expect(fixture.air && aevix::dumpAIR(*fixture.air, fixture.types.types).find("return %") != std::string::npos, "typed AIR dump");
        if (fixture.air)
        {
            auto malformed = *fixture.air;
            malformed.functions[0].blocks[0].terminator.value = 999999;
            aevix::Diagnostics invalid;
            expect(!aevix::verifyAIR(malformed, fixture.types.types, invalid) && !invalid.empty(), "AIR verifier rejects invalid return value");
            malformed = *fixture.air;
            malformed.functions[0].blocks[0].instructions.insert(malformed.functions[0].blocks[0].instructions.begin(), malformed.functions[0].blocks[0].instructions.back());
            invalid.clear();
            expect(!aevix::verifyAIR(malformed, fixture.types.types, invalid), "AIR verifier rejects duplicate value definition");
        }
        if (fixture.llvm)
        {
            aevix::AOTOptions options;
            options.clang = clang; options.output = fixture.directory / "x86.o"; options.objectOnly = true; options.targetTriple = "x86_64-unknown-linux-gnu";
            expect(aevix::compileAOT(*fixture.llvm, options).succeeded && std::filesystem::file_size(options.output) > 0, "cross emit x86-64 Linux object only");
        }
    }
    std::cout << checks << " AIR/AOT checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
