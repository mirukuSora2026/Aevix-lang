// ========================
//
// Created By : miruku_sora2026
//
// File name : SourceManager.hpp
//
// Description :
//     안정적인 소스 버퍼를 소유하고 줄 인덱스와 바이트 기준 위치 조회를 제공한다.
//     Own stable source buffers and provide line indexes and byte-based location lookup.
//
// ========================

#pragma once

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "aevix/Common/SourceLocation.hpp"

namespace aevix
{
    struct SourceFile final
    {
        SourceId id;
        std::string path;
        std::string text;
        std::vector<std::uint64_t> lines{0};
    };

    struct LineColumn final
    {
        std::uint64_t line;
        std::uint64_t column;
    };

    /// 소스 버퍼의 주소를 고정해 토큰의 string_view를 유효하게 유지한다.
    /// Stable source buffers keep token string_views valid.
    class SourceManager final
    {
        std::vector<std::unique_ptr<SourceFile>> files_;

    public:
        SourceId add(std::string path, std::string text)
        {
            if (files_.size() >= std::numeric_limits<SourceId>::max())
                throw std::length_error("too many source files.");

            const auto id = static_cast<SourceId>(files_.size() + 1);

            auto file = std::make_unique<SourceFile>(
                SourceFile{id, std::move(path), std::move(text)}
            );

            // CRLF는 한 줄바꿈으로, 단독 CR과 LF도 줄바꿈으로 센다.
            // Count CRLF as one newline and accept standalone CR and LF.
            for (std::size_t i = 0; i < file->text.size(); ++i)
            {
                if (file->text[i] == '\r')
                {
                    if (i + 1 < file->text.size() && file->text[i + 1] == '\n')
                        ++i;
                    file->lines.push_back(i + 1);
                }
                else if (file->text[i] == '\n')
                {
                    file->lines.push_back(i + 1);
                }
            }

            files_.push_back(std::move(file));
            return id;
        }

        [[nodiscard]] const SourceFile& get(SourceId id) const
        {
            if (id == InvalidSourceId || id > files_.size())
                throw std::out_of_range("Invalid source ID.");

            return *files_[id - 1];
        }

        [[nodiscard]] bool contains(SourceRange range) const noexcept
        {
            return range.isWellFormed()
                && range.begin.source <= files_.size()
                && range.end.offset
                <= files_[range.begin.source - 1]->text.size();
        }

        /// 열 번호도 바이트 기준이며 1부터 시작한다.
        /// Column numbers also count bytes and start at one.
        [[nodiscard]] LineColumn locate(SourceLocation location) const
        {
            const auto& file = get(location.source);

            if (location.offset > file.text.size())
                throw std::out_of_range("Source offset outside file.");

            const auto it = std::upper_bound(
                file.lines.begin(),
                file.lines.end(),
                location.offset
            );

            const auto index = static_cast<std::size_t>(
                it - file.lines.begin() - 1
            );

            return {
                index + 1,
                location.offset - file.lines[index] + 1
            };
        }
    };
}
