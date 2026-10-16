// ========================
//
// Created By : miruku_sora2026
//
// File name : NameResolver.hpp
//
// Description :
//     모듈 선언, 가져오기 및 지역 이름을 연결하는 이름 해석 인터페이스를 선언한다.
//     Declare the name resolution interface for connecting module declarations, imports, and lexical names.
//
// ========================

#pragma once

#include "aevix/Common/Diagnostic.hpp"
#include "aevix/Frontend/Sema/SymbolTable.hpp"

namespace aevix
{
    /// 모든 모듈의 선언을 먼저 모은 후 가져오기와 지역 이름을 연결한다.
    /// Collect all module declarations before connecting imports and lexical names.
    class NameResolver final
    {
    public:
        explicit NameResolver(Diagnostics& diagnostics);
        [[nodiscard]] NameResolution resolve(const ModuleGraph& graph);

    private:
        Diagnostics& diagnostics_;
    };
}
