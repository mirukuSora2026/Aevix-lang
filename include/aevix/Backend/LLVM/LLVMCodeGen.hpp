// ========================
//
// Created By : miruku_sora2026
//
// File name : LLVMCodeGen.hpp
//
// Description :
//     타입이 붙은 AIR를 안전 검사가 포함된 LLVM IR로 변환한다.
//     Translate typed AIR into LLVM IR with explicit runtime safety checks.
//
// ========================
#pragma once
#include "aevix/IR/AIR/Module.hpp"
#include <cstdint>
#include <optional>

namespace aevix
{
    // 증명한 지점과 실제 제거한 검사 출력을 구분하며 기본 경로에서는 분석하지 않는다.
    // Distinguish proven sites from emitted checks removed; the default path performs no analysis.
    struct ProofOptimizationStatistics final
    {
        std::uint64_t functions = 0;
        std::uint64_t skipped = 0;
        std::uint64_t budgetFallbacks = 0;
        std::uint64_t arithmeticConsidered = 0;
        std::uint64_t arithmeticProven = 0;
        std::uint64_t arithmeticChecksRemoved = 0;
        std::uint64_t fixedIndicesConsidered = 0;
        std::uint64_t fixedIndicesProven = 0;
        std::uint64_t boundsChecksRemoved = 0;
        std::uint64_t cfgIterations = 0;
        std::uint64_t analysisNanoseconds = 0;
    };
    struct ProofOptimizationOptions final
    {
        bool enabled = false;
        ProofOptimizationStatistics* statistics = nullptr;
    };
    std::optional<std::string> emitLLVM(const AIRModule& module, const TypeCheckResult& types,
                                      const NameResolution& names, Diagnostics& diagnostics,
                                      std::string targetTriple = {}, ProofOptimizationOptions proof = {});
}
