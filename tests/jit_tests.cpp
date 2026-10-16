// ========================
//
// Created By : miruku_sora2026
//
// File name : jit_tests.cpp
//
// Description :
//     ORC 네이티브 실행, 세션 저장소, 초기화·정리, C 심벌 및 오류 경로를 검증한다.
//     Verify ORC native execution, session storage, initialization, cleanup, C symbols, and error paths.
//
// ========================

#include "aevix/JIT/JITEngine.hpp"
#include "ProcessTestSupport.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>


namespace
{
    unsigned checks = 0;
    unsigned failures = 0;
    std::atomic<unsigned> constructors = 0, destructors = 0, persistInitializers = 0, persistDrops = 0;

    void expect(bool success, std::string_view label, std::string_view detail = {})
    {
        ++checks;
        if (!success)
        {
            ++failures;
            std::cerr << "FAIL: " << label;
            if (!detail.empty()) std::cerr << ": " << detail;
            std::cerr << '\n';
        }
    }

    extern "C" std::int32_t hostAdd(std::int32_t value) { return value + 7; }
    extern "C" void reportEvent(std::int32_t event)
    {
        if (event == 1) ++constructors;
        if (event == 2) ++destructors;
        if (event == 10) ++persistInitializers;
        if (event == 20) ++persistDrops;
    }

    std::unique_ptr<aevix::JITEngine> create(const aevix::JITOptions& options = {})
    {
        std::string error;
        auto engine = aevix::JITEngine::create(options, error);
        expect(engine != nullptr && error.empty(), "create native session", error);
        return engine;
    }
    bool add(aevix::JITEngine& engine, std::string_view ir)
    {
        std::string error;
        const auto success = engine.addModule(ir, error);
        expect(success && error.empty(), "add verified LLVM module", error);
        return success;
    }
    void result(aevix::JITEngine& engine, std::string_view name, std::int32_t value, std::string_view label)
    {
        const auto invoked = engine.invokeI32(name);
        expect(invoked.succeeded && invoked.value == value && invoked.message.empty(), label, invoked.message);
    }

    void basic()
    {
        auto engine = create();
        if (!engine) return;
        expect(!engine->targetTriple().empty(), "native target is reported");
        if (!add(*engine, "define i32 @main() { ret i32 42 }")) return;
        result(*engine, "main", 42, "invoke native i32 main");
        result(*engine, "main", 42, "invoke same function repeatedly");
        expect(!engine->invokeI32("unknown").succeeded, "missing entry returns an error");
        expect(!engine->invokeVoid("main").succeeded, "invocation rejects return mismatch");
        std::string error;
        expect(!engine->addModule("define i32 @main() { ret i32 0 }", error)
            && error.find("already defined") != std::string::npos, "duplicate definition is rejected before linking");
        result(*engine, "main", 42, "rejected duplicate preserves original function");
    }

