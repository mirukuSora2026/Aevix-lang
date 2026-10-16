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
#include <fstream>
#include <iterator>
#include <vector>

namespace aevix
{
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
                    { result.message = "Link input is not a regular file: " + utf8Path(input); return result; }
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
            }
        }
        ProcessOptions processOptions;
        processOptions.stderrFile = logPath;
        const auto process = runProcess(arguments, processOptions);
        { std::ifstream log(logPath, std::ios::binary); result.message.assign(std::istreambuf_iterator<char>(log), {}); }
        if (!process.started || !process.error.empty())
        { result.message += process.error; return result; }
        result.exitCode = process.exitCode;
        result.succeeded = result.exitCode == 0;
        return result;
    }
}
