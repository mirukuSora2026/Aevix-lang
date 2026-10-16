// ========================
//
// Created By : miruku_sora2026
//
// File name : SymbolTable.hpp
//
// Description :
//     선언의 원본 식별자, 접근 권한, 이름 후보 및 AST 참조 연결 결과를 정의한다.
//     Define original declaration identities, visibility, name candidates, and resolved AST references.
//
// ========================

#pragma once

#include "aevix/Frontend/Modules/ModuleLoader.hpp"

#include <cstddef>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace aevix
{
    using SymbolId = std::size_t;
    inline constexpr SymbolId InvalidSymbolId = std::numeric_limits<SymbolId>::max();

    enum class SymbolKind
    {
        Type,
        Function,
        Value,
        Intrinsic
    };

    /// 선언의 원본 ID를 유지하여 재공개 경로가 달라도 같은 선언으로 판별한다.
    /// Preserve the original ID so re-export paths still identify the same declaration.
    struct ResolvedSymbol final
    {
        SymbolId id = InvalidSymbolId;
        ModuleId module = InvalidModuleId;
        const Node* declaration = nullptr;
        std::string name;
        SymbolKind kind = SymbolKind::Value;
        bool isPublic = false;
    };

    using SymbolTable = std::unordered_map<std::string, std::vector<SymbolId>>;

    /// AST는 ModuleGraph가 소유하며 이 결과보다 오래 살아 있어야 한다.
    /// ModuleGraph owns the AST and must outlive this result.
    struct NameResolution final
    {
        std::vector<ResolvedSymbol> symbols;
        std::unordered_map<const Node*, std::vector<SymbolId>> references;
        bool succeeded = false;
    };
}
