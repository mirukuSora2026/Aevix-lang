// ========================
//
// Created By : miruku_sora2026
//
// File name : Diagnostic.hpp
//
// Description :
//     소스 범위와 오류 메시지를 보관하는 진단 값 및 진단 목록을 정의한다.
//     Define diagnostic values and collections that retain source ranges and error messages.
//
// ========================

#pragma once

#include "aevix/Common/SourceLocation.hpp"

#include <string>
#include <vector>

namespace aevix
{
    /// 오류를 값으로 모으고, 출력 형식은 드라이버가 결정한다.
    /// Collect errors as values; the driver chooses their presentation.
    struct Diagnostic final
    {
        SourceRange range;
        std::string message;
    };

    using Diagnostics = std::vector<Diagnostic>;

}
