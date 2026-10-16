// ========================
//
// Created By : miruku_sora2026
//
// File name : ASTLowering.hpp
//
// Description :
//     타입 검사 결과를 사용하여 AST를 명시적인 AIR 제어 흐름으로 변환한다.
//     Lower the checked AST into explicit typed AIR control flow.
//
// ========================

#pragma once
#include "aevix/IR/AIR/Module.hpp"
#include <optional>

namespace aevix
{
    std::optional<AIRModule> lowerToAIR(const ModuleGraph& graph, const NameResolution& names,
                                      const TypeCheckResult& types, Diagnostics& diagnostics);
}
