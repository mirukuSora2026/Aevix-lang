// ========================
//
// Created By : miruku_sora2026
//
// File name : AtomicOperation.hpp
//
// Description :
//     컴파일러가 허용하는 CPU 원자 연산과 정확한 피연산자 역할을 정의한다.
//     Define the closed CPU atomic operations and their exact operand roles.
//
// ========================
#pragma once
#include "aevix/Runtime/AtomicRuntime.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aevix
{
    enum class AtomicResult { Cell, Scalar, Unit, MutableReference, Cas, Wait, Boolean, Alignment };
    struct AtomicOperationInfo final
    {
        std::string_view name;
        std::size_t operands;
        AtomicResult result;
        bool fallible;
        bool cell;
        bool integerOnly;
        int order;
        int failure;
        int value;
        std::uint32_t native;
    };
    inline const AtomicOperationInfo* atomicOperation(std::string_view name)
    {
        using enum AtomicResult;
        static constexpr std::uint32_t none = 0xffffffffu;
        static constexpr std::array operations = {
            AtomicOperationInfo{"new",1,Cell,false,false,false,-1,-1,0,none},
            AtomicOperationInfo{"load",2,Scalar,true,true,false,1,-1,-1,0},
            AtomicOperationInfo{"store",3,Unit,true,true,false,2,-1,1,1},
            AtomicOperationInfo{"swap",3,Scalar,true,true,false,2,-1,1,2},
            AtomicOperationInfo{"compare_exchange",5,Cas,true,true,false,3,4,2,3},
            AtomicOperationInfo{"compare_exchange_weak",5,Cas,true,true,false,3,4,2,4},
            AtomicOperationInfo{"fetch_add",3,Scalar,true,true,true,2,-1,1,5},
            AtomicOperationInfo{"fetch_sub",3,Scalar,true,true,true,2,-1,1,6},
            AtomicOperationInfo{"fetch_and",3,Scalar,true,true,false,2,-1,1,7},
            AtomicOperationInfo{"fetch_or",3,Scalar,true,true,false,2,-1,1,8},
            AtomicOperationInfo{"fetch_xor",3,Scalar,true,true,false,2,-1,1,9},
            AtomicOperationInfo{"fetch_nand",3,Scalar,true,true,false,2,-1,1,10},
            AtomicOperationInfo{"fetch_min",3,Scalar,true,true,true,2,-1,1,11},
            AtomicOperationInfo{"fetch_max",3,Scalar,true,true,true,2,-1,1,12},
            AtomicOperationInfo{"checked_fetch_add",3,Scalar,true,true,true,2,-1,1,13},
            AtomicOperationInfo{"checked_fetch_sub",3,Scalar,true,true,true,2,-1,1,14},
            AtomicOperationInfo{"saturating_fetch_add",3,Scalar,true,true,true,2,-1,1,15},
            AtomicOperationInfo{"saturating_fetch_sub",3,Scalar,true,true,true,2,-1,1,16},
            AtomicOperationInfo{"get_mut",1,MutableReference,false,true,false,-1,-1,-1,none},
            AtomicOperationInfo{"into_inner",1,Scalar,false,true,false,-1,-1,-1,none},
            AtomicOperationInfo{"is_supported",0,Boolean,false,false,false,-1,-1,-1,none},
            AtomicOperationInfo{"is_lock_free",0,Boolean,false,false,false,-1,-1,-1,none},
            AtomicOperationInfo{"required_alignment",0,Alignment,false,false,false,-1,-1,-1,none},
            AtomicOperationInfo{"fence",1,Unit,true,false,false,0,-1,-1,none},
            AtomicOperationInfo{"compiler_fence",1,Unit,true,false,false,0,-1,-1,none},
            AtomicOperationInfo{"wait",3,Scalar,true,true,false,2,-1,1,none},
            AtomicOperationInfo{"wait_timeout",4,Wait,true,true,false,3,-1,1,none},
            AtomicOperationInfo{"notify_one",1,Unit,false,true,false,-1,-1,-1,17},
            AtomicOperationInfo{"notify_all",1,Unit,false,true,false,-1,-1,-1,18}
        };
        for (const auto& operation : operations) if (operation.name == name) return &operation;
        return nullptr;
    }
}
