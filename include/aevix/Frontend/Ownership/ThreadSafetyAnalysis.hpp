// ========================
//
// Created By : miruku_sora2026
//
// File name : ThreadSafetyAnalysis.hpp
//
// Description :
//     구체 AIR의 봉인된 스레드 전달 특성과 전이적 작업·정리 효과를 분석한다.
//     Analyze sealed thread-transfer traits and transitive worker/cleanup effects in concrete AIR.
//
// ========================
#pragma once

#include "aevix/IR/AIR/Module.hpp"
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace aevix
{
    // 이 특성은 수명 증명이 아니다. containsBorrow는 별도의 스코프 대여 검사가 필요함을 표시한다.
    // These traits are not lifetime proofs; containsBorrow requires separate scoped-loan validation.
    struct ThreadTypeFacts final
    {
        bool send = false;
        bool sync = false;
        bool containsBorrow = false;
        bool operator==(const ThreadTypeFacts&) const = default;
    };
    enum class ThreadEffectKind
    {
        MissingFunction, ForeignCall, DynamicCall, UnapprovedIntrinsic,
        MissingGlobal, MutableGlobal, GlobalNotSync, UnknownCleanup
    };
    struct ThreadEffectIssue final
    {
        ThreadEffectKind kind = ThreadEffectKind::MissingFunction;
        SymbolId origin = InvalidSymbolId;
        SymbolId subject = InvalidSymbolId;
        SourceRange range{};
        std::string message;
        // 첫 항목은 요약 대상 함수이며 마지막 항목에서 위반이 직접 발생한다.
        // The path starts at the summarized function and ends where the violation occurs.
        std::vector<SymbolId> callPath;
    };
    struct ThreadFunctionEffects final
    {
        bool workerSafe = true;
        std::set<SymbolId> directCalls;
        std::set<SymbolId> cleanupCalls;
        std::set<SymbolId> globalsRead;
        std::set<SymbolId> globalsWritten;
        std::optional<ThreadEffectIssue> issue;
    };
    struct ThreadFunctionSignature final
    {
        TypeId result = InvalidTypeId;
        std::vector<TypeId> parameters;
    };
    struct ThreadSafetyResult final
    {
        bool validAIR = false;
        std::map<TypeId, ThreadTypeFacts> types;
        std::map<SymbolId, ThreadFunctionEffects> functions;
        std::map<SymbolId, ThreadFunctionSignature> signatures;
        std::map<TypeId, Type> descriptions;
        ThreadTypeFacts traits(TypeId type) const;
    };

    class ThreadSafetyAnalysis final
    {
    public:
        explicit ThreadSafetyAnalysis(Diagnostics& diagnostics);
        // 모든 일반 함수를 거부하지 않고 사실만 계산한다. 실제 작업 허가는 별도의 요청으로 검사한다.
        // Compute facts without rejecting ordinary functions; worker admission is a separate explicit request.
        ThreadSafetyResult analyze(const AIRModule& module, const TypeCheckResult& typed);
        // 첫 프로필은 소유한 캡처·결과만 허용한다. 정적 참조도 별도 증명 전에는 허용하지 않는다.
        // The first profile admits owned captures/results only; even static references await separate proof.
        // @since 0.3
        // @deprecated_at none
        // @removed_at none
        bool validateOwnedWorker(const ThreadSafetyResult& analysis, SymbolId invoke,
            TypeId callable, TypeId result, SourceRange range = {});
        // 미계산 작업자가 없는 Ready 값에도 소유 데이터와 안전한 정리의 초기 경계를 유지한다.
        // Keep the initial owned-data and approved-cleanup boundary for Ready without inventing a worker.
        // @since 0.3
        // @deprecated_at none
        // @removed_at none
        bool validateOwnedReady(const ThreadSafetyResult& analysis, TypeId result, SourceRange range = {});
    private:
        Diagnostics& diagnostics_;
    };

    // 검사 모드와 모든 코드 생성 경로에서 동일한 작업 허가 검사를 사용한다.
    // Use the same worker-admission check in check mode and every code-generation path.
    bool validateTaskWorkers(const AIRModule& module, const TypeCheckResult& typed,
                             Diagnostics& diagnostics);
}