    void storage()
    {
        auto engine = create();
        if (!engine) return;
        if (!add(*engine, R"(
            @counter = global i32 0
            define i32 @next() {
                %old = load i32, ptr @counter
                %new = add i32 %old, 1
                store i32 %new, ptr @counter
                ret i32 %new
            }
        )")) return;
        result(*engine, "next", 1, "first invocation updates session global");
        result(*engine, "next", 2, "second invocation shares the same storage");
        if (!add(*engine, R"(
            @counter = external global i32
            declare i32 @next()
            define i32 @bridge() {
                %ignored = call i32 @next()
                %value = load i32, ptr @counter
                ret i32 %value
            }
        )")) return;
        result(*engine, "bridge", 3, "modules share one resolved global and function");
        result(*engine, "next", 4, "later calls preserve the linked global identity");
        auto separate = create();
        if (separate && add(*separate, "@counter = global i32 99\ndefine i32 @next() { %v = load i32, ptr @counter ret i32 %v }"))
            result(*separate, "next", 99, "separate engines own separate globals");
    }

    void errors()
    {
        auto engine = create();
        if (!engine) return;
        std::string error;
        expect(!engine->addModule("this is not LLVM IR", error) && !error.empty(), "malformed LLVM IR returns a parse diagnostic");
        expect(!engine->addModule("define i32 @bad() { entry: br label %entry }", error) && !error.empty(), "LLVM verification rejects invalid entry predecessors");
        const auto foreign = engine->targetTriple().find("aarch64") != std::string::npos
            || engine->targetTriple().find("arm64") != std::string::npos
            ? "x86_64-unknown-linux-gnu" : "aarch64-unknown-linux-gnu";
        expect(!engine->addModule(std::string("target triple = \"") + foreign + "\"\ndefine i32 @foreign() { ret i32 1 }", error)
            && error.find("running host") != std::string::npos, "foreign target is rejected");
        expect(!engine->addModule("target datalayout = \"e-p:32:32\"\ndefine i32 @layout() { ret i32 1 }", error)
            && error.find("data layout") != std::string::npos, "foreign layout is rejected");
        if (add(*engine, "define i64 @wide() { ret i64 1 }\ndefine i32 @arg(i32 %x) { ret i32 %x }\ndefine fastcc i32 @fast() { ret i32 1 }\ndefine void @empty() { ret void }"))
        {
            expect(!engine->invokeI32("wide").succeeded, "wide return cannot be called as i32");
            expect(!engine->invokeI32("arg").succeeded, "function arguments cannot be omitted");
            expect(!engine->invokeI32("fast").succeeded, "non-C calling convention cannot be invoked as C");
            expect(engine->invokeVoid("empty").succeeded, "void invocation checks its own signature");
        }
        expect(!engine->defineSymbol("", 1, error), "host symbol name cannot be empty");
        expect(!engine->defineSymbol("zero", 0, error), "host symbol address cannot be zero");
        aevix::JITOptions invalid;
        invalid.optimization = 4;
        expect(!aevix::JITEngine::create(invalid, error) && !error.empty(), "invalid optimization is rejected");
        aevix::JITOptions missing;
        missing.sharedLibraries.push_back("/aevix/missing/library/does-not-exist.so");
        expect(!aevix::JITEngine::create(missing, error) && !error.empty(), "missing shared library returns loader diagnostic");
    }

    void ffi()
    {
        aevix::JITOptions options;
        options.processSymbols = false;
        auto engine = create(options);
        if (!engine) return;
        std::string error;
        expect(engine->defineSymbol("host_add", reinterpret_cast<std::uintptr_t>(&hostAdd), error), "explicit host C ABI symbol is registered", error);
        if (add(*engine, "declare i32 @host_add(i32)\ndefine i32 @entry() { %v = call i32 @host_add(i32 5) ret i32 %v }"))
            result(*engine, "entry", 12, "JIT invokes registered native C function");
        auto process = create();
        if (process && add(*process, "declare i32 @abs(i32)\ndefine i32 @absolute() { %v = call i32 @abs(i32 -11) ret i32 %v }"))
            result(*process, "absolute", 11, "process C library symbol lookup");
        auto disabled = create(options);
        if (disabled && add(*disabled, "declare i32 @abs(i32)\ndefine i32 @absolute() { %v = call i32 @abs(i32 -11) ret i32 %v }"))
            expect(!disabled->invokeI32("absolute").succeeded, "disabled process lookup does not leak runtime process symbols");
        auto unresolved = create(options);
        if (unresolved && add(*unresolved, "declare i32 @aevix_symbol_that_does_not_exist()\ndefine i32 @entry() { %v = call i32 @aevix_symbol_that_does_not_exist() ret i32 %v }"))
        {
            const auto invoked = unresolved->invokeI32("entry");
            expect(!invoked.succeeded && !invoked.message.empty(), "unresolved extern returns a materialization error");
        }
    }

    void initialization()
    {
        constructors = 0; destructors = 0;
        {
            auto engine = create();
            if (!engine) return;
            std::string error;
            expect(engine->defineSymbol("report", reinterpret_cast<std::uintptr_t>(&reportEvent), error), "register lifecycle observer", error);
            if (!add(*engine, R"(
                @value = global i32 0
                @llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 65535, ptr @init, ptr null }]
                @llvm.global_dtors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 65535, ptr @finish, ptr null }]
                declare void @report(i32)
                define internal void @init() { store i32 9, ptr @value call void @report(i32 1) ret void }
                define internal void @finish() { call void @report(i32 2) ret void }
                define i32 @read() { %v = load i32, ptr @value ret i32 %v }
            )")) return;
            result(*engine, "read", 9, "module constructor runs before invocation");
            result(*engine, "read", 9, "repeated invocation preserves initialized storage");
            expect(constructors == 1 && destructors == 0, "module constructor runs exactly once");
        }
        expect(destructors == 1, "module destructor runs before session code unload");

