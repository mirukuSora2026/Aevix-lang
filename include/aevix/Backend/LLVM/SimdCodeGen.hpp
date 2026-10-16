// ========================
//
// Created By : miruku_sora2026
//
// File name : SimdCodeGen.hpp
//
// Description :
//     고정 폭 CPU SIMD 명령의 LLVM 출력에 필요한 제한된 콜백 문맥을 정의한다.
//     Define the restricted callback context for emitting fixed-width CPU SIMD instructions as LLVM IR.
//
// ========================

#pragma once

#include "aevix/IR/AIR/Module.hpp"

#include <functional>
#include <ostream>
#include <set>
#include <string>

namespace aevix
{
    struct SimdCodeGenContext final
    {
        std::ostream& output;
        std::set<std::string>& declarations;
        std::function<std::string(TypeId)> type;
        std::function<TypeId(AIRValueId)> valueType;
        std::function<std::string(AIRValueId)> value;
        std::function<std::string()> temporary;
        std::function<void(const std::string&)> guard;
    };

    void emitSimdInstruction(const AIRInstruction& instruction, const TypeContext& types,
                             const SimdCodeGenContext& context);
}
