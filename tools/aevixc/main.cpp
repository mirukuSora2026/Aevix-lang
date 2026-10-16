// ========================
//
// Created By : miruku_sora2026
//
// File name : main.cpp
//
// Description :
//     구문·타입·소유권 검사와 AIR·LLVM 출력, AOT 빌드 및 호스트 실행을 제공한다.
//     Provide syntax, type, and ownership checks, AIR and LLVM output, AOT builds, and host execution.
//
// ========================

#include "aevix/Common/SourceManager.hpp"
#include "aevix/Frontend/Lexer/Lexer.hpp"
#include "aevix/Frontend/Modules/ModuleLoader.hpp"
#include "aevix/Frontend/Parser/Parser.hpp"
#include "aevix/Frontend/Sema/NameResolver.hpp"
#include "aevix/Frontend/Sema/ControlFlowChecker.hpp"
#include "aevix/Frontend/Sema/TypeChecker.hpp"
#include "aevix/Frontend/Sema/GenericResolver.hpp"
#include "aevix/Frontend/Sema/CoreErrorTypes.hpp"
#include "aevix/Frontend/Sema/ForeignValidator.hpp"
#include "aevix/Frontend/Ownership/BorrowChecker.hpp"
#include "aevix/IR/Lowering/ASTLowering.hpp"
#include "aevix/Backend/LLVM/LLVMCodeGen.hpp"
#include "aevix/Backend/AOT/AOTCompiler.hpp"
#include "aevix/JIT/JITEngine.hpp"
#include "aevix/Support/Process.hpp"

#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifndef AEVIX_DEFAULT_CLANG
#define AEVIX_DEFAULT_CLANG "clang"
#endif
#ifndef AEVIX_DEFAULT_RUNTIME
#define AEVIX_DEFAULT_RUNTIME ""
#endif
#ifndef AEVIX_INSTALL_RUNTIME_DIR
#define AEVIX_INSTALL_RUNTIME_DIR "lib/aevix"
#endif
#ifndef AEVIX_RUNTIME_FILENAME
#define AEVIX_RUNTIME_FILENAME "libaevix_runtime.a"
#endif
#ifndef AEVIX_VERSION
#define AEVIX_VERSION "0.1.0-dev"
#endif

namespace
{
    enum class Mode { Ast, Tokens, Check, AIR, LLVM, Build, Run, JIT, Starter };

    void usage(std::ostream& output)
    {
        output << "usage: aevixc [mode] [options] file.avx\n"
               << "  file.avx               Compile and execute without keeping build outputs (default)\n"
               << "  --with-starter         Save a native executable beside the source and a POSIX .sh launcher\n"
               << "  --tokens | --ast       Inspect syntax\n"
               << "  --check                Check types, control flow, moves, and borrows\n"
               << "  --emit-air | --emit-llvm  Print checked intermediate code\n"
               << "  --build | --run        Compile an executable or compile and run on the host\n"
               << "  --jit                  Execute checked code using the native ORC JIT\n"
               << "  --stdin                Read entry contents from stdin (preserves module path)\n"
               << "  --link FILE            Link an external object, archive, or shared library\n"
               << "  --load FILE            Load a shared library into the JIT session\n"
               << "  --runtime FILE         Runtime archive for an executable target\n"
               << "  --version              Print compiler version and JIT availability\n"
               << "  -o FILE                Build output (default: a.out, a.exe on Windows)\n"
               << "  -c                     Build an object file\n"
               << "  -O0 | -O1 | -O2 | -O3  LLVM optimization level\n"
               << "  --target TRIPLE        Apple ARM64, Linux ARM64/x86-64, Windows x86-64 target\n"
               << "  --clang FILE           LLVM clang executable\n"
               << "  --source-root DIR      Defaults to nearest src/ ancestor, then entry directory\n"
               << "  --module-root DIR      Additional unprefixed module root (repeatable)\n"
               << "  --package NAME=DIR     Package-prefixed root (repeatable)\n";
    }

    bool supportedTarget(const std::string& target)
    {
        return target.empty() || target == "aarch64-unknown-linux-gnu"
            || target == "x86_64-unknown-linux-gnu" || target == "arm64-apple-macosx"
            || target == "aarch64-apple-macosx" || target == "x86_64-pc-windows-msvc"
            || target == "x86_64-w64-windows-gnu";
    }

