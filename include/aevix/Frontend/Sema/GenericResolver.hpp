// ========================
//
// Created By : miruku_sora2026
//
// File name : GenericResolver.hpp
//
// Description :
//     구체 제네릭 인스턴스와 정적 인터페이스 기본 구현을 생성한다.
//     Materialize concrete generic instances and static interface default implementations.
//
// ========================

#pragma once

#include "aevix/Frontend/Sema/NameResolver.hpp"

#include <optional>

namespace aevix
{
    class GenericResolver final
    {
    public:
        explicit GenericResolver(Diagnostics& diagnostics);
        std::optional<NameResolution> resolve(ModuleGraph& graph, const NameResolution& names);
    private:
        Diagnostics& diagnostics_;
    };
}
