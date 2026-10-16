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
#include <optional>

namespace aevix
{
    std::optional<std::string> emitLLVM(const AIRModule& module, const TypeCheckResult& types,
                                      const NameResolution& names, Diagnostics& diagnostics,
                                      std::string targetTriple = {});
}
