// ========================
//
// Created By : miruku_sora2026
//
// File name : AOTCompiler.cpp
//
// Description :
//     임시 IR 파일과 인수 배열을 사용하여 clang을 안전하게 실행한다.
//     Invoke clang safely using temporary IR files and an argument vector.
//
// ========================
#include "aevix/Backend/AOT/AOTCompiler.hpp"
#include "aevix/Support/Process.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <string_view>
#include <vector>

namespace aevix
{
    namespace
    {
        std::string toolchainHint(std::string target)
        {
            if (target.empty())
            {
#ifdef __APPLE__
                target = "apple";
#elif defined(_WIN32)
#ifdef _MSC_VER
                target = "windows-msvc";
#else
                target = "windows-gnu";
#endif
#else
                target = "linux";
#endif
            }
            if (target.find("apple") != std::string::npos)
                return "Install Xcode Command Line Tools (xcode-select --install) and a compatible LLVM/Clang driver.";
            if (target.find("windows") != std::string::npos)
                return target.find("gnu") != std::string::npos
                    ? "Install LLVM/Clang and a matching MinGW-w64 linker and runtime."
                    : "Install LLVM/Clang and MSVC Build Tools with the Windows SDK; use a developer command prompt.";
            return "Install LLVM/Clang, the target linker (GNU binutils or LLVM lld), libc development files, and a matching sysroot.";
        }

