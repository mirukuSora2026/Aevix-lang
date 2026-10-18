// ========================
//
// Created By : miruku_sora2026
//
// File name : OsOperation.hpp
//
// Description :
//     신뢰된 표준 라이브러리에서만 사용할 수 있는 운영체제 호출의 닫힌 서명을 정의한다.
//     Define the closed signatures of OS operations available only to trusted standard modules.
//
// ========================

#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace aevix
{
    enum class OsValueKind { U64, U32, I64, Usize, Text, Bytes, MutableBytesLoan, Arguments };
    enum class OsResultKind { Progress, Text, Exit, Metadata, SignedTime };

    struct OsOperation final
    {
        std::string_view name;
        std::array<OsValueKind, 3> parameters;
        std::size_t parameterCount;
        bool tokenResult = false;
        bool unitResult = false;
        OsResultKind payload = OsResultKind::Progress;
        std::string_view module = "io";
    };

    // 서명은 소스의 함수 이름이나 주석으로 확장할 수 없다.
    // Source names and comments cannot extend this operation registry.
    inline const OsOperation* osOperation(std::string_view name) noexcept
    {
        static constexpr std::array operations{
            OsOperation{"file_open", {OsValueKind::Text, OsValueKind::U32}, 2, true},
            OsOperation{"file_read", {OsValueKind::U64, OsValueKind::MutableBytesLoan}, 2},
            OsOperation{"file_write", {OsValueKind::U64, OsValueKind::Bytes}, 2},
            OsOperation{"file_seek", {OsValueKind::U64, OsValueKind::I64, OsValueKind::U32}, 3},
            OsOperation{"file_flush", {OsValueKind::U64}, 1},
            OsOperation{"file_close", {OsValueKind::U64}, 1},
            OsOperation{"file_size", {OsValueKind::U64}, 1},
            OsOperation{"file_set_len", {OsValueKind::U64, OsValueKind::Usize}, 2},
            OsOperation{"file_cleanup", {OsValueKind::U64}, 1, false, true},
            OsOperation{"env_get", {OsValueKind::Text}, 1, false, false, OsResultKind::Text, "env"},
            OsOperation{"env_set", {OsValueKind::Text, OsValueKind::Text}, 2, false, false, OsResultKind::Progress, "env"},
            OsOperation{"env_remove", {OsValueKind::Text}, 1, false, false, OsResultKind::Progress, "env"},
            OsOperation{"monotonic_ns", {}, 0, true, false, OsResultKind::Progress, "time"},
            OsOperation{"unix_time_ns", {}, 0, false, false, OsResultKind::SignedTime, "time"},
            OsOperation{"sleep_ns", {OsValueKind::U64}, 1, false, false, OsResultKind::Progress, "time"},
            OsOperation{"path_normalize", {OsValueKind::Text}, 1, false, false, OsResultKind::Text, "path"},
            OsOperation{"path_metadata", {OsValueKind::Text, OsValueKind::U32}, 2, false, false, OsResultKind::Metadata, "fs"},
            OsOperation{"path_mkdir", {OsValueKind::Text}, 1, false, false, OsResultKind::Progress, "fs"},
            OsOperation{"path_remove", {OsValueKind::Text}, 1, false, false, OsResultKind::Progress, "fs"},
            OsOperation{"process_spawn", {OsValueKind::Arguments}, 1, true, false, OsResultKind::Progress, "process"},
            OsOperation{"process_wait", {OsValueKind::U64}, 1, false, false, OsResultKind::Exit, "process"},
            OsOperation{"process_try_wait", {OsValueKind::U64}, 1, false, false, OsResultKind::Exit, "process"},
            OsOperation{"process_kill", {OsValueKind::U64}, 1, false, false, OsResultKind::Progress, "process"},
            OsOperation{"process_cleanup", {OsValueKind::U64}, 1, false, true, OsResultKind::Progress, "process"},
        };
        for (const auto& operation : operations)
            if (operation.name == name) return &operation;
        return nullptr;
    }
}
