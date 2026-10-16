// ========================
//
// Created By : miruku_sora2026
//
// File name : ProcessTestSupport.hpp
//
// Description :
//     운영체제별 실행 파일 이름과 안전 정지 판정을 공통 테스트에서 사용한다.
//     Share platform executable names and safe-stop checks across process tests.
//
// ========================
#pragma once
#include "aevix/Support/Process.hpp"
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iterator>

namespace aevix::test
{
    inline std::filesystem::path executablePath(const std::filesystem::path& directory,
                                                const std::string& name)
    {
#if defined(_WIN32)
        return directory / (name + ".exe");
#else
        return directory / name;
#endif
    }

    inline bool exitedWith(const ProcessResult& result, int code)
    {
        return result.started && !result.timedOut && result.exitedNormally && result.exitCode == code;
    }

    inline bool arithmeticTrap(const ProcessResult& result)
    {
        if (!result.started || result.timedOut || result.exitedNormally) return false;
#if defined(_WIN32)
        const auto code = static_cast<std::uint32_t>(result.exitCode);
        return code == 0xc000001du || code == 0x80000003u;
#else
        return result.signal == SIGILL || result.signal == SIGTRAP;
#endif
    }

    inline bool persistAbort(const ProcessResult& result, const std::filesystem::path& log)
    {
        if (!result.started || result.timedOut || result.exitCode == 0) return false;
#if !defined(_WIN32)
        if (result.exitedNormally || result.signal != SIGABRT) return false;
#endif
        // CRT마다 abort 종료 코드가 달라질 수 있으므로 런타임 진단도 확인한다.
        // Check the runtime diagnostic because abort exit codes vary between CRTs.
        std::ifstream file(log, std::ios::binary);
        const std::string diagnostic(std::istreambuf_iterator<char>(file), {});
        return diagnostic.find("Aevix persist initialization error:") != std::string::npos;
    }
}
