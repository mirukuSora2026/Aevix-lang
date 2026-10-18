// ========================
//
// Created By : miruku_sora2026
//
// File name : RangeOperation.hpp
//
// Description :
//     범위 설명자의 경계·간격·방향을 읽는 닫힌 순수 연산을 정의한다.
//     Define closed pure operations inspecting range bounds, magnitude, and direction.
//
// ========================
#pragma once
#include <array>
#include <cstddef>
#include <string_view>

namespace aevix
{
    enum class RangeInspectResult { Element, Magnitude, Boolean, Empty };
    struct RangeInspectOperation final
    {
        std::string_view name;
        RangeInspectResult result;
        std::size_t field;
    };
    inline const RangeInspectOperation* rangeInspectOperation(std::string_view name)
    {
        using enum RangeInspectResult;
        static constexpr std::array operations{
            RangeInspectOperation{"Range.start",Element,0},
            RangeInspectOperation{"Range.end",Element,1},
            RangeInspectOperation{"Range.step_magnitude",Magnitude,2},
            RangeInspectOperation{"Range.step",Magnitude,2},
            RangeInspectOperation{"Range.is_inclusive",Boolean,3},
            RangeInspectOperation{"Range.is_descending",Boolean,4},
            RangeInspectOperation{"Range.is_empty",Empty,0}
        };
        for (const auto& operation : operations) if (operation.name == name) return &operation;
        return nullptr;
    }
}
