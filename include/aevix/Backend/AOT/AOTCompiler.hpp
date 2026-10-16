// ========================
//
// Created By : miruku_sora2026
//
// File name : AOTCompiler.hpp
//
// Description :
//     쉘을 사용하지 않고 LLVM IR을 객체 파일이나 실행 파일로 컴파일한다.
//     Compile LLVM IR to object files or executables without invoking a shell.
//
// ========================
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace aevix
{
    struct AOTOptions final
    {
        std::filesystem::path clang = "clang";
        std::filesystem::path output;
        std::string targetTriple;
        bool objectOnly = false;
        unsigned optimization = 0;
        std::vector<std::filesystem::path> runtimeObjects;
        std::vector<std::filesystem::path> linkInputs;
    };
    struct AOTResult final
    {
        bool succeeded = false;
        int exitCode = -1;
        std::string message;
    };
    AOTResult compileAOT(const std::string& llvmIR, const AOTOptions& options);
}
