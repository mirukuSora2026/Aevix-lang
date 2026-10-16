// ========================
//
// Created By : miruku_sora2026
//
// File name : AST.hpp
//
// Description :
//     소스 범위, 주석 및 자식 노드를 보관하는 구문 트리와 노드 생성 함수를 정의한다.
//     Define syntax tree nodes with source ranges, comments, and children, plus their creation helper.
//
// ========================

#pragma once

#include "aevix/Common/SourceLocation.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aevix
{
    struct Node;
    using NodePtr = std::unique_ptr<Node>;

    /// 이름과 타입을 해석하기 전의 문법 트리이며 kind로 자식의 역할을 구분한다.
    /// This syntax tree precedes name and type resolution; kind identifies each child's role.
    struct Node final
    {
        std::string kind;
        std::string value;
        SourceRange range{};
        std::size_t height = 1;
        std::vector<std::string> comments;
        std::vector<NodePtr> children;
    };

    inline NodePtr makeNode(
        std::string kind,
        std::string value,
        SourceRange range = {}
    )
    {
        auto node = std::make_unique<Node>();
        node->kind = std::move(kind);
        node->value = std::move(value);
        node->range = range;
        return node;
    }
}
