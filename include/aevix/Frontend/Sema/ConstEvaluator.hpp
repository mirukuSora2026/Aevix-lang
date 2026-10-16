// ========================
//
// Created By : miruku_sora2026
//
// File name : ConstEvaluator.hpp
//
// Description :
//     정적 상수식의 값과 실패 원인을 표현하는 제한된 상수 계산기를 정의한다.
//     Define a bounded constant evaluator and its values and failure reasons.
//
// ========================

#pragma once

#include "aevix/Frontend/Sema/Type.hpp"
#include "aevix/Common/UInt128.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace aevix
{
    enum class ConstantKind { Integer, Float, Boolean, Character, String, Unit };
    struct ConstantValue final
    {
        ConstantKind kind = ConstantKind::Unit;
        TypeId type = InvalidTypeId;
        std::uint64_t magnitude = 0;
        std::uint64_t highMagnitude = 0;
        bool negative = false;
        long double floating = 0;
        bool boolean = false;
        std::string text;
        UInt128 integerMagnitude() const { return {magnitude, highMagnitude}; }
        void setIntegerMagnitude(UInt128 value) { magnitude = value.low; highMagnitude = value.high; }
        std::string integerSpelling() const { return std::string(negative ? "-" : "") + integerMagnitude().decimal(); }
        std::string key() const;
    };

    enum class ConstStatus { Constant, NotConstant, Invalid };
    struct ConstEvaluation final
    {
        ConstStatus status = ConstStatus::NotConstant;
        ConstantValue value;
        SourceRange range;
        std::string message;
    };

    class ConstEvaluator final
    {
    public:
        ConstEvaluator(const NameResolution& names, const TypeContext& types,
            const std::unordered_map<const Node*, TypeId>& nodeTypes,
            const std::unordered_map<SymbolId, TypeId>& symbolTypes);
        ConstEvaluation evaluate(const Node& node, TypeId expected = InvalidTypeId) const;

    private:
        const NameResolution& names_;
        const TypeContext& types_;
        const std::unordered_map<const Node*, TypeId>& nodeTypes_;
        const std::unordered_map<SymbolId, TypeId>& symbolTypes_;
    };
}
