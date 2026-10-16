// ========================
//
// Created By : miruku_sora2026
//
// File name : JITOptions.hpp
//
// Description :
//     네이티브 ORC 실행의 최적화 수준과 C 심벌 및 공유 라이브러리 검색 설정을 정의한다.
//     Define native ORC optimization, process-symbol lookup, and shared-library search settings.
//
// ========================

#pragma once

#include <filesystem>
#include <vector>

namespace aevix
{
    struct JITOptions final
    {
        unsigned optimization = 0;
        bool processSymbols = true;
        // 명시한 라이브러리를 프로세스 심벌보다 먼저 검색하며 로더는 라이브러리를 유지한다.
        // Explicit libraries are searched before process symbols and retained by the dynamic loader.
        std::vector<std::filesystem::path> sharedLibraries;
    };
}
