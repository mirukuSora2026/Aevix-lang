// ========================
//
// Created By : miruku_sora2026
//
// File name : TypeChecker.hpp
//
// Description :
//     해석된 선언과 표현식의 타입, 호출 대상 및 정적 타입 검사 결과를 정의한다.
//     Define resolved declaration and expression types, selected calls, and static type-checking results.
//
// ========================

#pragma once

#include "aevix/Frontend/Sema/Type.hpp"
#include "aevix/Frontend/Sema/ConstEvaluator.hpp"
#include "aevix/Frontend/Sema/TypeLayout.hpp"

#include <unordered_map>
#include <unordered_set>

namespace aevix
{
    struct EnumVariantInfo final
    {
        SymbolId variant = InvalidSymbolId;
        TypeId type = InvalidTypeId;
        std::size_t tag = 0;
        std::vector<SymbolId> fields;
        std::vector<TypeId> fieldTypes;
    };
    struct EnumBindingInfo final
    {
        SymbolId variant = InvalidSymbolId;
        SymbolId field = InvalidSymbolId;
        bool consuming = false;
        bool mutableBorrow = false;
    };
    struct CoreCarrierInfo final
    {
        bool optional = false;
        TypeId type = InvalidTypeId;
        TypeId value = InvalidTypeId;
        TypeId error = InvalidTypeId;
        SymbolId success = InvalidSymbolId;
        SymbolId failure = InvalidSymbolId;
    };
    struct CorePredicateInfo final
    {
        TypeId carrier = InvalidTypeId;
        bool success = true;
    };
    struct PropagationInfo final
    {
        TypeId operand = InvalidTypeId;
        TypeId result = InvalidTypeId;
    };
    struct DynViewInfo final
    {
        TypeId original = InvalidTypeId;
        TypeId interfaceType = InvalidTypeId;
        TypeId concreteType = InvalidTypeId;
        std::vector<SymbolId> methods;
    };
    struct DynCallInfo final
    {
        TypeId interfaceType = InvalidTypeId;
        std::size_t slot = 0;
        std::vector<SymbolId> targets;
    };
    struct TypeCheckResult final
    {
        TypeContext types;
        std::unordered_map<const Node*, TypeId> nodeTypes;
        std::unordered_map<SymbolId, TypeId> symbolTypes;
        std::unordered_map<const Node*, SymbolId> calls;
        std::unordered_map<const Node*, ConstantValue> constants;
        std::unordered_map<TypeId, TypeLayoutInfo> layouts;
        std::unordered_map<SymbolId, EnumVariantInfo> enumVariants;
        std::unordered_map<const Node*, SymbolId> enumConstructors;
        std::unordered_map<const Node*, SymbolId> enumCases;
        std::unordered_map<const Node*, EnumBindingInfo> enumBindings;
        std::unordered_set<const Node*> exhaustiveSwitches;
        std::unordered_map<TypeId, CoreCarrierInfo> carriers;
        std::unordered_map<const Node*, CorePredicateInfo> predicates;
        std::unordered_map<const Node*, PropagationInfo> propagations;
        std::unordered_set<const Node*> unitValues;
        std::unordered_map<const Node*, std::string> intrinsics;
        std::unordered_map<const Node*, TypeId> variadicPacks;
        std::unordered_map<const Node*, TypeId> dynamicLiterals;
        std::unordered_map<const Node*, TypeId> collectionElements;
        std::unordered_map<const Node*, std::string> collectionCalls;
        std::unordered_map<const Node*, DynViewInfo> dynViews;
        std::unordered_map<const Node*, DynViewInfo> boxDynViews;
        std::unordered_map<const Node*, std::string> boxIntrinsics;
        std::unordered_map<const Node*, DynCallInfo> dynCalls;
        std::unordered_map<const Node*, std::string> valueIntrinsics;
        std::unordered_map<const Node*, std::size_t> tupleIndices;
        std::unordered_map<const Node*, TypeId> iterationViews;
        std::unordered_map<const Node*, TypeId> simdTypes;
        // InvalidSymbolId는 worker_count이며 나머지는 검증한 정적 작업 함수다.
        // InvalidSymbolId denotes worker_count; other entries identify validated static workers.
        std::unordered_map<const Node*, SymbolId> parallelCalls;
        bool succeeded = false;
    };

    class TypeChecker final
    {
    public:
        explicit TypeChecker(Diagnostics& diagnostics);
        TypeCheckResult check(const ModuleGraph& graph, const NameResolution& names);

    private:
        Diagnostics& diagnostics_;
    };
}