        persistInitializers = 0; persistDrops = 0;
        {
            aevix::JITOptions options;
            options.processSymbols = false;
            auto engine = create(options);
            if (!engine) return;
            std::string error;
            expect(engine->defineSymbol("report", reinterpret_cast<std::uintptr_t>(&reportEvent), error), "register persist observer", error);
            if (!add(*engine, R"(
                @domain = internal global ptr null
                @guard = internal global ptr null
                @value = internal global i32 0
                @name = private constant [6 x i8] c"value\00"
                @llvm.global_dtors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 65535, ptr @finish, ptr null }]
                declare void @report(i32)
                declare void @aevix_persist_ensure(ptr, ptr, ptr, ptr, ptr)
                declare void @aevix_persist_shutdown(ptr)
                define internal void @init() { store i32 40, ptr @value call void @report(i32 10) ret void }
                define internal void @drop() { call void @report(i32 20) ret void }
                define internal void @finish() { call void @aevix_persist_shutdown(ptr @domain) ret void }
                define i32 @next() {
                    call void @aevix_persist_ensure(ptr @domain, ptr @guard, ptr @name, ptr @init, ptr @drop)
                    %old = load i32, ptr @value
                    %new = add i32 %old, 1
                    store i32 %new, ptr @value
                    ret i32 %new
                }
            )")) return;
            result(*engine, "next", 41, "persist runtime initializes through absolute host symbol");
            result(*engine, "next", 42, "persist storage is shared across JIT invocations");
            expect(persistInitializers == 1 && persistDrops == 0, "persist initializes once within a live session");
        }
        expect(persistDrops == 1, "persist cleanup runs before JIT session unload");
    }

    bool compileLibrary(const std::string& compiler, const std::filesystem::path& source,
                        const std::filesystem::path& output)
    {
        std::vector<std::string> arguments{compiler};
#if defined(__APPLE__)
        arguments.push_back("-dynamiclib");
#else
        arguments.push_back("-shared");
#endif
#if !defined(_WIN32)
        arguments.push_back("-fPIC");
#endif
        arguments.insert(arguments.end(), {aevix::utf8Path(source), "-o", aevix::utf8Path(output)});
        aevix::ProcessOptions options;
        options.timeout = std::chrono::seconds(30);
        return aevix::test::exitedWith(aevix::runProcess(arguments, options), 0);
    }

    void sharedLibrary(const std::string& compiler)
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() / ("aevix-jit-library-" + std::to_string(stamp));
        std::filesystem::create_directories(directory);
        const auto source = directory / "fixture.c";
#if defined(__APPLE__)
        const auto library = directory / "fixture.dylib";
#elif defined(_WIN32)
        const auto library = directory / "fixture.dll";
#else
        const auto library = directory / "fixture.so";
#endif
        {
            std::ofstream file(source);
            file << "#if defined(_WIN32)\n#define EXPORT __declspec(dllexport)\n#else\n#define EXPORT\n#endif\n"
                 << "EXPORT int aevix_jit_shared_value = 3; EXPORT int aevix_jit_shared_add(int n) { aevix_jit_shared_value += n; return aevix_jit_shared_value; }\n";
        }
        const auto built = compileLibrary(compiler, source, library);
        expect(built, "compile native C shared-library fixture");
        if (built)
        {
            aevix::JITOptions options;
            options.processSymbols = false;
            options.sharedLibraries.push_back(library);
            auto engine = create(options);
            if (engine && add(*engine, R"(
                @aevix_jit_shared_value = external global i32
                declare i32 @aevix_jit_shared_add(i32)
                define i32 @next() {
                    %ignored = call i32 @aevix_jit_shared_add(i32 2)
                    %value = load i32, ptr @aevix_jit_shared_value
                    ret i32 %value
                }
            )"))
            {
                result(*engine, "next", 5, "explicit shared library exports C function and global");
                result(*engine, "next", 7, "native C and JIT access the same storage");
            }
        }
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
}

int main(int argc, char** argv)
{
    if (!aevix::JITEngine::available())
    {
        std::string error;
        expect(!aevix::JITEngine::create({}, error), "LLVM-free build returns no JIT engine");
        expect(error.find("unavailable") != std::string::npos, "LLVM-free build explains unavailability");
    }
    else
    {
        basic(); storage(); errors(); ffi(); initialization();
        sharedLibrary(argc > 1 ? argv[1] : "clang");
    }
    std::cout << checks << " JIT checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
