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

#include <set>
#include <unordered_map>

namespace aevix
{
    struct ReferenceReturnSummary final
    {
        std::set<std::size_t> parameters;
        // Slice 저장소와 그 안에 복사된 참조 값의 수명을 구분한다.
        // Distinguish a Slice storage lifetime from the reference values copied inside it.
        std::set<std::size_t> elementParameters;
        std::set<SymbolId> globals;
        bool global = false;
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
