// ========================
//
// Created By : miruku_sora2026
//
// File name : ForeignValidator.hpp
//
// Description :
//     초기 C FFI에서 지원하는 함수 서명과 외부 심볼의 일관성을 검사한다.
//     Validate function signatures and external symbol consistency for the initial C FFI.
//
// ========================

#pragma once

#include "aevix/Frontend/Sema/TypeChecker.hpp"

namespace aevix
{
    bool validateForeignDeclarations(const NameResolution& names, const TypeCheckResult& types,
                                     Diagnostics& diagnostics);
}
