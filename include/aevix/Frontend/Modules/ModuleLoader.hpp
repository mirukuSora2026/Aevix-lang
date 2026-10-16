// ========================
//
// Created By : miruku_sora2026
//
// File name : ModuleLoader.hpp
//
// Description :
//     모듈 검색 옵션과 소유 AST 그래프를 정의하고 모듈 로더 인터페이스를 선언한다.
//     Define module search options and the owning AST graph, and declare the module loader interface.
//
// ========================

#pragma once

#include "aevix/Common/Diagnostic.hpp"
#include "aevix/Common/SourceManager.hpp"
#include "aevix/Frontend/Parser/AST.hpp"

#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aevix
{
    using ModuleId = std::size_t;
    inline constexpr ModuleId InvalidModuleId = std::numeric_limits<ModuleId>::max();

    struct ModuleRoot final
    {
        std::filesystem::path directory;
        std::string prefix;
    };

    struct ModuleOptions final
    {
        std::filesystem::path sourceRoot;
        std::vector<ModuleRoot> searchRoots;
        std::optional<std::string> entrySource = std::nullopt;
    };

    struct SourceModule final
    {
        std::string name;
        std::filesystem::path path;
        std::string package;
        SourceId source = InvalidSourceId;
        NodePtr ast;
        std::vector<ModuleId> dependencies;
    };

    /// 그래프가 AST를 소유하며, 소스 버퍼는 SourceManager가 더 오래 보관한다.
    /// The graph owns ASTs; SourceManager must retain their source buffers longer.
    struct ModuleGraph final
    {
        std::vector<std::unique_ptr<SourceModule>> modules;
        ModuleId entry = InvalidModuleId;

        [[nodiscard]] ModuleId find(std::string_view name) const noexcept;
    };

    /// 가져온 모듈만 파싱하며, 선언 순환은 이미 등록한 모듈을 재사용한다.
    /// Parse only reachable imports and reuse registered modules for declaration cycles.
    class ModuleLoader final
    {
    public:
        ModuleLoader(SourceManager& sources, Diagnostics& diagnostics);
        [[nodiscard]] std::optional<ModuleGraph> load(
            const std::filesystem::path& entry,
            const ModuleOptions& options
        );

    private:
        SourceManager& sources_;
        Diagnostics& diagnostics_;
    };
}
