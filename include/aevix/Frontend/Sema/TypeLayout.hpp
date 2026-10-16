// ========================
//
// Created By : miruku_sora2026
//
// File name : TypeLayout.hpp
//
// Description :
//     64비트 대상의 언어 내부 저장 배치와 재귀 값 타입 검사를 정의한다.
//     Define internal storage layouts and recursive value-type checks for 64-bit targets.
//
// ========================

#pragma once

#include "aevix/Frontend/Sema/Type.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace aevix
{
    enum class LayoutTarget { AArch64, X86_64 };
    struct FieldLayout final
    {
        SymbolId field = InvalidSymbolId;
        std::size_t offset = 0;
        TypeId type = InvalidTypeId;
    };
    struct TypeLayoutInfo final
    {
        std::size_t size = 0;
        std::size_t alignment = 1;
        std::vector<FieldLayout> fields;
    };
    struct TypeLayoutResult final
    {
        bool succeeded = false;
        TypeLayoutInfo layout;
        SourceRange range;
        std::string message;
    };

    // 이 배치는 언어 내부 저장 규약이며 C/C++ 호출 ABI를 보장하지 않는다.
    // This layout is an internal storage contract, not a C/C++ calling ABI guarantee.
    class TypeLayout final
    {
    public:
        TypeLayout(const TypeContext& types, const NameResolution& names,
            const std::unordered_map<SymbolId, TypeId>& symbolTypes,
            LayoutTarget target = LayoutTarget::AArch64);
        TypeLayoutResult layout(TypeId type) const;

    private:
        const TypeContext& types_;
        const NameResolution& names_;
        const std::unordered_map<SymbolId, TypeId>& symbolTypes_;
        LayoutTarget target_;
    };
}
