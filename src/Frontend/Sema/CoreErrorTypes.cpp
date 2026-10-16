// ========================
//
// Created By : miruku_sora2026
//
// File name : CoreErrorTypes.cpp
//
// Description :
//     메모리 안의 Result와 Optional 열거형 선언 및 내장 함수 표식을 생성한다.
//     Create in-memory Result and Optional enum declarations and intrinsic markers.
//
// ========================

#include "aevix/Frontend/Sema/CoreErrorTypes.hpp"
#include "aevix/Frontend/Lexer/Lexer.hpp"
#include "aevix/Frontend/Parser/Parser.hpp"

namespace aevix
{
    bool installCoreErrorTypes(ModuleGraph& graph, SourceManager& sources, Diagnostics& diagnostics)
    {
        for (const auto& module : graph.modules) if (module && module->name == "$core") return true;
        if (!diagnostics.empty()) return false;
        const std::string source = R"(
public enum CollectionError { AllocationFailure; SizeOverflow; };
public enum RangeError { InvalidStep; CountOverflow; };
public enum InputError<T> {
    AllocationFailure { public T input; };
    SizeOverflow { public T input; };
};
public enum Result<T, E> {
    Ok { public T value; };
    Err { public E error; };
    public Result<T, E> ok(T value) {}
    public Result<T, E> err(E error) {}
    public bool is_ok(&Self value) {}
    public bool is_err(&Self value) {}
};
public enum Optional<T> {
    Some { public T value; };
    None;
    public Optional<T> some(T value) {}
    public Optional<T> none() {}
    public bool is_some(&Self value) {}
    public bool is_none(&Self value) {}
};
)";
        const auto id = sources.add("<aevix-core>", source);
        auto tokens = Lexer(sources.get(id), diagnostics).tokenize();
        if (!diagnostics.empty()) return false;
        auto ast = Parser(std::move(tokens), diagnostics).parseModule();
        if (!ast || !diagnostics.empty()) return false;
        const auto marker = [](std::string kind, std::string value, SourceRange range)
        {
            auto item = std::make_unique<Node>(); item->kind = std::move(kind); item->value = std::move(value); item->range = range; return item;
        };
        ast->children.push_back(marker("CompilerCore", "", ast->range));
        for (auto& declaration : ast->children)
        {
            if (declaration->kind != "Enum" || (declaration->value != "Result" && declaration->value != "Optional")) continue;
            declaration->children.push_back(marker("CompilerCarrier", declaration->value, declaration->range));
            for (auto& method : declaration->children)
                if (method->kind == "Function") method->children.push_back(marker("CompilerIntrinsic", "", method->range));
        }
        auto module = std::make_unique<SourceModule>();
        module->name = "$core"; module->path = "<aevix-core>"; module->package = "$core"; module->source = id; module->ast = std::move(ast);
        graph.modules.push_back(std::move(module));
        return true;
    }
}