    std::string nativeTarget()
    {
#if defined(_WIN32)
#if defined(_MSC_VER)
        return "x86_64-pc-windows-msvc";
#else
        return "x86_64-w64-windows-gnu";
#endif
#elif defined(__aarch64__) || defined(__arm64__)
#if defined(__APPLE__)
        return "arm64-apple-macosx";
#else
        return "aarch64-unknown-linux-gnu";
#endif
#else
#if defined(__APPLE__)
        return "x86_64-apple-macosx";
#else
        return "x86_64-unknown-linux-gnu";
#endif
#endif
    }

    bool hostTarget(const std::string& target)
    {
        if (target.empty()) return true;
#if defined(_WIN32)
        const bool operatingSystem = target == nativeTarget();
#elif defined(__APPLE__)
        const bool operatingSystem = target.find("apple-macosx") != std::string::npos;
#else
        const bool operatingSystem = target.find("unknown-linux-gnu") != std::string::npos;
#endif
#if defined(__aarch64__) || defined(__arm64__)
        return operatingSystem && (target.starts_with("arm64-") || target.starts_with("aarch64-"));
#else
        return operatingSystem && target.starts_with("x86_64-");
#endif
    }

    // 쉘 명령 문자열 대신 인수 배열로 생성된 프로그램을 실행한다.
    // Launch generated programs with an argument vector, without a shell command.
    int runExecutable(const std::filesystem::path& file)
    {
        const auto process = aevix::runProcess({aevix::utf8Path(std::filesystem::absolute(file))});
        if (!process.started || !process.error.empty())
        { std::cerr << "error: " << process.error << '\n'; return 2; }
        return process.exitCode;
    }

    int jitWorker(int argc, char** argv)
    {
        if (argc < 1) return 2;
        aevix::JITOptions options;
        for (int index = 1; index < argc; ++index)
        {
            const std::string argument = argv[index];
            if (argument.size() == 3 && argument.starts_with("-O") && argument[2] >= '0' && argument[2] <= '3')
                options.optimization = static_cast<unsigned>(argument[2] - '0');
            else if (argument == "--load" && index + 1 < argc)
                options.sharedLibraries.emplace_back(aevix::pathFromUTF8(argv[++index]));
            else return 2;
        }
        std::ifstream input(aevix::pathFromUTF8(argv[0]), std::ios::binary);
        if (!input) { std::cerr << "error: cannot read JIT input\n"; return 2; }
        const std::string ir(std::istreambuf_iterator<char>(input), {});
        if (input.bad()) return 2;
        std::string error;
        auto engine = aevix::JITEngine::create(options, error);
        if (!engine || !engine->addModule(ir, error)) { std::cerr << "error: " << error << '\n'; return 1; }
        const auto invoked = engine->invokeI32();
        if (!invoked.succeeded) { std::cerr << "error: " << invoked.message << '\n'; return 1; }
        return invoked.value;
    }

    // JIT를 별도 프로세스에서 실행하여 trap과 정리 경계를 운영체제마다 동일하게 유지한다.
    // Run JIT in a separate process to preserve trap isolation and cleanup on each platform.
    int runJIT(const std::string& ir, const aevix::JITOptions& options)
    {
        if (!aevix::JITEngine::available())
        { std::cerr << "error: ORC JIT unavailable; build with LLVM 22 or newer\n"; return 2; }
        std::string error;
        auto temporary = aevix::TemporaryDirectory::create("aevix-jit-", error);
        if (!temporary) { std::cerr << "error: " << error << '\n'; return 2; }
        const auto executable = aevix::currentExecutable(error);
        if (executable.empty()) { std::cerr << "error: " << error << '\n'; return 2; }
        const auto input = temporary->path() / "module.ll";
        { std::ofstream file(input, std::ios::binary); file << ir; if (!file) return 2; }
        std::vector<std::string> arguments{aevix::utf8Path(executable), "--internal-jit-worker", aevix::utf8Path(input), "-O" + std::to_string(options.optimization)};
        for (const auto& library : options.sharedLibraries)
        { arguments.push_back("--load"); arguments.push_back(aevix::utf8Path(library)); }
        const auto process = aevix::runProcess(arguments);
        if (!process.started || !process.error.empty())
        { std::cerr << "error: " << process.error << '\n'; return 2; }
        return process.exitCode;
    }

