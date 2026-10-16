// ========================
//
// Created By : miruku_sora2026
//
// File name : NumericLiteral.hpp
//
// Description :
//     숫자 리터럴의 명시적 타입 접미사와 값 부분을 분리한다.
//     Separate explicit numeric type suffixes from the value spelling.
//
// ========================
#pragma once
#include <string_view>
#include <initializer_list>
namespace aevix
{
    inline std::string_view numericSuffix(std::string_view text)
    {
        const bool prefixed = text.starts_with("0x") || text.starts_with("0X") || text.starts_with("0b") || text.starts_with("0B") || text.starts_with("0o") || text.starts_with("0O");
        for (const auto suffix : {"isize", "usize", "i128", "u128", "bf16", "i16", "i32", "i64", "u16", "u32", "u64", "f16", "f32", "f64", "i8", "u8"})
        {
            const auto name = std::string_view(suffix);
            if (prefixed && (name.front() == 'f' || name == "bf16")) continue;
            if (text.size() > name.size() && text.ends_with(name)) return name;
        }
        return {};
    }
    inline std::string_view numericSpelling(std::string_view text)
    {
        return text.substr(0, text.size() - numericSuffix(text).size());
    }
    inline bool numericFloating(std::string_view text)
    {
        const auto suffix = numericSuffix(text);
        if (!suffix.empty()) return suffix.front() == 'f' || suffix == "bf16";
        if (text.starts_with("0x") || text.starts_with("0X") || text.starts_with("0b") || text.starts_with("0B") || text.starts_with("0o") || text.starts_with("0O")) return false;
        return text.find_first_of(".eE") != std::string_view::npos;
    }
}
