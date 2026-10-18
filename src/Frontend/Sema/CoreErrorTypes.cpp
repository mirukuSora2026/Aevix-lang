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
        const std::string source = R"AEVIXDOC(/*
 * Lifecycle of the $core public module.
 * @module $core
 * @since 0.1
 * @deprecated_at none
 * @removed_at none
 */

/*
 * Version history for the $core.IntegerError public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
public enum IntegerError { /*
 * Version history for the $core.IntegerError.OutOfRange public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
OutOfRange; };
/*
 * Version history for the $core.FloatError public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
public enum FloatError { /*
 * Version history for the $core.FloatError.OutOfRange public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
OutOfRange; /*
 * Version history for the $core.FloatError.NonFinite public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
NonFinite; /*
 * Version history for the $core.FloatError.NonExact public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
NonExact; };
/*
 * Version history for the $core.CollectionError public API.
 * @since 0.1
 * @deprecated_at none
 * @removed_at none
 */
public enum CollectionError { /*
 * Version history for the $core.CollectionError.AllocationFailure public API.
 * @since 0.1
 * @deprecated_at none
 * @removed_at none
 */
AllocationFailure; /*
 * Version history for the $core.CollectionError.SizeOverflow public API.
 * @since 0.1
 * @deprecated_at none
 * @removed_at none
 */
SizeOverflow; };
/*
 * Version history for the $core.RangeError public API.
 * @since 0.1
 * @deprecated_at none
 * @removed_at none
 */
public enum RangeError { /*
 * Version history for the $core.RangeError.InvalidStep public API.
 * @since 0.1
 * @deprecated_at none
 * @removed_at none
 */
InvalidStep; /*
 * Version history for the $core.RangeError.CountOverflow public API.
 * @since 0.1
 * @deprecated_at none
 * @removed_at none
 */
CountOverflow; };
/*
 * Version history for the $core.BoundsError public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
public enum BoundsError { /*
 * Version history for the $core.BoundsError.OutOfBounds public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
OutOfBounds; /*
 * Version history for the $core.BoundsError.ReversedRange public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
ReversedRange; };
/*
 * Version history for the $core.TextError public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
public enum TextError { /*
 * Version history for the $core.TextError.AllocationFailure public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
AllocationFailure; /*
 * Version history for the $core.TextError.SizeOverflow public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
SizeOverflow; /*
 * Version history for the $core.TextError.InvalidUtf8 public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
InvalidUtf8; /*
 * Version history for the $core.TextError.InvalidBoundary public API.
 * @since 0.2
 * @deprecated_at none
 * @removed_at none
 */
InvalidBoundary; };
/*
 * A rejected Unicode scalar conversion, preserving its exact unsigned input.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum CharError implements Copy {
    /*
     * The input is a surrogate or exceeds U+10FFFF; noncharacters remain valid scalars.
     * @since 0.3
     * @deprecated_at none
     * @removed_at none
     */
    InvalidScalar {
        /*
         * The original rejected u32 without truncation or normalization.
         * @since 0.3
         * @deprecated_at none
         * @removed_at none
         */
        public u32 value;
    };
};
/*
 * Version history for the $core.ParseError public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum ParseError implements Copy { /*
 * Version history for the $core.ParseError.EmptyInput public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
EmptyInput; /*
 * Version history for the $core.ParseError.InvalidSyntax public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
InvalidSyntax; /*
 * Version history for the $core.ParseError.OutOfRange public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
OutOfRange; /*
 * Version history for the $core.ParseError.NonFinite public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
NonFinite; /*
 * Version history for the $core.ParseError.InvalidScalar public API.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
InvalidScalar; };
/*
 * Version history for the $core.InputError public API.
 * @since 0.1
 * @deprecated_at none
 * @removed_at none
 */
public enum InputError<T> {
    /*
     * Version history for the $core.InputError.AllocationFailure public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    AllocationFailure { /*
 * Version history for the $core.InputError.AllocationFailure.input public API.
 * @since 0.1
 * @deprecated_at none
 * @removed_at none
 */
public T input; };
    /*
     * Version history for the $core.InputError.SizeOverflow public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    SizeOverflow { /*
 * Version history for the $core.InputError.SizeOverflow.input public API.
 * @since 0.1
 * @deprecated_at none
 * @removed_at none
 */
public T input; };
    /*
     * Invalid insertion index; retain the unused input owner unchanged.
     * @since 0.3
     * @deprecated_at none
     * @removed_at none
     */
    OutOfBounds {
        /*
         * Original input not inserted into the collection.
         * @since 0.3
         * @deprecated_at none
         * @removed_at none
         */
        public T input;
    };
};
/*
 * Version history for the $core.Result public API.
 * @since 0.1
 * @deprecated_at none
 * @removed_at none
 */
public enum Result<T, E> {
    /*
     * Version history for the $core.Result.Ok public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    Ok { /*
 * Version history for the $core.Result.Ok.value public API.
 * @since 0.1
 * @deprecated_at none
 * @removed_at none
 */
public T value; };
    /*
     * Version history for the $core.Result.Err public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    Err { /*
 * Version history for the $core.Result.Err.error public API.
 * @since 0.1
 * @deprecated_at none
 * @removed_at none
 */
public E error; };
    /*
     * Version history for the $core.Result.ok public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    public Result<T, E> ok(T value) {}
    /*
     * Version history for the $core.Result.err public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    public Result<T, E> err(E error) {}
    /*
     * Version history for the $core.Result.is_ok public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    public bool is_ok(&Self value) {}
    /*
     * Version history for the $core.Result.is_err public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    public bool is_err(&Self value) {}
};
/*
 * Version history for the $core.Optional public API.
 * @since 0.1
 * @deprecated_at none
 * @removed_at none
 */
public enum Optional<T> {
    /*
     * Version history for the $core.Optional.Some public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    Some { /*
 * Version history for the $core.Optional.Some.value public API.
 * @since 0.1
 * @deprecated_at none
 * @removed_at none
 */
public T value; };
    /*
     * Version history for the $core.Optional.None public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    None;
    /*
     * Version history for the $core.Optional.some public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    public Optional<T> some(T value) {}
    /*
     * Version history for the $core.Optional.none public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    public Optional<T> none() {}
    /*
     * Version history for the $core.Optional.is_some public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    public bool is_some(&Self value) {}
    /*
     * Version history for the $core.Optional.is_none public API.
     * @since 0.1
     * @deprecated_at none
     * @removed_at none
     */
    public bool is_none(&Self value) {}
};
)AEVIXDOC";
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