    std::filesystem::path runtimeArchive()
    {
        std::string error;
        const auto executable = aevix::currentExecutable(error);
        if (!executable.empty())
        {
            const auto installed = executable.parent_path().parent_path() / AEVIX_INSTALL_RUNTIME_DIR / AEVIX_RUNTIME_FILENAME;
            std::error_code filesystemError;
            if (std::filesystem::is_regular_file(installed, filesystemError)) return installed;
        }
        return AEVIX_DEFAULT_RUNTIME;
    }

    std::string shellQuote(const std::string& text)
    {
        std::string result = "'";
        for (const auto character : text) result += character == '\'' ? "'\\''" : std::string(1, character);
        return result + "'";
    }

    bool publishStarter(const std::filesystem::path& binary, const std::filesystem::path& destination, bool windows)
    {
        std::error_code error;
        auto launcher = destination;
        launcher += ".sh";
        if (!std::filesystem::copy_file(binary, destination, std::filesystem::copy_options::none, error))
        { std::cerr << "error: cannot create executable: " << error.message() << '\n'; return false; }
        if (windows) return true;
        const auto staged = binary.parent_path() / "starter.sh";
        std::string fileName = aevix::utf8Path(launcher.filename());
        for (auto& character : fileName) if (static_cast<unsigned char>(character) < 32) character = '?';
        {
            std::ofstream file(staged, std::ios::binary);
            file << "#!/bin/sh\n# ========================\n#\n# Created By : miruku_sora2026\n#\n# File name : " << fileName
                 << "\n#\n# Description :\n#     같은 폴더의 Aevix 실행 파일을 실행한다.\n#     Run the Aevix executable in the same directory.\n#\n# ========================\nset -eu\n"
                 << "case $0 in\n  */*) AEVIX_STARTER_DIR=${0%/*} ;;\n  *) AEVIX_STARTER_DIR=. ;;\nesac\n"
                 << "exec \"$AEVIX_STARTER_DIR\"/" << shellQuote(aevix::utf8Path(destination.filename())) << " \"$@\"\n";
            if (!file) error = std::make_error_code(std::errc::io_error);
        }
        if (!error) std::filesystem::permissions(staged, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                               std::filesystem::perm_options::add, error);
        if (!error) std::filesystem::copy_file(staged, launcher, std::filesystem::copy_options::none, error);
        if (error)
        {
            std::cerr << "error: cannot create starter: " << error.message() << '\n';
            std::error_code ignored; std::filesystem::remove(destination, ignored);
            return false;
        }
        std::cout << "Starter " << aevix::utf8Path(launcher) << '\n';
        return true;
    }

    // 토큰과 AST 출력에는 이름 해석을 요구하지 않는다.
    // Token and AST inspection do not require name resolution.
    void dump(const aevix::Node& node, unsigned depth = 0)
    {
        std::cout << std::string(depth * 2, ' ') << node.kind;
        if (!node.value.empty()) std::cout << ' ' << std::quoted(node.value);
        std::cout << " [" << node.range.begin.offset << ',' << node.range.end.offset << ')';
        if (!node.comments.empty()) std::cout << " comments=" << node.comments.size();
        std::cout << '\n';
        for (const auto& child : node.children) dump(*child, depth + 1);
    }

    // 가져온 파일의 진단도 해당 파일의 경로와 바이트 기준 행·열로 출력한다.
    // Report imported-file diagnostics with their own paths and byte-based locations.
    void printDiagnostics(const aevix::SourceManager& sources, const aevix::Diagnostics& diagnostics)
    {
        for (const auto& diagnostic : diagnostics)
        {
            if (sources.contains(diagnostic.range))
            {
                const auto& file = sources.get(diagnostic.range.begin.source);
                const auto location = sources.locate(diagnostic.range.begin);
                std::cerr << file.path << ':' << location.line << ':' << location.column << ": ";
            }
            std::cerr << "error: " << diagnostic.message << '\n';
        }
    }
}

