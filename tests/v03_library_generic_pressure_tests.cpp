// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_library_generic_pressure_tests.cpp
//
// Description :
//     반복 구체화와 AST 할당 교란에서도 실제 Vector 라이브러리의 내장 호출 계약을 보존하는지 검사한다.
//     Check builtin-call contracts in the actual Vector library across repeated specialization and AST allocation pressure.
//
// ========================

#include "aevix/Frontend/Lexer/Lexer.hpp"
#include "aevix/Frontend/Parser/Parser.hpp"
#include "aevix/Frontend/Sema/NameResolver.hpp"
#include "aevix/Frontend/Sema/CoreErrorTypes.hpp"
#include "aevix/Frontend/Sema/GenericResolver.hpp"
#include "aevix/Frontend/Sema/ControlFlowChecker.hpp"
#include "aevix/Frontend/Ownership/BorrowChecker.hpp"
#include "aevix/IR/Lowering/ASTLowering.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    std::string read(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return {};
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    bool check(const std::string& program, const std::string& library,
               std::size_t padding, std::size_t round)
    {
        aevix::SourceManager sources;
        aevix::Diagnostics diagnostics;
        aevix::ModuleGraph graph;
        const auto add = [&](const std::string& name, const std::string& source)
        {
            const auto id = sources.add("<pressure-" + name + ">", source);
            auto tokens = aevix::Lexer(sources.get(id), diagnostics).tokenize();
            auto module = std::make_unique<aevix::SourceModule>();
            module->name = name;
            module->path = "<pressure-" + name + ">";
            module->source = id;
            module->ast = aevix::Parser(std::move(tokens), diagnostics).parseModule();
            graph.modules.push_back(std::move(module));
        };
        add("main", program);
        add("vector", library);
        graph.entry = 0;
        graph.modules[0]->dependencies.push_back(1);
        if (diagnostics.empty() && aevix::installCoreErrorTypes(graph, sources, diagnostics))
        {
            auto names = aevix::NameResolver(diagnostics).resolve(graph);
            if (names.succeeded)
            {
                // 초기 이름 해석 후 다른 Node를 할당·해제해 임시 AST의 주소 배치를 바꾼다.
                // Perturb temporary AST placement after initial name resolution with released Node allocations.
                std::vector<aevix::NodePtr> pressure;
                for (std::size_t index = 0; index < padding * (round + 1); ++index)
                    pressure.push_back(std::make_unique<aevix::Node>());
                pressure.clear();
                auto concrete = aevix::GenericResolver(diagnostics).resolve(graph, names);
                if (concrete)
                {
                    names = std::move(*concrete);
                    auto types = aevix::TypeChecker(diagnostics).check(graph, names);
                    if (diagnostics.empty() && aevix::ControlFlowChecker(diagnostics).check(graph, names, types))
                    {
                        auto air = aevix::lowerToAIR(graph, names, types, diagnostics);
                        if (air && aevix::BorrowChecker(diagnostics).check(*air, names, types).succeeded &&
                            aevix::verifyAIR(*air, types.types, diagnostics)) return true;
                    }
                }
            }
        }
        std::cerr << "Valid Vector library source rejected at padding " << padding
                  << ", round " << round << ":\n";
        for (const auto& diagnostic : diagnostics) std::cerr << diagnostic.message << '\n';
        return false;
    }
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Expected the Aevix source directory.\n";
        return 1;
    }
    const std::filesystem::path root = argv[1];
    const auto fixture = read(root / "tests/v03_library_vector.avx");
    const auto library = read(root / "library/Collections/vector.avx");
    const std::string imported = "import vector;";
    if (fixture.empty() || library.empty() || !fixture.starts_with(imported))
    {
        std::cerr << "Cannot load the real Vector library and its execution fixture.\n";
        return 1;
    }
    unsigned checks = 0;
    for (const auto count : std::array<std::size_t, 7>{0, 1, 2, 4, 8, 16, 32})
    {
        std::string declarations;
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto number = std::to_string(index);
            declarations += "i32 pressure_value_" + number + "() { Optional<i32> item = Optional.some(" +
                            number + "); return Optional.is_some(&item) ? " + number + " : -1; }\n";
        }
        const auto program = imported + "\n" + declarations + fixture.substr(imported.size());
        for (std::size_t round = 0; round < 4; ++round)
        {
            if (!check(program, library, count, round)) return 1;
            ++checks;
        }
    }
    std::cout << checks << " valid source-library specialization/AIR checks passed across AST allocation variants.\n";
}
