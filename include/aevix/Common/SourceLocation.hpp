// ========================
//
// Created By : miruku_sora2026
//
// File name : SourceLocation.hpp
//
// Description :
//     소스 버퍼 식별자, 바이트 위치 및 반열린 소스 범위를 정의한다.
//     Define source buffer identifiers, byte positions, and half-open source ranges.
//
// ========================

#pragma once

#include <cstdint>
#include <optional>

namespace aevix
{
    /// 소스 버퍼의 식별자이며, 실제 버퍼의 수명은 SourceManager가 관리한다.
    /// Identifies a source buffer whose lifetime is managed by SourceManager.
    using SourceId = std::uint32_t;

    /// 0번 식별자는 아직 소스가 지정되지 않은 기본 위치를 뜻한다.
    /// Identifier zero denotes a default location without an assigned source.
    inline constexpr SourceId InvalidSourceId = 0;

    /**
     * @brief 단일 소스 버퍼 안의 바이트 위치다.
     * A byte position within a single source buffer.
     *
     * offset은 문자 수나 화면에 표시되는 열 번호가 아니다.
     * The offset is not a character count or a display column.
     */
    struct SourceLocation final
    {
        SourceId source = InvalidSourceId;
        std::uint64_t offset = 0;

        /**
         * @brief 소스 식별자가 지정되었는지 검사한다.
         * Check whether a source identifier is assigned.
         * @return 식별자가 0이 아니면 true를 반환한다.
         * Return true when the identifier is nonzero.
         *
         * 실제 등록 여부와 버퍼 경계는 SourceManager가 검사한다.
         * SourceManager checks registration and buffer bounds.
         */
        [[nodiscard]] constexpr bool isValid() const noexcept
        {
            return source != InvalidSourceId;
        }
    };

    /**
     * @brief 같은 소스 안의 반열린 구간 [begin, end)이다.
     * A half-open interval [begin, end) within one source.
     *
     * end 위치의 바이트는 포함하지 않는다.
     * The byte at the end position is excluded.
     * begin == end인 빈 범위로 EOF나 삽입 위치를 표현할 수 있다.
     * An empty range with begin == end can represent EOF or an insertion point.
     */
    struct SourceRange final
    {
        SourceLocation begin{};
        SourceLocation end{};

        /// @brief 소스 식별자와 오프셋 순서가 구조적으로 올바른지 검사한다.
        /// Check structural consistency of source identifiers and offset order.
        [[nodiscard]] constexpr bool isWellFormed() const noexcept
        {
            return begin.isValid()
                && end.isValid()
                && begin.source == end.source
                && begin.offset <= end.offset;
        }

        /**
         * @brief 구조적으로 올바른 범위의 바이트 길이를 계산한다.
         * Calculate the byte length of a structurally valid range.
         * @return 범위가 올바르지 않으면 std::nullopt를 반환한다.
         * Return std::nullopt when the range is invalid.
         *
         * 부호 없는 뺄셈의 언더플로를 막기 위해 순서를 먼저 검사한다.
         * Check the order first to prevent unsigned subtraction underflow.
         * 실제 버퍼 경계까지 검증하는 계산은 아니다.
         * This calculation does not verify actual buffer bounds.
         */
        [[nodiscard]] constexpr std::optional<std::uint64_t>
        byteLength() const noexcept
        {
            if (!isWellFormed()) return std::nullopt;
            return end.offset - begin.offset;
        }
    };
}
