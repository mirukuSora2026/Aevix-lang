// ========================
//
// Created By : miruku_sora2026
//
// File name : ProofRangeAnalysis.hpp
//
// Description :
//     수렴한 AIR 정수 구간으로만 산술 및 고정 배열 검사의 불필요함을 증명한다.
//     Prove arithmetic and fixed-array checks redundant only from converged AIR integer intervals.
//
// ========================
#pragma once
#include "aevix/IR/AIR/Module.hpp"
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
namespace aevix {
struct ProofRangeOptions final {
    std::size_t maxCFGIterations = 4096;
    std::size_t maxInstructionVisits = 1000000;
    std::size_t maxStateCells = 1000000;
    std::size_t maxBlocks = 4096;
};
struct ProofRangeStats final {
    std::size_t functions = 0;
    std::size_t skipped = 0;
    std::size_t considered = 0;
    std::size_t proven = 0;
    std::size_t arithmeticConsidered = 0;
    std::size_t arithmeticProven = 0;
    std::size_t fixedIndicesConsidered = 0;
    std::size_t fixedIndicesProven = 0;
    std::size_t cfgIterations = 0;
    std::size_t instructionVisits = 0;
    bool budgetFallback = false;
    std::string skipReason;
};
struct ProofRangeResult final {
    // AIR과 TypeContext는 결과보다 오래 살아 있어야 하며 분석 후 변경되면 안 된다.
    // AIR and TypeContext must outlive this result and remain unchanged after analysis.
    std::unordered_set<const AIRInstruction*> arithmeticNoOverflow;
    std::unordered_map<const AIRInstruction*, std::unordered_set<std::size_t>> fixedArrayIndices;
    ProofRangeStats stats;
};
// 타입·제어 흐름·대여 검증을 통과한 불변 AIR에만 사용한다. 정리된 종료 간선도 보수적으로 취급한다.
// Requires immutable, type/control-flow/borrow-verified AIR; outgoing cleanup edges are handled conservatively.
[[nodiscard]] ProofRangeResult analyzeProofRanges(const AIRFunction& function,
    const TypeContext& types, const ProofRangeOptions& options = {});
}
