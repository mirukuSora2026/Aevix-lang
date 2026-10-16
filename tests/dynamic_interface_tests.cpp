// ========================
//
// Created By : miruku_sora2026
//
// File name : dynamic_interface_tests.cpp
//
// Description :
//     동적 인터페이스 대여, 고유 소유, 할당 실패 복구와 실제 자원 정리를 검증한다.
//     Verify dynamic interface loans, unique owners, allocation failure recovery, and real cleanup.
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
    void expect(bool condition, const std::string& name)
    {
        ++checks;
        if (!condition) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
    }
    std::size_t allocationCalls = 0, freeCalls = 0, failAllocation = 0, liveAllocations = 0;
    extern "C" void* trackedAllocate(std::uint64_t size)
    {
        ++allocationCalls;
        if (failAllocation && allocationCalls == failAllocation) return nullptr;
        auto* value = std::malloc(static_cast<std::size_t>(size));
        if (value) ++liveAllocations;
        return value;
    }
    extern "C" void trackedFree(void* value)
    {
        if (value) { ++freeCalls; --liveAllocations; std::free(value); }
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
            if (air && aevix::BorrowChecker(diagnostics).check(*air, names, types).succeeded
                && aevix::verifyAIR(*air, types.types, diagnostics))
                llvm = aevix::emitLLVM(*air, types, names, diagnostics);
        }
        bool contains(std::string_view text) const
        {
            for (const auto& item : diagnostics) if (item.message.find(text) != std::string::npos) return true;
            return false;
        }
        bool jit(int expected, std::size_t failAt = 0, std::size_t expectedCalls = 0, std::size_t expectedFrees = 0)
        {
            if (!llvm) { for (const auto& item : diagnostics) std::cerr << item.message << '\n'; return false; }
            allocationCalls = 0; freeCalls = 0; failAllocation = failAt; liveAllocations = 0;
            std::string error;
            auto engine = aevix::JITEngine::create({}, error);
            if (!engine || !engine->defineSymbol("malloc", reinterpret_cast<std::uintptr_t>(&trackedAllocate), error)
                || !engine->defineSymbol("free", reinterpret_cast<std::uintptr_t>(&trackedFree), error)
                || !engine->addModule(*llvm, error)) { std::cerr << error << '\n'; return false; }
            const auto result = engine->invokeI32();
            if (!result.succeeded) std::cerr << result.message << '\n';
            return result.succeeded && result.value == expected && allocationCalls == expectedCalls && freeCalls == expectedFrees && liveAllocations == 0;
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
    const std::string valueContract = "interface Value { public i32 value(&Self self); public i32 doubled(&Self self) { return self.value() * 2; } }; struct Number implements Value { public i32 amount; public i32 value(&Number self) { return self.amount; } }; ";
    const auto borrowed = valueContract + "i32 read(&dyn Value item) { return item.doubled(); } i32 main() { Number number = Number { amount = 21 }; &dyn Value view = &number; return read(view); }";
    expect(Fixture(borrowed).run(clang, 42), "AOT borrowed dynamic default dispatch");
    if (aevix::JITEngine::available()) expect(Fixture(borrowed).jit(42), "JIT borrowed dynamic default dispatch without allocation");
    const auto twoTypes = valueContract + "struct Other implements Value { public i32 value(&Other self) { return 40; } }; i32 read(&dyn Value item) { return item.value(); } i32 main() { Number number = Number { amount = 2 }; Other other = Other {}; return read(&number) + read(&other); }";
    expect(Fixture(twoTypes).run(clang, 42), "one dyn parameter dispatches two concrete layouts");
    const std::string counter = "interface Counter { public void increment(&mut Self self); public i32 value(&Self self); }; struct Number implements Counter { public i32 amount; public void increment(&mut Number self) { self.amount++; return; } public i32 value(&Number self) { return self.amount; } }; ";
    expect(Fixture(counter + "i32 main() { mut Number number = Number { amount = 41 }; &mut dyn Counter view = &mut number; view.increment(); return view.value(); }").run(clang, 42), "exclusive dyn supports mutation and shared reborrow");
    {
        Fixture test(counter + "i32 main() { Number number = Number { amount = 41 }; &dyn Counter view = &number; view.increment(); return 0; }");
        expect(!test.llvm && test.contains("No matching overload"), "shared dyn cannot call exclusive method");
    }
    {
        Fixture test(valueContract + "&dyn Value bad() { Number number = Number { amount = 42 }; return &number; } i32 main() { return 0; }");
        expect(!test.llvm && test.contains("local"), "borrowed dyn cannot escape local owner");
    }
    {
        Fixture test(counter + "i32 main() { mut Number number = Number { amount = 1 }; &dyn Counter view = &number; number.amount = 2; return view.value(); }");
        expect(!test.llvm && test.contains("borrowed"), "live dyn loan prevents owner mutation");
    }
    {
        Fixture test("interface Value { public i32 value(&Self self); }; struct Number { public i32 value(&Number self) { return 42; } }; i32 main() { Number number = Number {}; &dyn Value view = &number; return view.value(); }");
        expect(!test.llvm, "structural resemblance does not replace implements contract");
    }
    const std::string concreteOnly = "interface Maker { public i32 value(&Self self); public Self duplicate(&Self self); }; struct Number implements Maker { public i32 value(&Number self) { return 42; } public Number duplicate(&Number self) { return Number {}; } }; ";
    expect(Fixture(concreteOnly + "i32 main() { Number number = Number {}; &dyn Maker view = &number; return view.value(); }").run(clang, 42), "view may expose interface with concrete-only Self return");
    {
        Fixture test(concreteOnly + "i32 main() { Number number = Number {}; &dyn Maker view = &number; rsn copy = view.duplicate(); return 0; }");
        expect(!test.llvm && test.contains("concrete Self"), "concrete-only call through dyn is rejected");
    }
    {
        Fixture test("interface Hidden { i32 value(&Self self); }; struct Number implements Hidden { public i32 value(&Number self) { return 42; } }; i32 main() { Number number = Number {}; &dyn Hidden view = &number; return view.value(); }");
        expect(!test.llvm && test.contains("private member"), "dynamic calls retain contract method visibility");
    }
    const auto boxed = "import memory { Box, InputError }; " + valueContract + "Result<i32, InputError<Number>> run() { Box<Number> concrete = Box.new(Number { amount = 21 })?; Box<dyn Value> erased = Box.into_dyn(concrete); return Result.ok(erased.doubled()); } i32 main() { Result<i32, InputError<Number>> result = run(); switch (result) { case Result<i32, InputError<Number>>.Ok { i32 output = value }: return output; break; case Result<i32, InputError<Number>>.Err: return 1; break; } }";
    expect(Fixture(boxed).run(clang, 42), "AOT owned dyn creation conversion dispatch and cleanup");
    if (aevix::JITEngine::available()) expect(Fixture(boxed).jit(42, 0, 1, 1), "owned dyn conversion performs no extra allocation and frees exactly once");
    const auto boxedMutation = "import memory { Box }; " + counter + "Result<i32, InputError<Number>> run() { mut Box<Number> concrete = Box.new(Number { amount = 41 })?; mut Box<dyn Counter> erased = Box.into_dyn(concrete); erased.increment(); &dyn Counter view = Box.as_ref(&erased); return Result.ok(view.value()); } i32 main() { rsn result = run(); switch (result) { case Result<i32, InputError<Number>>.Ok { i32 output = value }: return output; break; case Result<i32, InputError<Number>>.Err: return 1; break; } }";
    expect(Fixture(boxedMutation).run(clang, 42), "owned dyn mutable auto borrowing and explicit shared view");
    const auto scalarOwner = "import memory { Box }; i32 main() { Result<Box<i32>, InputError<i32>> result = Box.new(42); switch (move result) { case Result<Box<i32>, InputError<i32>>.Ok { Box<i32> owner = value }: &i32 view = Box.as_ref(&owner); return *view; break; case Result<Box<i32>, InputError<i32>>.Err { InputError<i32> error = error }: switch (move error) { case InputError<i32>.AllocationFailure { i32 input = input }: return input; break; case InputError<i32>.SizeOverflow { i32 input = input }: return input; break; } break; } }";
    expect(Fixture(scalarOwner).run(clang, 42), "Box concrete payload view");
    if (aevix::JITEngine::available())
    {
        expect(Fixture(scalarOwner).jit(42, 1, 1, 0), "allocation failure preserves original scalar input in InputError");
        expect(Fixture(scalarOwner).jit(42, 0, 1, 1), "success drops scalar Box payload exactly once");
    }
    {
        Fixture test("i32 main() { rsn result = Box.new(42); return 0; }");
        expect(!test.llvm && test.contains("Unknown name 'Box'"), "Box requires explicit import");
    }
    {
        Fixture test("import memory { Box }; i32 main() { Box<i32> owner = Box.new(42); return 0; }");
        expect(!test.llvm && test.contains("Variable initializer"), "fallible Box.new cannot be used without Result handling");
    }
    {
        Fixture test("import memory { Box }; i32 main() { Result<Box<i32>,InputError<i32>> value = Box.new(42); rsn duplicate = value; rsn again = value; return 0; }");
        expect(!test.llvm && test.contains("moved"), "Box carrier ownership is move-only");
    }
    const auto nested = "import memory { Box }; Result<i32,InputError<i32>> run() { Box<i32> inner = Box.new(42)?; Result<Box<Box<i32>>,InputError<Box<i32>>> outer = Box.new(inner); switch (move outer) { case Result<Box<Box<i32>>,InputError<Box<i32>>>.Ok { Box<Box<i32>> owner = value }: return Result.ok(42); break; case Result<Box<Box<i32>>,InputError<Box<i32>>>.Err { InputError<Box<i32>> failure = error }: switch (move failure) { case InputError<Box<i32>>.AllocationFailure { Box<i32> recovered = input }: return Result.ok(*Box.as_ref(&recovered)); break; case InputError<Box<i32>>.SizeOverflow { Box<i32> recovered = input }: return Result.ok(*Box.as_ref(&recovered)); break; } break; } } i32 main() { rsn result = run(); switch (result) { case Result<i32,InputError<i32>>.Ok { i32 value = value }: return value; break; case Result<i32,InputError<i32>>.Err: return 1; break; } }";
    expect(Fixture(nested).run(clang,42), "nested owners recursively clean payloads");
    if (aevix::JITEngine::available())
    {
        expect(Fixture(nested).jit(42,0,2,2), "nested Box frees both payload allocations once");
        expect(Fixture(nested).jit(42,2,2,1), "failed outer allocation returns and frees inner owner once");
    }
    const auto earlyExit = "import memory { Box }; i32 consume(Box<i32> owner, i32 count) { return count; } Result<i32,InputError<i32>> fail() { return Result.err(InputError<i32>.AllocationFailure { input = 7 }); } Result<i32,InputError<i32>> run() { Box<i32> owner = Box.new(42)?; i32 result = consume(owner, fail()?); return Result.ok(result); } i32 main() { rsn result = run(); return Result.is_err(&result) ? 42 : 1; }";
    expect(Fixture(earlyExit).run(clang,42), "failure during a later argument cleans prepared Box owner");
    if (aevix::JITEngine::available()) expect(Fixture(earlyExit).jit(42,0,1,1), "early argument propagation frees Box exactly once");
    {
        Fixture test("import memory { Box }; Result<&i32,InputError<i32>> bad() { Box<i32> owner = Box.new(42)?; return Result.ok(Box.as_ref(&owner)); } i32 main() { return 0; }");
        expect(!test.llvm && test.contains("local"), "Box payload view cannot outlive owning local");
    }
    const auto explicitBox = "import memory { Box }; i32 main() { Result<Box<i8>,InputError<i8>> result = Box.new<i8>(value = 42); switch (move result) { case Result<Box<i8>,InputError<i8>>.Ok { Box<i8> owner = value }: &i8 view = Box.as_ref(owner = &owner); return *view == 42 ? 42 : 1; break; case Result<Box<i8>,InputError<i8>>.Err: return 1; break; } }";
    expect(Fixture(explicitBox).run(clang,42), "explicit Box payload type and named intrinsic argument");
    {
        Fixture test("import memory { Box }; i32 main() { rsn owner = Box.new(unknown = 42); return 0; }");
        expect(!test.llvm && test.contains("argument name"), "Box intrinsics reject unknown named argument");
    }
    expect(Fixture("interface Value<T> { public T value(&Self self); }; struct Number implements Value<i32> { public i32 value(&Number self) { return 42; } }; i32 main() { Number number = Number {}; &dyn Value<i32> view = &number; return view.value(); }").run(clang,42), "closed generic interface view dispatch");
    const auto mutableBox = "import memory { Box }; Result<i32,InputError<i32>> run() { mut Box<i32> owner = Box.new(41)?; &mut i32 view = Box.as_mut(&mut owner); (*view)++; return Result.ok(*view); } i32 main() { rsn result = run(); switch (result) { case Result<i32,InputError<i32>>.Ok { i32 value = value }: return value; break; case Result<i32,InputError<i32>>.Err: return 1; break; } }";
    expect(Fixture(mutableBox).run(clang,42), "explicit exclusive Box payload view");
    const auto returnedReference = valueContract + "&Number identity(&Number value) { return value; } i32 read(&dyn Value item) { return item.value(); } i32 main() { Number number = Number { amount = 21 }; &Number borrowed = identity(&number); &dyn Value view = identity(&number); return read(borrowed) + read(view); }";
    expect(Fixture(returnedReference).run(clang,42), "returned and stored concrete references coerce to dyn with correct ABI");
    if (aevix::JITEngine::available()) expect(Fixture(returnedReference).jit(42), "JIT reference coercion creates views without allocations");
    const auto referencePack = "&i32 first(&i32 values[]) { return values[0]; } i32 main() { i32 input = 42; &i32 result = first(&input); return *result; }";
    expect(Fixture(referencePack).run(clang,42), "variadic elements preserve external reference provenance");
    if (aevix::JITEngine::available()) expect(Fixture(referencePack).jit(42), "reference pack creates no heap allocation");
    {
        Fixture test("Slice<i32> escape(i32 values[]) { return values; } i32 main() { return 0; }");
        expect(!test.llvm && test.contains("variadic call-frame"), "uncalled variadic storage escape is rejected at declaration");
    }
    {
        Fixture test("&i32 first(&i32 values[]) { return values[0]; } &i32 bad() { i32 input = 42; return first(&input); } i32 main() { return 0; }");
        expect(!test.llvm && test.contains("local"), "returned pack element reference cannot outlive its external owner");
    }
    {
        Fixture test("&i32 first(&i32 values[]) { return values[0]; } i32 main() { mut i32 input = 42; &i32 result = first(&input); input++; return *result; }");
        expect(!test.llvm && test.contains("borrowed"), "reference returned from pack prevents conflicting owner modification");
    }
    const auto owningPack = "import memory { Box }; usize count(Box<i32> values[]) { return Slice.len(values); } Result<i32,InputError<i32>> run() { Box<i32> first = Box.new(1)?; Box<i32> second = Box.new(2)?; usize length = count(first,second); return Result.ok(length == 2 ? 42 : 1); } i32 main() { rsn result = run(); return Result.is_ok(&result) ? 42 : 1; }";
    expect(Fixture(owningPack).run(clang,42), "variadic call-frame owns moved Box elements");
    if (aevix::JITEngine::available()) expect(Fixture(owningPack).jit(42,0,2,2), "owning variadic pack adds no allocation and drops both elements once");
    const auto boxVector = "import memory { Box }; Result<i32,CollectionError> run() { Vector<i32> input = [1,2]?; Result<Box<Vector<i32>>,InputError<Vector<i32>>> result = Box.new(input); return Result.ok(Result.is_ok(&result) ? 42 : 1); } i32 main() { rsn result = run(); return Result.is_ok(&result) ? 42 : 1; }";
    expect(Fixture(boxVector).run(clang,42), "Box owns Vector payload with unified recursive cleanup");
    if (aevix::JITEngine::available()) expect(Fixture(boxVector).jit(42,0,2,2), "Box Vector nesting frees both allocations exactly once");
    const auto vectorBox = "import memory { Box }; Result<i32,InputError<i32>> run() { Box<i32> first = Box.new(1)?; Box<i32> second = Box.new(2)?; Result<Vector<Box<i32>>,CollectionError> result = [first,second]; return Result.ok(Result.is_ok(&result) ? 42 : 1); } i32 main() { rsn result = run(); return Result.is_ok(&result) ? 42 : 1; }";
    expect(Fixture(vectorBox).run(clang,42), "Vector owns Box elements with unified recursive cleanup");
    if (aevix::JITEngine::available()) expect(Fixture(vectorBox).jit(42,0,3,3), "Vector Box nesting frees all three allocations exactly once");
    expect(Fixture("usize count(i32 values[]) { return Slice.len(values); } i32 main() { return count() == 0 && count(1,2,3) == 3 ? 42 : 1; }").run(clang,42), "scalar pack length drops storage provenance for empty and nonempty calls");
    std::cout << checks << " dynamic interface checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
