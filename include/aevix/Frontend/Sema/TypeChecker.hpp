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
        bool borrowedBinding = false;
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
    struct BoxExtractionInfo final
    {
        TypeId owner = InvalidTypeId;
        TypeId payload = InvalidTypeId;
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
        // 검증된 구체 Box 소유자와 이동할 실제 T를 별도로 기록한다.
        // Record the validated concrete Box owner and exact extracted T separately.
        std::unordered_map<const Node*, BoxExtractionInfo> boxExtractions;
        std::unordered_map<const Node*, DynCallInfo> dynCalls;
        std::unordered_map<const Node*, std::string> valueIntrinsics;
        // 출력이 소유 문자열을 이동하지 않도록 내부 읽기 대여 타입을 보관한다.
        // Keep internal shared-loan types so output does not consume owned strings.
        std::unordered_map<const Node*, TypeId> systemBorrows;
        std::unordered_map<const Node*, std::size_t> tupleIndices;
        std::unordered_map<const Node*, TypeId> iterationViews;
        std::unordered_map<const Node*, TypeId> simdTypes;
        // 명시적 return에만 허용한 bool의 0/1 정수 변환을 표현식 타입과 분리한다.
        // Keep return-only bool-to-0/1 adaptations separate from expression types.
        std::unordered_map<const Node*, TypeId> returnConversions;
        // 검증한 사용자 정리 메서드를 구체 소유 타입에 연결하여 자동 정리에 사용한다.
        // Associate validated user cleanup methods with concrete owned types for automatic cleanup.
        std::unordered_map<TypeId, SymbolId> dropMethods;
        // InvalidSymbolId는 worker_count이며 나머지는 검증한 정적 작업 함수다.
        // InvalidSymbolId denotes worker_count; other entries identify validated static workers.
        std::unordered_map<const Node*, SymbolId> parallelCalls;
        std::unordered_map<const Node*, SymbolId> taskInvokes;
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
