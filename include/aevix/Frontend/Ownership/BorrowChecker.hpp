// ========================
//
// Created By : miruku_sora2026
//
// File name : BorrowChecker.hpp
//
// Description :
//     AIR 제어 흐름에서 이동, 대여, 참조 수명과 조건부 자원 정리 계획을 검사한다.
//     Check moves, loans, reference lifetimes, and conditional cleanup plans over AIR control flow.
//
// ========================

#pragma once

#include "aevix/IR/AIR/Module.hpp"

#include <map>
#include <set>
#include <unordered_map>

namespace aevix
{
    struct ReferenceProjection final
    {
        AIRProjectionKind kind = AIRProjectionKind::Field;
        std::size_t field = 0;
        SymbolId variant = InvalidSymbolId;
        auto operator<=>(const ReferenceProjection&) const = default;
    };
    using ReferencePath = std::vector<ReferenceProjection>;
    struct ReferenceParameterPath final
    {
        std::size_t parameter = 0;
        ReferencePath path;
        ReferencePath storageProjection;
        bool fromContents = true;
        bool opaqueStorage = false;
        auto operator<=>(const ReferenceParameterPath&) const = default;
    };

    struct ReferenceOrigins
    {
        std::set<std::size_t> parameters;
        // Slice 저장소와 그 안에 복사된 참조 값의 수명을 구분한다.
        // Distinguish a Slice storage lifetime from the reference values copied inside it.
        std::set<std::size_t> elementParameters;
        // 대여한 컨테이너 저장소와 그 안에서 이동하는 참조를 구별한다.
        // Distinguish borrowed container storage from references moved out of its contents.
        std::set<std::size_t> contentParameters;
        // 소유 값의 필드와 참조를 따라간 내용을 구별해 요약한다.
        // Preserve owned-value fields and contents reached through stored references.
        std::set<ReferenceParameterPath> parameterPaths;
        std::set<ReferenceParameterPath> contentPaths;
        std::set<SymbolId> globals;
        bool global = false;
        bool operator==(const ReferenceOrigins&) const = default;
    };

    struct ReferenceCapability final
    {
        ReferenceOrigins parents;
        bool exclusive = false;
        bool operator==(const ReferenceCapability&) const = default;
    };

    struct ReferenceReturnSummary final : ReferenceOrigins
    {
        // 수정 매개변수의 종료 시점 내용도 호출자의 실제 저장소에서 검사한다.
        // Check final mutable-parameter contents against the actual caller storage.
        std::map<std::size_t, ReferenceOrigins> stores;
        std::map<ReferencePath, ReferenceOrigins> fields;
        std::map<std::size_t, std::map<ReferencePath, ReferenceOrigins>> storeFields;
        std::map<ReferencePath, ReferenceCapability> fieldCapabilities;
        std::map<ReferenceParameterPath, ReferenceOrigins> indirectStores;
        // 대상 저장소와 보호자 등 부모의 생존 의존성을 별도로 요약한다.
        // Summarize target storage separately from retained parent lifetimes such as guards.
        ReferenceOrigins retainedParents;
        bool derivedCapability = false;
        bool targetExclusive = false;
        bool operator==(const ReferenceReturnSummary&) const = default;
    };

    struct OwnershipResult final
    {
        // 반환 참조가 빌리는 매개변수 위치이며 소유한 값 반환과는 구별된다.
        // These are parameter positions borrowed by the result, distinct from owned value returns.
        std::unordered_map<SymbolId, ReferenceReturnSummary> returns;
        std::size_t plannedDrops = 0;
        bool succeeded = false;
    };

    class BorrowChecker final
    {
    public:
        explicit BorrowChecker(Diagnostics& diagnostics);

        // 성공한 AIR에만 조건부 Drop을 추가한다. 같은 모듈을 다시 검사해도 중복하지 않는다.
        // Add conditional Drops only to successful AIR, without duplication on repeated checks.
        OwnershipResult check(AIRModule& module, const NameResolution& names,
                              const TypeCheckResult& types);

    private:
        Diagnostics& diagnostics_;
    };
}