        // 실제 드라이버 진단에서 확인된 누락만 경고하며 일반 미정의 심볼 오류와 구별한다.
        // Warn about omissions identified by the real driver, separately from ordinary undefined symbols.
        void diagnoseToolchainFailure(AOTResult& result, const AOTOptions& options)
        {
            std::string message = result.message;
            std::transform(message.begin(), message.end(), message.begin(),
                [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            const auto containsAny = [&](std::initializer_list<const char*> patterns)
            {
                return std::any_of(patterns.begin(), patterns.end(),
                    [&](const char* pattern) { return message.find(pattern) != std::string::npos; });
            };
            const auto missingSystemFile = [&]
            {
                std::istringstream lines(message);
                std::string line;
                while (std::getline(lines, line))
                {
                    for (const char* prefix : {"cannot find ", "unable to find library "})
                        for (const std::string library : {"-lc", "-lgcc", "-lgcc_s", "-lmingw32", "-lmingwex",
                                                         "-lmoldname", "-lmsvcrt", "-lucrt", "-lpthread", "-lm"})
                        {
                            const std::string pattern = std::string(prefix) + library;
                            const auto begin = line.find(pattern);
                            if (begin == std::string::npos) continue;
                            const auto end = begin + pattern.size();
                            if (end == line.size() || std::string_view(" \t\r:'\" )").find(line[end]) != std::string_view::npos)
                                return true;
                        }
                    if (line.find("cannot find ") == std::string::npos && line.find("cannot open ") == std::string::npos) continue;
                    for (const char* startup : {"crt1.o", "scrt1.o", "rcrt1.o", "crti.o", "crtn.o", "crtbegin.o",
                                               "crtbegins.o", "crtend.o", "crtends.o", "crt2.o", "dllcrt2.o"})
                        if (line.find(startup) != std::string::npos) return true;
                }
                return false;
            };
            if (containsAny({"error while loading shared libraries:", "library not loaded:"}))
                result.dependencyWarnings.push_back("The Clang driver cannot load one of its own shared libraries. Reinstall a complete LLVM/Clang installation and check its library search path.");
            else if (containsAny({"unable to execute command", "unable to find the linker", "linker not found",
                             "invalid linker name", "could not find a linker", "link.exe not found"}))
                result.dependencyWarnings.push_back("The selected Clang toolchain cannot start a required tool. " + toolchainHint(options.targetTriple));
            else if (!options.objectOnly && (missingSystemFile() || containsAny({"library not found for -lsystem", "library 'system' not found",
                          "could not open 'libcmt.lib'", "could not open 'msvcrt.lib'", "could not open 'kernel32.lib'",
                          "cannot open file 'libcmt.lib'", "cannot open file 'msvcrt.lib'", "cannot open file 'kernel32.lib'",
                          "unable to find a compatible sdk", "no such sysroot directory"})))
                result.dependencyWarnings.push_back("The target platform SDK, startup objects, or system libraries are unavailable. " + toolchainHint(options.targetTriple));
        }
    }

    AOTResult compileAOT(const std::string& llvmIR, const AOTOptions& options)
    {
        AOTResult result;
        if (options.output.empty()) { result.message = "AOT output path is empty."; return result; }
        if (options.optimization > 3) { result.message = "AOT optimization must be between 0 and 3."; return result; }
        if (options.objectOnly && !options.linkInputs.empty())
        { result.message = "Link inputs require an executable build."; return result; }
        std::string temporaryError;
        auto temporary = TemporaryDirectory::create("aevix-aot-", temporaryError);
        if (!temporary) { result.message = temporaryError; return result; }
        const auto irPath = temporary->path() / "module.ll";
        const auto logPath = temporary->path() / "compiler.log";
        { std::ofstream file(irPath, std::ios::binary); file << llvmIR; if (!file) { result.message = "Cannot write temporary LLVM IR."; return result; } }
        std::vector<std::string> arguments{utf8Path(options.clang), "-x", "ir", utf8Path(irPath), "-o", utf8Path(options.output), "-O" + std::to_string(options.optimization)};
        if (options.objectOnly) arguments.push_back("-c");
        if (!options.targetTriple.empty()) { arguments.push_back("-target"); arguments.push_back(options.targetTriple); }
        if (!options.objectOnly)
        {
            // IR 입력 모드를 종료한 뒤 경로로만 지정한 런타임과 외부 객체를 연결한다.
            // Leave IR input mode before linking runtime and foreign objects specified only by paths.
            arguments.push_back("-x"); arguments.push_back("none");
            for (const auto* inputs : {&options.runtimeObjects, &options.linkInputs})
                for (const auto& input : *inputs)
                {
                    std::error_code error;
                    const auto absolute = std::filesystem::absolute(input, error);
                    if (error || !std::filesystem::is_regular_file(absolute, error) || error)
                    {
                        result.message = "Link input is not a regular file: " + utf8Path(input);
                        result.dependencyWarnings.push_back(inputs == &options.runtimeObjects
                            ? "Aevix runtime archive is unavailable: " + utf8Path(input) + ". Reinstall the matching runtime or select it with --runtime FILE."
                            : "An external --link input is unavailable: " + utf8Path(input) + ". Check the path and build this dependency first.");
                        return result;
                    }
                    arguments.push_back(utf8Path(absolute));
                }
            if (!options.runtimeObjects.empty())
            {
                const bool linuxTarget = options.targetTriple.find("linux") != std::string::npos;
#if defined(__linux__)
                if (options.targetTriple.empty() || linuxTarget) arguments.push_back("-pthread");
#else
                if (linuxTarget) arguments.push_back("-pthread");
#endif
                // 복소수의 수치 헬퍼가 사용하는 표준 수학 기호를 Linux에서 연결한다.
                // Link the standard math symbols used by complex numeric helpers on Linux.
                const bool complexMath = llvmIR.find("@aevix_complex_") != std::string_view::npos;
#if defined(__linux__)
                if (complexMath && (options.targetTriple.empty() || linuxTarget)) arguments.push_back("-lm");
#else
                if (complexMath && linuxTarget) arguments.push_back("-lm");
#endif
            }
        }
        ProcessOptions processOptions;
        processOptions.stderrFile = logPath;
        processOptions.timeout = options.timeout;
        const auto process = runProcess(arguments, processOptions);
        { std::ifstream log(logPath, std::ios::binary); result.message.assign(std::istreambuf_iterator<char>(log), {}); }
        if (!process.started || !process.error.empty())
        {
            result.message += process.error;
            if (!process.started)
                result.dependencyWarnings.push_back("Clang driver '" + utf8Path(options.clang)
                    + "' is missing or cannot be started. Install LLVM/Clang or select an executable with --clang FILE.");
            return result;
        }
        result.exitCode = process.exitCode;
        result.succeeded = result.exitCode == 0;
        if (!result.succeeded) diagnoseToolchainFailure(result, options);
        return result;
    }
}
