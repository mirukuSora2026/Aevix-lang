// ========================
//
// Created By : miruku_sora2026
//
// File name : ControlFlowChecker.hpp
//
// Description :
//     실행 경로별 초기화 상태, 분기 종료와 함수의 반환 누락을 검사한다.
//     Check path-sensitive initialization, control exits, and missing function returns.
//
// ========================

#pragma once

#include "aevix/Frontend/Sema/TypeChecker.hpp"

namespace aevix
{
    class ControlFlowChecker final
    {
    public:
        explicit ControlFlowChecker(Diagnostics& diagnostics);
        [[nodiscard]] bool check(const ModuleGraph& graph,
                                 const NameResolution& names,
                                 const TypeCheckResult& types);

    private:
        Diagnostics& diagnostics_;
    };
}
