// ========================
//
// Created By : miruku_sora2026
//
// File name : CoreErrorTypes.hpp
//
// Description :
//     라이브러리 파일 없이 오류 운반 타입의 컴파일러 내부 선언을 설치한다.
//     Install compiler-owned error carrier declarations without library files.
//
// ========================

#pragma once

#include "aevix/Frontend/Modules/ModuleLoader.hpp"

namespace aevix
{
    bool installCoreErrorTypes(ModuleGraph& graph, SourceManager& sources, Diagnostics& diagnostics);
}
