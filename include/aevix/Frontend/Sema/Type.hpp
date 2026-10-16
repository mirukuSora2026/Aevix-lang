// ========================
//
// Created By : miruku_sora2026
//
// File name : Type.hpp
//
// Description :
//     기본형과 합성 타입의 정규 표현 및 동일 타입을 공유하는 타입 저장소를 정의한다.
//     Define canonical representations of builtin and composite types and their shared type context.
//
// ========================

#pragma once

#include "aevix/Frontend/Sema/SymbolTable.hpp"

#include <cstddef>
#include <deque>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aevix
{
    using TypeId = std::size_t;
    inline constexpr TypeId InvalidTypeId = std::numeric_limits<TypeId>::max();

    enum class TypeKind
    {
        Error,
        Void,
        Never,
        Unit,
        Bool,
        Integer,
        Float,
        Char,
        Byte,
        String,
        StringView,
        Reference,
        Function,
        Nominal,
        Array,
        Range,
        Vector,
        Slice,
        Dynamic,
        Box,
        Tuple,
        Simd,
        SimdMask
    };

    /// 타입 ID는 해당 TypeContext 안에서만 의미를 가지며 별칭은 같은 ID를 사용한다.
    /// Type IDs belong to one TypeContext, and aliases reuse their canonical type ID.
    struct Type final
    {
        TypeKind kind = TypeKind::Error;
        std::string name;
        // 논리적인 숫자 비트 폭이며 대상 플랫폼의 저장 크기나 정렬을 의미하지 않는다.
        // This is the logical numeric width, not a target-specific storage size or alignment.
        std::size_t bits = 0;
        bool isSigned = false;
        bool isMutable = false;
        TypeId element = InvalidTypeId;
        TypeId result = InvalidTypeId;
        std::vector<TypeId> parameters;
        SymbolId declaration = InvalidSymbolId;
        std::size_t extent = 0;
    };

    class TypeContext final
    {
    public:
        TypeContext();

        TypeId builtin(std::string_view name) const;
        TypeId error() const noexcept;
        TypeId reference(TypeId element, bool isMutable);
        TypeId function(TypeId result, const std::vector<TypeId>& parameters);
        TypeId nominal(SymbolId declaration, std::string name);
        TypeId array(TypeId element, std::size_t extent);
        TypeId range(TypeId element);
        TypeId vector(TypeId element);
        TypeId slice(TypeId element, bool isMutable = false);
        TypeId tuple(const std::vector<TypeId>& fields);
        TypeId simd(TypeId element, std::size_t lanes);
        TypeId simdMask(std::size_t lanes);
        TypeId dynamic(SymbolId declaration, std::string name);
        TypeId boxed(SymbolId declaration, std::string name, TypeId element);

        // 잘못된 ID는 오류 타입으로 처리하여 후속 검사의 연쇄 오류를 줄인다.
        // Invalid IDs resolve to the error type to limit cascading diagnostics.
        const Type& get(TypeId id) const noexcept;
        std::string display(TypeId id) const;

    private:
        TypeId add(Type type);
        void addBuiltin(std::string name, TypeKind kind, std::size_t bits = 0, bool isSigned = false);

        // 타입을 추가해도 이미 반환한 타입 참조의 주소를 유지한다.
        // Adding types preserves references previously returned by get().
        std::deque<Type> types_;
        std::unordered_map<std::string, TypeId> builtins_;
        std::map<std::pair<TypeId, bool>, TypeId> references_;
        std::map<std::pair<TypeId, std::vector<TypeId>>, TypeId> functions_;
        std::unordered_map<SymbolId, TypeId> nominals_;
        std::map<std::pair<TypeId, std::size_t>, TypeId> arrays_;
        std::map<TypeId, TypeId> ranges_;
        std::map<TypeId, TypeId> vectors_;
        std::map<std::pair<TypeId, bool>, TypeId> slices_;
        std::map<std::vector<TypeId>, TypeId> tuples_;
        std::map<std::pair<TypeId, std::size_t>, TypeId> simd_;
        std::map<std::size_t, TypeId> simdMasks_;
    };
}