int compilerMain(int argc, char** argv)
{
    if (argc >= 2 && std::string(argv[1]) == "--internal-jit-worker") return jitWorker(argc - 2, argv + 2);
    Mode mode = Mode::Run;
    std::string path;
    aevix::ModuleOptions options;
    bool moduleOptions = false;
    bool backendOptions = false;
    bool stdinSource = false;
    bool clangSpecified = false;
    aevix::AOTOptions backend;
    aevix::JITOptions jit;
    backend.clang = AEVIX_DEFAULT_CLANG;
    if (backend.clang.is_absolute() && !std::filesystem::exists(backend.clang)) backend.clang = "clang";
    std::filesystem::path output;
    const auto invalidUsage = []
    {
        std::cerr << "invalid command line; use --help\n";
        return 2;
    };

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--help") { usage(std::cout); return 0; }
        if (argument == "--version")
        { std::cout << "Aevix " << AEVIX_VERSION << " (ORC JIT " << (aevix::JITEngine::available() ? "enabled" : "unavailable") << ")\n"; return 0; }
        if (argument == "--tokens") mode = Mode::Tokens;
        else if (argument == "--ast") mode = Mode::Ast;
        else if (argument == "--check") mode = Mode::Check;
        else if (argument == "--emit-air") mode = Mode::AIR;
        else if (argument == "--emit-llvm") mode = Mode::LLVM;
        else if (argument == "--build") mode = Mode::Build;
        else if (argument == "--with-starter") mode = Mode::Starter;
        else if (argument == "--run") mode = Mode::Run;
        else if (argument == "--jit") mode = Mode::JIT;
        else if (argument == "--stdin") stdinSource = true;
        else if (argument == "-c") { backend.objectOnly = true; backendOptions = true; }
        else if (argument.size() == 3 && argument.starts_with("-O") && argument[2] >= '0' && argument[2] <= '3')
        {
            backend.optimization = static_cast<unsigned>(argument[2] - '0');
            backendOptions = true;
        }
        else if (argument == "-o" || argument == "--target" || argument == "--clang"
                 || argument == "--link" || argument == "--load" || argument == "--runtime")
        {
            if (i + 1 >= argc) return invalidUsage();
            const std::string value = argv[++i];
            if (value.empty() || value.starts_with('-')) return invalidUsage();
            backendOptions = true;
            if (argument == "-o") output = aevix::pathFromUTF8(value);
            else if (argument == "--target") backend.targetTriple = value;
            else if (argument == "--link") backend.linkInputs.emplace_back(aevix::pathFromUTF8(value));
            else if (argument == "--load") jit.sharedLibraries.emplace_back(aevix::pathFromUTF8(value));
            else if (argument == "--runtime") backend.runtimeObjects.emplace_back(aevix::pathFromUTF8(value));
            else { backend.clang = aevix::pathFromUTF8(value); clangSpecified = true; }
        }
        else if (argument == "--source-root" || argument == "--module-root" || argument == "--package")
        {
            if (i + 1 >= argc) return invalidUsage();
            const std::string value = argv[++i];
            if (value.empty() || value.starts_with("--")) return invalidUsage();
            moduleOptions = true;
            if (argument == "--source-root")
            {
                if (!options.sourceRoot.empty()) return invalidUsage();
                options.sourceRoot = aevix::pathFromUTF8(value);
            }
            else if (argument == "--module-root") options.searchRoots.push_back({aevix::pathFromUTF8(value), ""});
            else
            {
                const auto separator = value.find('=');
                if (separator == std::string::npos || separator == 0 || separator + 1 == value.size())
                    return invalidUsage();
                options.searchRoots.push_back({aevix::pathFromUTF8(value.substr(separator + 1)), value.substr(0, separator)});
            }
        }
        else if (path.empty() && !argument.starts_with('-')) path = argument;
        else return invalidUsage();
    }

    if (path.empty()) { usage(std::cerr); return 2; }
    const bool syntax = mode == Mode::Ast || mode == Mode::Tokens;
    if (moduleOptions && syntax) return invalidUsage();
    if (stdinSource && syntax) return invalidUsage();
    if (backendOptions && (syntax || mode == Mode::Check || mode == Mode::AIR)) return invalidUsage();
    if (backend.objectOnly && mode != Mode::Build) return invalidUsage();
    if (!output.empty() && mode != Mode::Build) return invalidUsage();
    if (!backend.linkInputs.empty() && mode != Mode::Build && mode != Mode::Run && mode != Mode::Starter) return invalidUsage();
    if (!backend.runtimeObjects.empty() && mode != Mode::Build && mode != Mode::Run && mode != Mode::Starter) return invalidUsage();
    if (!jit.sharedLibraries.empty() && mode != Mode::JIT) return invalidUsage();
    if (mode == Mode::JIT && clangSpecified) return invalidUsage();
    if (backend.targetTriple.empty()) backend.targetTriple = nativeTarget();
    if (!supportedTarget(backend.targetTriple))
    {
        std::cerr << "error: supported targets are Apple Silicon macOS, ARM64/x86-64 Linux, and x86-64 Windows\n";
        return 2;
    }
    if ((mode == Mode::Run || mode == Mode::JIT) && !hostTarget(backend.targetTriple))
    {
        std::cerr << "error: --run requires a host-compatible target; use --build -c for cross-target objects\n";
        return 2;
    }

    aevix::SourceManager sources;
    aevix::Diagnostics diagnostics;
    if (!syntax)
    {
        if (stdinSource)
        {
            options.entrySource = std::string(std::istreambuf_iterator<char>(std::cin), {});
            if (std::cin.bad()) { std::cerr << "error: cannot read stdin\n"; return 2; }
        }
        auto graph = aevix::ModuleLoader(sources, diagnostics).load(aevix::pathFromUTF8(path), options);
        if (!graph)
        {
            printDiagnostics(sources, diagnostics);
            return 2;
        }
        if (diagnostics.empty()) aevix::installCoreErrorTypes(*graph, sources, diagnostics);
        aevix::NameResolution names;
        if (diagnostics.empty()) names = aevix::NameResolver(diagnostics).resolve(*graph);
        if (diagnostics.empty() && names.succeeded)
        {
            auto concrete = aevix::GenericResolver(diagnostics).resolve(*graph, names);
            if (concrete) names = std::move(*concrete);
        }
        aevix::TypeCheckResult types;
        if (diagnostics.empty() && names.succeeded)
            types = aevix::TypeChecker(diagnostics).check(*graph, names);
        if (diagnostics.empty() && types.succeeded)
            aevix::validateForeignDeclarations(names, types, diagnostics);
        bool flow = false;
        if (diagnostics.empty() && types.succeeded)
            flow = aevix::ControlFlowChecker(diagnostics).check(*graph, names, types);
        std::optional<aevix::AIRModule> air;
        if (diagnostics.empty() && flow)
            air = aevix::lowerToAIR(*graph, names, types, diagnostics);
        bool ownership = false;
        if (diagnostics.empty() && air)
            ownership = aevix::BorrowChecker(diagnostics).check(*air, names, types).succeeded;
        if (diagnostics.empty() && air && ownership)
            ownership = aevix::verifyAIR(*air, types.types, diagnostics);
        printDiagnostics(sources, diagnostics);
        if (!diagnostics.empty() || !names.succeeded || !types.succeeded || !flow || !air || !ownership) return 1;
        if (mode == Mode::Check)
        {
            const auto sourceModules = std::count_if(graph->modules.begin(), graph->modules.end(),
                [](const auto& module) { return module->name != "$core"; });
            std::cout << "Type checking succeeded: " << sourceModules
                      << " module(s), " << names.symbols.size() << " symbol(s); ownership checked\n";
            return 0;
        }
        if (mode == Mode::AIR) { std::cout << aevix::dumpAIR(*air, types.types); return 0; }
        auto llvm = aevix::emitLLVM(*air, types, names, diagnostics, backend.targetTriple);
        printDiagnostics(sources, diagnostics);
        if (!llvm || !diagnostics.empty()) return 1;
        if (mode == Mode::LLVM) { std::cout << *llvm; return 0; }
        if (air->entry == aevix::InvalidSymbolId && !backend.objectOnly)
        {
            std::cerr << "error: executable build requires a main function\n";
            return 1;
        }
        if (mode == Mode::JIT)
        {
            jit.optimization = backend.optimization;
            return runJIT(*llvm, jit);
        }
        const bool windowsTarget = backend.targetTriple.find("windows") != std::string::npos;
        std::optional<aevix::TemporaryDirectory> temporary;
        std::filesystem::path starterDestination;
        if (mode == Mode::Run || mode == Mode::Starter)
        {
            std::string error;
            temporary = aevix::TemporaryDirectory::create("aevix-run-", error);
            if (!temporary) { std::cerr << "error: " << error << '\n'; return 2; }
            backend.output = temporary->path() / (windowsTarget ? "program.exe" : "program");
            if (mode == Mode::Starter)
            {
                starterDestination = std::filesystem::absolute(aevix::pathFromUTF8(path));
                starterDestination.replace_extension(windowsTarget ? ".exe" : "");
                auto launcher = starterDestination;
                launcher += ".sh";
                for (const auto& candidate : {starterDestination, windowsTarget ? starterDestination : launcher})
                {
                    std::error_code filesystemError;
                    if (std::filesystem::exists(std::filesystem::symlink_status(candidate, filesystemError)))
                    { std::cerr << "error: starter output already exists: " << aevix::utf8Path(candidate) << '\n'; return 2; }
                }
            }
        }
        else backend.output = output.empty() ? (backend.objectOnly ? (windowsTarget ? "a.obj" : "a.o") : (windowsTarget ? "a.exe" : "a.out")) : output;
        bool needsRuntime = false;
        for (const auto& global : air->globals) needsRuntime = needsRuntime || global.isPersist;
        if (needsRuntime && !backend.objectOnly && backend.runtimeObjects.empty())
        {
            if (!hostTarget(backend.targetTriple))
            { std::cerr << "error: a cross-target executable requires --runtime and a matching linker/sysroot\n"; return 2; }
            backend.runtimeObjects.push_back(runtimeArchive());
        }
        const auto compiled = aevix::compileAOT(*llvm, backend);
        if (!compiled.succeeded)
        {
            std::cerr << "error: AOT compilation failed\n" << compiled.message;
            return 1;
        }
        if (mode == Mode::Run) return runExecutable(backend.output);
        if (mode == Mode::Starter)
        {
            if (!publishStarter(backend.output, starterDestination, windowsTarget)) return 2;
            backend.output = starterDestination;
        }
        std::cout << "Built " << aevix::utf8Path(backend.output) << '\n';
        return 0;
    }

    std::ifstream input(aevix::pathFromUTF8(path), std::ios::binary);
    if (!input) { std::cerr << "cannot open: " << path << '\n'; return 2; }
    std::string text((std::istreambuf_iterator<char>(input)), {});
    if (input.bad()) { std::cerr << "cannot read: " << path << '\n'; return 2; }
    const auto id = sources.add(path, std::move(text));
    auto tokens = aevix::Lexer(sources.get(id), diagnostics).tokenize();
    aevix::NodePtr module;
    if (diagnostics.empty() && mode == Mode::Ast)
        module = aevix::Parser(std::move(tokens), diagnostics).parseModule();
    printDiagnostics(sources, diagnostics);
    if (!diagnostics.empty()) return 1;
    if (mode == Mode::Tokens)
    {
        for (const auto& token : tokens)
            std::cout << token.range.begin.offset << ':' << token.range.end.offset
                      << ' ' << std::quoted(token.text) << '\n';
    }
    else if (module) dump(*module);
    return 0;
}

#ifdef _WIN32
// 넓은 Windows 명령행을 UTF-8로 바꿔 같은 드라이버로 전달한다.
// Convert the wide Windows command line to UTF-8 for the shared driver.
int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::vector<std::string> arguments;
    for (int index = 0; index < argc; ++index) arguments.push_back(aevix::utf8Path(std::filesystem::path(argv[index])));
    std::vector<char*> pointers;
    for (auto& argument : arguments) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    return compilerMain(argc, pointers.data());
}
#else
int main(int argc, char** argv) { return compilerMain(argc, argv); }
#endif
