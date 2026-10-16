// ========================
//
// Created By : miruku_sora2026
//
// File name : Parser.cpp
//
// Description :
//     선언, 타입, 문장 및 우선순위 기반 표현식을 분석하고 오류 복구와 중첩 깊이를 관리한다.
//     Parse declarations, types, statements, and precedence-based expressions with error recovery and nesting limits.
//
// ========================

#include "aevix/Frontend/Parser/Parser.hpp"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace aevix
{
    namespace
    {
        struct Failure
        {
            std::size_t position;
            std::string message;
        };

        struct Fork
        {
            std::size_t position;
        };

        using Choices = std::map<std::size_t, bool>;

        class Engine final
        {
        public:
            Engine(
                const std::vector<Token>& tokens,
                Diagnostics& diagnostics
            )
                : tokens_(tokens),
                  diagnostics_(diagnostics)
            {
            }

            NodePtr module()
            {
                auto result = makeNode("Module", "");
                bool declarationStarted = false;

                while (!atEnd())
                {
                    const auto before = position_;

                    try
                    {
                        const bool importing =
                            is("import")
                            || (is("public") && is("import", 1));

                        if (importing)
                        {
                            if (declarationStarted)
                                fail("Imports must precede other declarations.");

                            attach(*result, importDeclaration());
                        }
                        else
                        {
                            declarationStarted = true;
                            attach(*result, declaration(false, false));
                        }
                    }
                    catch (const Failure& error)
                    {
                        report(error);
                        recover(false);
                    }

                    if (position_ == before && !atEnd())
                        ++position_;
                }
                result->range = span(0);
                return result;
            }

        private:
            const std::vector<Token>& tokens_;
            Diagnostics& diagnostics_;

            std::size_t position_ = 0;
            std::size_t depth_ = 0;

            Choices* choices_ = nullptr;
            bool structures_ = true;
            bool genericDefault_ = false;

            struct BooleanScope final
            {
                bool& value;
                bool previous;
                BooleanScope(bool& target, bool replacement) : value(target), previous(target) { value = replacement; }
                ~BooleanScope() { value = previous; }
            };

            struct ChoicesScope final
            {
                Choices*& value;
                Choices* previous;
                explicit ChoicesScope(Choices*& target) : value(target), previous(target) { value = nullptr; }
                ~ChoicesScope() { value = previous; }
            };

            struct Depth final
            {
                Engine& engine;

                explicit Depth(Engine& value)
                    : engine(value)
                {
                    if (++engine.depth_ > 256)
                    {
                        --engine.depth_;
                        engine.fail("Syntax nesting exceeds the limit of 256.");
                    }
                }

                ~Depth()
                {
                    --engine.depth_;
                }
            };

            const Token& token(std::size_t lookahead = 0) const
            {
                return tokens_[
                    std::min(position_ + lookahead, tokens_.size() - 1)
                ];
            }

            bool atEnd() const
            {
                return token().kind == TokenKind::End;
            }

            bool is(
                std::string_view text,
                std::size_t lookahead = 0
            ) const
            {
                return token(lookahead).text == text;
            }

            bool take(std::string_view text)
            {
                if (!is(text)) return false;
                ++position_;
                return true;
            }

            [[noreturn]] void fail(std::string message) const
            {
                throw Failure{position_, std::move(message)};
            }

            void expect(std::string_view text)
            {
                if (!take(text))
                    fail("Expected '" + std::string(text) + "'.");
            }

            std::string name()
            {
                static const std::set<std::string_view> reserved = {
                    "import", "public", "private",
                    "struct", "enum", "interface", "type",
                    "implements", "where", "override",
                    "mut", "const", "persist", "rsn", "dyn", "extern",
                    "if", "else", "while", "for", "in",
                    "switch", "case", "default",
                    "break", "continue", "return", "move",
                    "true", "false"
                };

                if (
                    token().kind != TokenKind::Identifier
                    || reserved.contains(token().text)
                )
                {
                    fail("Expected an identifier.");
                }

                return std::string(tokens_[position_++].text);
            }

            SourceRange span(std::size_t start) const
            {
                const auto first = std::min(start, tokens_.size() - 1);

                if (position_ <= first)
                    return {
                        tokens_[first].range.begin,
                        tokens_[first].range.begin
                    };

                return {
                    tokens_[first].range.begin,
                    tokens_[position_ - 1].range.end
                };
            }

            NodePtr node(
                std::string kind,
                std::string value,
                std::size_t start
            ) const
            {
                return makeNode(
                    std::move(kind),
                    std::move(value),
                    span(start)
                );
            }

            // 반복으로 만든 체인도 AST 깊이를 제한해 안전하게 소멸시킨다.
            // Bound AST depth even for iterative chains, keeping recursive destruction safe.
            void attach(Node& owner, NodePtr child)
            {
                if (child->height >= 256)
                    fail("Syntax tree depth exceeds the limit of 256.");

                owner.height = std::max(owner.height, child->height + 1);

                if (!owner.range.isWellFormed())
                {
                    owner.range = child->range;
                }
                else if (
                    owner.range.begin.source == child->range.begin.source
                )
                {
                    owner.range.begin.offset = std::min(
                        owner.range.begin.offset,
                        child->range.begin.offset
                    );

                    owner.range.end.offset = std::max(
                        owner.range.end.offset,
                        child->range.end.offset
                    );
                }

                owner.children.push_back(std::move(child));
            }

            void comments(Node& result, std::size_t start) const
            {
                for (auto comment : tokens_[start].comments)
                    result.comments.emplace_back(comment);
            }

            void report(const Failure& error)
            {
                const auto index = std::min(
                    error.position,
                    tokens_.size() - 1
                );

                diagnostics_.push_back({
                    tokens_[index].range,
                    error.message
                });
            }

            void recover(bool retainClosingBrace)
            {
                // 복구 중에는 토큰을 소비하여 같은 오류에 머무르지 않는다.
                // Recovery consumes tokens so it cannot remain stuck on the same error.
                unsigned braces = 0;

                while (!atEnd())
                {
                    if (is("}") && braces == 0)
                    {
                        if (!retainClosingBrace) ++position_;
                        return;
                    }

                    if (take("{"))
                    {
                        ++braces;
                        continue;
                    }

                    if (take("}"))
                    {
                        --braces;
                        continue;
                    }

                    if (take(";") && braces == 0)
                        return;

                    ++position_;
                }
            }

            bool adjacent(std::size_t left, std::size_t right) const
            {
                return tokens_[left].range.end.source
                    == tokens_[right].range.begin.source
                    && tokens_[left].range.end.offset
                    == tokens_[right].range.begin.offset;
            }

            std::string modifiers()
            {
                std::set<std::string> seen;
                std::string result;

                while (
                    is("public") || is("private") || is("override")
                    || is("mut") || is("const") || is("persist") || is("extern")
                )
                {
                    std::string value(token().text);

                    if (!seen.insert(value).second)
                        fail("Repeated declaration modifier.");

                    if (
                        (value == "public" && seen.contains("private"))
                        || (value == "private" && seen.contains("public"))
                    )
                    {
                        fail("A declaration cannot be both public and private.");
                    }

                    if (!result.empty()) result += ' ';
                    result += value;
                    ++position_;
                    if (value == "extern") expect("\"C\"");
                }

                return result;
            }

            std::string path()
            {
                auto value = name();

                while (take("."))
                    value += "." + name();

                return value;
            }

            NodePtr importDeclaration()
            {
                const auto start = position_;
                const bool exposed = take("public");

                expect("import");

                auto result = node("Import", path(), start);

                if (take("{"))
                {
                    if (take("*"))
                    {
                        if (exposed)
                            fail("Wildcard re-exports are not supported.");

                        attach(
                            *result,
                            node("ImportName", "*", position_ - 1)
                        );
                    }
                    else
                    {
                        do
                        {
                            const auto itemStart = position_;

                            attach(
                                *result,
                                node("ImportName", name(), itemStart)
                            );
                        }
                        while (take(","));
                    }

                    expect("}");
                }
                else if (exposed)
                {
                    fail("Re-exports must explicitly list the exported names.");
                }

                expect(";");

                if (exposed)
                    attach(*result, node("Modifier", "public", start));

                result->range = span(start);
                comments(*result, start);

                return result;
            }

            NodePtr type()
            {
                Depth depth(*this);
                const auto start = position_;

                if (take("&"))
                {
                    const bool writable = take("mut");

                    auto result = node(
                        "ReferenceType",
                        writable ? "mut" : "read",
                        start
                    );

                    attach(*result, type());
                    result->range = span(start);
                    return result;
                }

                if (take("dyn"))
                {
                    auto result = node("DynamicType", "", start);
                    attach(*result, type());
                    result->range = span(start);
                    return result;
                }

                auto result = node("Type", name(), start);
                genericTypeArguments(*result);

                while (take("."))
                {
                    auto qualified = node("QualifiedType", name(), start);

                    attach(*qualified, std::move(result));
                    genericTypeArguments(*qualified);

                    result = std::move(qualified);
                }

                result->range = span(start);
                return result;
            }

            void genericTypeArguments(Node& owner)
            {
                if (!take("<")) return;
                if (owner.value == "Tuple" && take(">")) return;

                do
                {
                    attach(owner, typeArgument());
                }
                while (take(",") && !is(">"));

                expect(">");
            }

            NodePtr genericParameters()
            {
                const auto start = position_;
                auto result = node("GenericParameters", "", start);

                if (!take("<")) return result;

                bool defaultSeen = false;
                do
                {
                    const auto parameterStart = position_;
                    NodePtr parameter;
                    if (take("const"))
                    {
                        auto parameterType = type();
                        parameter = node("ConstParameter", name(), parameterStart);
                        attach(*parameter, std::move(parameterType));
                    }
                    else parameter = node("TypeParameter", name(), parameterStart);
                    if (take("="))
                    {
                        auto initial = node("GenericDefault", "", position_ - 1);
                        if (parameter->kind == "ConstParameter")
                        {
                            BooleanScope delimiter(genericDefault_, true);
                            attach(*initial, expression({",", ">"}));
                        }
                        else attach(*initial, type());
                        attach(*parameter, std::move(initial));
                        defaultSeen = true;
                    }
                    else if (defaultSeen) fail("A required generic parameter cannot follow a defaulted parameter.");
                    parameter->range = span(parameterStart);
                    attach(*result, std::move(parameter));
                }
                while (take(",") && !is(">"));

                expect(">");

                result->range = span(start);
                return result;
            }

            void constraints(Node& declarationNode)
            {
                if (take("implements"))
                {
                    auto list = node("Implements", "", position_ - 1);

                    do
                    {
                        attach(*list, type());
                    }
                    while (take(","));

                    attach(declarationNode, std::move(list));
                }

                if (take("where"))
                {
                    do
                    {
                        const auto start = position_;
                        auto requirement = node("Constraint", "", start);

                        attach(*requirement, type());
                        expect("implements");
                        attach(*requirement, type());

                        requirement->range = span(start);

                        attach(declarationNode, std::move(requirement));
                    }
                    while (take(","));
                }
            }

            NodePtr suffixes()
            {
                const auto start = position_;
                auto result = node("ArraySuffixes", "", start);

                while (take("["))
                {
                    const auto suffixStart = position_ - 1;
                    auto suffix = node("ArraySuffix", "", suffixStart);

                    if (!is("]"))
                        attach(*suffix, expression({"]"}));

                    expect("]");

                    if (take("("))
                    {
                        if (
                            !suffix->children.empty()
                            || !result->children.empty()
                        )
                        {
                            fail("Rank shorthand must stand alone as name[](rank).");
                        }

                        suffix->kind = "RankSuffix";
                        attach(*suffix, expression({")"}));
                        expect(")");

                        if (is("["))
                        {
                            fail("Rank shorthand cannot be combined with other array suffixes.");
                        }
                    }

                    suffix->range = span(suffixStart);
                    attach(*result, std::move(suffix));
                }

                result->range = span(start);
                return result;
            }

            NodePtr parameter()
            {
                const auto start = position_;
                const bool writable = take("mut");

                auto parameterType = type();
                auto result = node("Parameter", name(), start);

                attach(*result, std::move(parameterType));

                auto arrays = suffixes();

                if (!arrays->children.empty())
                {
                    bool empty =
                        arrays->children.size() == 1
                        && arrays->children[0]->kind == "ArraySuffix"
                        && arrays->children[0]->children.empty();

                    if (empty)
                    {
                        if (writable)
                            fail("A variadic pack binding cannot be mut.");

                        result->kind = "VariadicParameter";
                    }
                    else
                    {
                        for (const auto& suffix : arrays->children)
                        {
                            if (
                                suffix->kind != "ArraySuffix"
                                || suffix->children.empty()
                            )
                            {
                                fail("Use canonical collection types for nested dynamic array parameters.");
                            }
                        }
                    }

                    attach(*result, std::move(arrays));
                }

                if (writable)
                    attach(*result, node("Modifier", "mut", start));

                if (take("="))
                {
                    if (result->kind == "VariadicParameter"
                        || result->value == "self"
                    )
                    {
                        fail("self and variadic parameters cannot have defaults.");
                    }

                    auto initial = node("Default", "", position_ - 1);
                    attach(*initial, expression({",", ")"}));
                    attach(*result, std::move(initial));
                }

                result->range = span(start);
                return result;
            }

            NodePtr declaration(
                bool local,
                bool interfaceMember,
                bool enumMember = false
            )
            {
                Depth depth(*this);

                const auto start = position_;
                NodePtr bench;
                while (take("@"))
                {
                    const auto annotationStart = position_ - 1;
                    const auto annotation = name();
                    if (annotation != "bench")
                        fail("Unknown function annotation '@" + annotation + "'.");
                    if (bench) fail("Repeated @bench annotation.");
                    bench = node("Bench", "", annotationStart);
                }
                const auto flags = modifiers();

                if (
                    local
                    && (
                        flags.find("public") != std::string::npos
                        || flags.find("private") != std::string::npos
                        || flags.find("override") != std::string::npos
                    )
                )
                {
                    fail("Visibility and override modifiers are not allowed on local declarations.");
                }

                if (take("type"))
                {
                    if (bench) fail("@bench only applies to functions with a body.");
                    if (local)
                        fail("Type aliases belong at module or type scope.");
                    if (!flags.empty() && flags != "public" && flags != "private")
                        fail("Type aliases only accept public or private visibility.");
                    auto result = node("TypeAlias", name(), start);
                    expect("=");
                    attach(*result, type());
                    expect(";");
                    if (!flags.empty())
                        attach(*result, node("Modifier", flags, start));
                    result->range = span(start);
                    comments(*result, start);
                    return result;
                }

                if (is("struct") || is("enum") || is("interface"))
                {
                    if (bench) fail("@bench only applies to functions with a body.");
                    if (flags.find("extern") != std::string::npos)
                        fail("extern C declarations must be module-level function prototypes.");
                    const std::string category(tokens_[position_++].text);

                    if (local)
                        fail("Type declarations belong at module or type scope.");

                    auto result = node(
                        category == "struct"
                            ? "Struct"
                            : category == "enum"
                            ? "Enum"
                            : "Interface",
                        name(),
                        start
                    );

                    attach(*result, genericParameters());
                    constraints(*result);
                    expect("{");

                    while (!is("}") && !atEnd())
                    {
                        attach(
                            *result,
                            declaration(
                                false,
                                category == "interface",
                                category == "enum"
                            )
                        );
                    }

                    expect("}");
                    expect(";");

                    if (!flags.empty())
                        attach(*result, node("Modifier", flags, start));

                    result->range = span(start);
                    comments(*result, start);

                    return result;
                }

                if (
                    enumMember
                    && token().kind == TokenKind::Identifier
                    && (is(";", 1) || is("{", 1))
                )
                {
                    if (bench) fail("@bench only applies to functions with a body.");
                    auto result = node("Variant", name(), start);

                    if (take("{"))
                    {
                        while (!is("}") && !atEnd())
                            attach(*result, declaration(false, false));

                        expect("}");
                    }

                    expect(";");

                    if (!flags.empty())
                        attach(*result, node("Modifier", flags, start));

                    result->range = span(start);
                    comments(*result, start);

                    return result;
                }

                const bool inferred = take("rsn");

                auto declaredType = inferred
                                        ? node("InferredType", "rsn", position_ - 1)
                                        : type();

                auto result = node("Variable", name(), start);
                attach(*result, std::move(declaredType));

                if (is("<") || is("("))
                {
                    if (inferred)
                        fail("Functions must explicitly declare their return type.");

                    if (local)
                        fail("Nested function declarations are not supported.");

                    result->kind = "Function";

                    attach(*result, genericParameters());
                    expect("(");

                    bool defaultSeen = false;
                    bool variadicSeen = false;

                    auto parameters = node("Parameters", "", position_ - 1);
                    if (!is(")"))
                    {
                        do
                        {
                            if (variadicSeen)
                                fail("The variadic parameter must be last.");

                            auto item = parameter();

                            const bool hasDefault = std::any_of(
                                item->children.begin(),
                                item->children.end(),
                                [](const auto& child)
                                {
                                    return child->kind == "Default";
                                }
                            );

                            if (item->kind == "VariadicParameter")
                            {
                                if (defaultSeen)
                                {
                                    fail(
                                        "Defaulted parameters cannot precede a variadic pack."
                                    );
                                }

                                variadicSeen = true;
                            }
                            else if (defaultSeen && !hasDefault)
                            {
                                fail(
                                    "A required parameter cannot follow a defaulted parameter."
                                );
                            }

                            defaultSeen = defaultSeen || hasDefault;
                            attach(*parameters, std::move(item));
                        }
                        while (take(",") && !is(")"));
                    }

                    expect(")");
                    parameters->range.end = tokens_[position_ - 1].range.end;

                    attach(*result, std::move(parameters));
                    constraints(*result);

                    if (take(";"))
                    {
                        if (bench) fail("@bench requires a function body; extern and abstract declarations cannot be benchmarked.");
                        if (!interfaceMember && flags.find("extern") == std::string::npos)
                        {
                            fail("Only interface requirements may omit a function body.");
                        }
                    }
                    else
                    {
                        if (flags.find("extern") != std::string::npos)
                            fail("An extern C function must not have a body.");
                        attach(*result, block());
                    }
                }
                else
                {
                    if (bench) fail("@bench only applies to functions with a body.");
                    if (flags.find("extern") != std::string::npos)
                        fail("extern C declarations must be module-level function prototypes.");
                    attach(*result, suffixes());

                    if (take("="))
                    {
                        attach(*result, expression({";"}));
                    }
                    else if (
                        inferred
                        || flags.find("const") != std::string::npos
                        || flags.find("persist") != std::string::npos
                    )
                    {
                        fail(
                            "rsn, const, and persist declarations require an initializer."
                        );
                    }

                    expect(";");
                }

                if (!flags.empty())
                    attach(*result, node("Modifier", flags, start));
                if (bench) attach(*result, std::move(bench));

                result->range = span(start);
                comments(*result, start);

                return result;
            }

            bool beginsDeclaration()
            {
                if (
                    is("mut") || is("const") || is("persist") || is("rsn") || is("extern") || is("@")
                    || is("public") || is("private")
                    || is("struct") || is("enum") || is("interface") || is("type")
                )
                {
                    return true;
                }

                const auto saved = position_;

                try
                {
                    auto ignored = type();
                    const bool result = token().kind == TokenKind::Identifier;
                    position_ = saved;
                    return result;
                }
                catch (const Failure&)
                {
                    position_ = saved;
                    return false;
                }
            }

            NodePtr block()
            {
                Depth depth(*this);

                const auto start = position_;
                expect("{");

                auto result = node("Block", "", start);

                while (!is("}") && !atEnd())
                {
                    const auto before = position_;

                    try
                    {
                        attach(*result, statement());
                    }
                    catch (const Failure& error)
                    {
                        report(error);
                        recover(true);
                    }

                    if (
                        position_ == before
                        && !atEnd()
                        && !is("}")
                    )
                    {
                        ++position_;
                    }
                }

                expect("}");

                result->range = span(start);
                return result;
            }

            NodePtr body()
            {
                if (is("{")) return block();

                if (beginsDeclaration())
                    fail("A declaration used as a control body requires braces.");

                return statement();
            }

            std::string assignmentOperator() const
            {
                if (
                    is("=") || is("+=") || is("-=")
                    || is("*=") || is("/=") || is("%=")
                    || is("&=") || is("|=") || is("^=")
                )
                {
                    return std::string(token().text);
                }

                if (
                    (is("<") && is("<", 1))
                    || (is(">") && is(">", 1))
                )
                {
                    if (
                        is("=", 2)
                        && adjacent(position_, position_ + 1)
                        && adjacent(position_ + 1, position_ + 2)
                    )
                    {
                        return is("<") ? "<<=" : ">>=";
                    }
                }

                return {};
            }

            NodePtr action(std::string_view terminator)
            {
                const auto start = position_;

                auto result = expression({
                    terminator,
                    "=", "+=", "-=", "*=", "/=", "%=",
                    "&=", "|=", "^=", "<", ">"
                });

                const auto operation = assignmentOperator();

                if (!operation.empty())
                {
                    position_ += operation == "<<=" || operation == ">>="
                                     ? 3
                                     : 1;

                    auto assignment = node("Assignment", operation, start);

                    attach(*assignment, std::move(result));
                    attach(*assignment, expression({terminator}));

                    assignment->range = span(start);
                    return assignment;
                }

                auto statementNode = node("ExpressionStatement", "", start);
                attach(*statementNode, std::move(result));

                return statementNode;
            }

            NodePtr statement()
            {
                Depth depth(*this);
                const auto start = position_;

                if (is("{")) return block();
                if (take(";")) return node("Empty", "", start);

                if (take("if"))
                {
                    expect("(");

                    auto result = node("If", "", start);

                    attach(*result, expression({")"}));
                    expect(")");
                    attach(*result, body());

                    if (take("else"))
                        attach(*result, body());

                    result->range = span(start);
                    return result;
                }

                if (take("while"))
                {
                    expect("(");

                    auto result = node("While", "", start);

                    attach(*result, expression({")"}));
                    expect(")");
                    attach(*result, body());

                    result->range = span(start);
                    return result;
                }

                if (take("for"))
                    return forStatement(start);

                if (take("switch"))
                    return switchStatement(start);

                if (take("return"))
                {
                    auto result = node("Return", "", start);

                    if (!is(";"))
                        attach(*result, expression({";"}));

                    expect(";");

                    result->range = span(start);
                    return result;
                }

                if (is("break") || is("continue"))
                {
                    const bool breaking = take("break");

                    if (!breaking)
                        expect("continue");

                    expect(";");

                    return node(
                        breaking ? "Break" : "Continue",
                        "",
                        start
                    );
                }

                if (beginsDeclaration())
                    return declaration(true, false);

                auto result = action(";");
                expect(";");

                result->range = span(start);
                return result;
            }

            NodePtr forStatement(std::size_t start)
            {
                expect("(");
                const auto saved = position_;

                // 타입이 있는 반복 변수를 C 형태의 초기화 선언보다 먼저 판별한다.
                // Detect a typed iteration binding before a C-style initializer declaration.
                try
                {
                    const bool writable = take("mut");

                    auto boundType = take("rsn")
                                         ? node("InferredType", "rsn", position_ - 1)
                                         : type();

                    auto binding = node("Binding", name(), saved);

                    attach(*binding, std::move(boundType));

                    if (writable)
                        attach(*binding, node("Modifier", "mut", saved));

                    if (is("in") || is(":"))
                    {
                        const bool range = take("in");

                        if (!range)
                            expect(":");

                        auto result = node(
                            range ? "RangeFor" : "EnhancedFor",
                            "",
                            start
                        );

                        attach(*result, std::move(binding));
                        attach(*result, expression({")"}));

                        expect(")");
                        attach(*result, body());

                        result->range = span(start);
                        return result;
                    }
                }
                catch (const Failure&)
                {
                }

                position_ = saved;

                auto result = node("For", "", start);

                if (take(";"))
                {
                    attach(*result, node("Empty", "", position_ - 1));
                }
                else if (beginsDeclaration())
                {
                    attach(*result, declaration(true, false));
                }
                else
                {
                    attach(*result, action(";"));
                    expect(";");
                }

                if (is(";"))
                    attach(*result, node("Empty", "", position_));
                else
                    attach(*result, expression({";"}));

                expect(";");

                if (is(")"))
                    attach(*result, node("Empty", "", position_));
                else
                    attach(*result, action(")"));

                expect(")");
                attach(*result, body());

                result->range = span(start);
                return result;
            }

            NodePtr switchStatement(std::size_t start)
            {
                expect("(");

                const bool consuming = take("move");

                auto result = node(
                    "Switch",
                    consuming ? "move" : "read-or-borrow",
                    start
                );

                attach(*result, expression({")"}));

                expect(")");
                expect("{");

                bool defaultSeen = false;

                while (!is("}") && !atEnd())
                {
                    const auto caseStart = position_;
                    NodePtr branch;

                    if (take("default"))
                    {
                        if (defaultSeen)
                            fail("A switch can contain only one default branch.");

                        defaultSeen = true;
                        branch = node("Default", "", caseStart);
                    }
                    else
                    {
                        expect("case");
                        branch = node("Case", "", caseStart);

                        const bool previous = structures_;
                        structures_ = false;

                        try
                        {
                            attach(*branch, expression({":", "{"}));
                        }
                        catch (...)
                        {
                            structures_ = previous;
                            throw;
                        }

                        structures_ = previous;

                        if (take("{"))
                        {
                            auto bindings = node(
                                "PatternBindings",
                                "",
                                position_ - 1
                            );

                            if (!is("}"))
                            {
                                do
                                {
                                    const auto bindingStart = position_;

                                    auto bindingType = type();
                                    auto binding = node(
                                        "PatternBinding",
                                        name(),
                                        bindingStart
                                    );

                                    attach(*binding, std::move(bindingType));
                                    expect("=");

                                    std::string source;

                                    if (take("&"))
                                        source = take("mut") ? "&mut " : "&";

                                    source += name();

                                    attach(
                                        *binding,
                                        node("PatternField", source, position_ - 1)
                                    );

                                    attach(*bindings, std::move(binding));
                                }
                                while (take(","));
                            }

                            expect("}");
                            attach(*branch, std::move(bindings));
                        }
                    }

                    expect(":");

                    auto scope = node("CaseScope", "", position_);

                    while (
                        !is("case")
                        && !is("default")
                        && !is("}")
                        && !atEnd()
                    )
                    {
                        attach(*scope, statement());
                    }

                    if (
                        scope->children.empty()
                        || scope->children.back()->kind != "Break"
                    )
                    {
                        fail("Every case/default must end with an explicit 'break;'.");
                    }

                    attach(*branch, std::move(scope));
                    branch->range = span(caseStart);
                    attach(*result, std::move(branch));
                }

                expect("}");

                result->range = span(start);
                return result;
            }

            struct Operator
            {
                std::string text;
                int precedence = 0;
                unsigned count = 1;
            };

            Operator binaryOperator() const
            {
                const std::string value(token().text);

                if (value == "<" || value == ">")
                {
                    if (is(value, 1) && adjacent(position_, position_ + 1))
                    {
                        if (
                            is("=", 2)
                            && adjacent(position_ + 1, position_ + 2)
                        )
                        {
                            return {};
                        }

                        return {value + value, 10, 2};
                    }

                    if (is("=", 1) && adjacent(position_, position_ + 1))
                        return {value + "=", 9, 2};

                    return {value, 9};
                }

                if (value == "*" || value == "/" || value == "%")
                    return {value, 12};

                if (value == "+" || value == "-")
                    return {value, 11};

                if (value == "==" || value == "!=")
                    return {value, 8};

                if (value == "&") return {value, 7};
                if (value == "^") return {value, 6};
                if (value == "|") return {value, 5};
                if (value == "&&") return {value, 4};
                if (value == "||") return {value, 3};

                if (value == ".." || value == "..=")
                    return {value, 2};

                return {};
            }

            static bool comparison(const Node& value)
            {
                if (value.kind != "Binary") return false;

                return value.value == "<"
                    || value.value == "<="
                    || value.value == ">"
                    || value.value == ">="
                    || value.value == "=="
                    || value.value == "!=";
            }

            bool postfixQuestion()
            {
                const auto found = choices_->find(position_);

                if (found == choices_->end())
                    throw Fork{position_};

                return found->second;
            }

            NodePtr expression(
                std::initializer_list<std::string_view> stops
            )
            {
                // ?는 타입 정보 없이 완결되는 구문 해석이 하나일 때만 채택한다.
                // Accept ? only when exactly one complete syntactic interpretation exists, without type information.
                const auto start = position_;
                auto* previousChoices = choices_;

                if (previousChoices)
                    return parseExpression(1);

                std::vector<Choices> work(1);
                NodePtr accepted;

                std::size_t acceptedEnd = start;
                Failure best{start, "Expected an expression."};
                unsigned attempts = 0;

                while (!work.empty())
                {
                    if (++attempts > 4096)
                    {
                        choices_ = previousChoices;
                        position_ = start;
                        fail("Expression ambiguity exceeds the limit; add parentheses.");
                    }

                    Choices branch = std::move(work.back());
                    work.pop_back();

                    position_ = start;
                    choices_ = &branch;

                    try
                    {
                        auto candidate = parseExpression(1);

                        bool complete = false;

                        for (auto stop : stops)
                            complete = complete || is(stop);

                        if (!complete)
                            fail("Unexpected token after expression.");

                        if (accepted)
                        {
                            choices_ = previousChoices;
                            position_ = start;
                            fail("Ambiguous '?' expression; add parentheses.");
                        }

                        acceptedEnd = position_;
                        accepted = std::move(candidate);
                    }
                    catch (const Fork& fork)
                    {
                        Choices postfix = branch;
                        postfix[fork.position] = true;
                        branch[fork.position] = false;

                        work.push_back(std::move(branch));
                        work.push_back(std::move(postfix));
                    }
                    catch (const Failure& error)
                    {
                        if (
                            error.message
                            == "Ambiguous '?' expression; add parentheses."
                        )
                        {
                            choices_ = previousChoices;
                            throw;
                        }

                        if (error.position >= best.position)
                            best = error;
                    }
                }

                choices_ = previousChoices;

                if (!accepted)
                {
                    position_ = start;
                    throw best;
                }

                position_ = acceptedEnd;
                return accepted;
            }

            NodePtr parseExpression(int minimum)
            {
                Depth depth(*this);

                const auto start = position_;
                auto left = prefix();

                while (true)
                {
                    if (genericDefault_ && is(">")) break;
                    if (minimum <= 1 && is("?"))
                    {
                        // 후위 해석은 prefix/postfix 단계에서 이미 소비한다.
                        // The prefix/postfix stage has already consumed postfix interpretations.
                        if (postfixQuestion())
                            fail("Unexpected postfix question mark.");

                        ++position_;

                        auto conditional = node("Conditional", "", start);

                        attach(*conditional, std::move(left));
                        attach(*conditional, parseExpression(1));

                        expect(":");

                        attach(*conditional, parseExpression(1));

                        conditional->range = span(start);
                        left = std::move(conditional);
                        continue;
                    }

                    const auto operation = binaryOperator();

                    if (
                        operation.precedence < minimum
                        || operation.precedence == 0
                    )
                    {
                        break;
                    }

                    position_ += operation.count;

                    auto right = parseExpression(operation.precedence + 1);

                    if (
                        operation.precedence == 8
                        || operation.precedence == 9
                    )
                    {
                        if (comparison(*left) || comparison(*right))
                            fail("Comparison chains require parentheses.");
                    }

                    if (
                        operation.precedence == 2
                        && (left->kind == "Range" || right->kind == "Range")
                    )
                    {
                        fail("Range operators cannot be chained.");
                    }

                    auto binary = node(
                        operation.precedence == 2 ? "Range" : "Binary",
                        operation.text,
                        start
                    );

                    attach(*binary, std::move(left));
                    attach(*binary, std::move(right));

                    binary->range = span(start);
                    left = std::move(binary);
                }

                return left;
            }

            NodePtr prefix()
            {
                Depth depth(*this);
                const auto start = position_;

                if (
                    is("&") || is("*") || is("+") || is("-")
                    || is("!") || is("~") || is("++") || is("--")
                )
                {
                    std::string operation(tokens_[position_++].text);

                    if (operation == "&" && take("mut"))
                        operation = "&mut";

                    auto result = node("Prefix", operation, start);

                    attach(*result, prefix());
                    result->range = span(start);

                    return result;
                }

                auto result = primary();

                while (true)
                {
                    if (take("."))
                    {
                        auto member = node("Member", name(), start);
                        attach(*member, std::move(result));
                        result = std::move(member);
                    }
                    else if (
                        is("<")
                        && (result->kind == "Name" || result->kind == "Member")
                    )
                    {
                        const auto saved = position_;
                        NodePtr arguments;

                        try
                        {
                            ++position_;

                            arguments = node("TypeArguments", "", saved);

                            do
                            {
                                attach(*arguments, typeArgument());
                            }
                            while (take(",") && !is(">"));

                            expect(">");

                            if (!is("(") && !is("{") && !is("."))
                                throw Failure{
                                    position_,
                                    "Not a generic invocation."
                                };
                        }
                        catch (const Failure&)
                        {
                            position_ = saved;
                            break;
                        }

                        auto generic = node("GenericName", "", start);

                        attach(*generic, std::move(result));
                        attach(*generic, std::move(arguments));

                        result = std::move(generic);
                    }
                    else if (take("("))
                    {
                        auto call = node("Call", "", start);
                        attach(*call, std::move(result));

                        bool namedSeen = false;
                        std::set<std::string> labels;

                        if (!is(")"))
                        {
                            do
                            {
                                const auto argumentStart = position_;
                                std::string label;

                                if (
                                    token().kind == TokenKind::Identifier
                                    && is("=", 1)
                                )
                                {
                                    label = name();

                                    if (!labels.insert(label).second)
                                        fail("Repeated named argument.");

                                    expect("=");
                                    namedSeen = true;
                                }
                                else if (namedSeen)
                                {
                                    fail(
                                        "Positional arguments must precede named arguments."
                                    );
                                }

                                auto argument = node(
                                    "Argument",
                                    label,
                                    argumentStart
                                );

                                attach(*argument, callArgumentValue());
                                argument->range = span(argumentStart);

                                attach(*call, std::move(argument));
                            }
                            while (take(",") && !is(")"));
                        }

                        expect(")");
                        result = std::move(call);
                    }
                    else if (take("["))
                    {
                        auto index = node("Index", "", start);

                        attach(*index, std::move(result));
                        attach(*index, parseExpression(1));

                        expect("]");
                        result = std::move(index);
                    }
                    else if (
                        structures_
                        && is("{")
                        && (
                            result->kind == "Name"
                            || result->kind == "Member"
                            || result->kind == "GenericName"
                        )
                    )
                    {
                        ++position_;

                        auto initial = node("NamedInitializer", "", start);
                        attach(*initial, std::move(result));

                        if (!is("}"))
                        {
                            do
                            {
                                const auto fieldStart = position_;

                                auto field = node(
                                    "FieldInitializer",
                                    name(),
                                    fieldStart
                                );

                                expect("=");
                                attach(*field, parseExpression(1));
                                attach(*initial, std::move(field));
                            }
                            while (take(",") && !is("}"));
                        }

                        expect("}");
                        result = std::move(initial);
                    }
                    else if (is("?") && postfixQuestion())
                    {
                        ++position_;

                        auto propagate = node("Postfix", "?", start);
                        attach(*propagate, std::move(result));
                        result = std::move(propagate);
                    }
                    else if (is("++") || is("--"))
                    {
                        std::string operation(tokens_[position_++].text);

                        auto increment = node("Postfix", operation, start);
                        attach(*increment, std::move(result));

                        result = std::move(increment);
                    }
                    else
                    {
                        break;
                    }

                    result->range = span(start);
                }

                return result;
            }

            // 복합 타입 인수 후보를 보존하고 허용 여부는 intrinsic 이름 해석에 맡긴다.
            // Preserve compound type-operand candidates; intrinsic resolution decides legality.
            NodePtr callArgumentValue()
            {
                const auto start = position_;

                try
                {
                    auto parsedType = type();
                    bool compound = false;

                    for (auto i = start; i < position_; ++i)
                    {
                        compound = compound
                            || tokens_[i].text == "<"
                            || tokens_[i].text == "dyn";
                    }

                    if (compound && (is(",") || is(")")))
                    {
                        auto candidate = node(
                            "TypeOperandCandidate",
                            "",
                            start
                        );

                        attach(*candidate, std::move(parsedType));
                        return candidate;
                    }
                }
                catch (const Failure&)
                {
                }

                position_ = start;

                auto value = parseExpression(1);

                if (!is(",") && !is(")"))
                    fail("Expected an argument delimiter.");

                return value;
            }

            NodePtr typeArgument()
            {
                const auto start = position_;

                if (
                    token().kind == TokenKind::Integer
                    || is("-")
                    || is("+")
                )
                {
                    std::string value;

                    if (is("-") || is("+"))
                        value = std::string(tokens_[position_++].text);

                    if (token().kind != TokenKind::Integer)
                        fail("Expected a constant integer type argument.");

                    value += tokens_[position_++].text;

                    return node("ConstantTypeArgument", value, start);
                }

                return type();
            }

            NodePtr primary()
            {
                const auto start = position_;

                if (take("("))
                {
                    auto result = node("Group", "", start);
                    BooleanScope delimiter(genericDefault_, false);

                    attach(*result, parseExpression(1));
                    expect(")");

                    result->range = span(start);
                    return result;
                }

                if (take("["))
                {
                    std::size_t end = position_;
                    while (end < tokens_.size() && tokens_[end].text != "]" && tokens_[end].text != ";"
                        && tokens_[end].text != "[") ++end;
                    if (end + 1 < tokens_.size() && tokens_[end].text == "]" && tokens_[end + 1].text == "(")
                    {
                        // 클로저의 문장과 기본 인수는 바깥 식과 독립적으로 ?를 해석한다.
                        // Resolve ? in closure statements and default arguments independently of the enclosing expression.
                        ChoicesScope closureChoices(choices_);
                        auto result = node("Closure", "", start);
                        auto captures = node("Captures", "", start);
                        if (!is("]")) do
                        {
                            const auto captureStart = position_;
                            std::string mode = "value";
                            if (take("&")) mode = take("mut") ? "mut" : "read";
                            const auto captureName = name();
                            auto capture = node("Capture", captureName, captureStart);
                            attach(*capture, node("CaptureMode", mode, captureStart));
                            attach(*capture, node("Name", captureName, captureStart));
                            attach(*captures, std::move(capture));
                        } while (take(",") && !is("]"));
                        expect("]"); expect("(");
                        auto parameters = node("Parameters", "", position_ - 1);
                        bool defaultSeen = false;
                        if (!is(")")) do
                        {
                            auto argument = parameter();
                            if (argument->value == "self") fail("Closure parameters cannot use the reserved receiver name self.");
                            if (argument->kind == "VariadicParameter") fail("Closure parameters have fixed arity in 0.2.");
                            const bool hasDefault = std::any_of(argument->children.begin(), argument->children.end(), [](const auto& item) { return item->kind == "Default"; });
                            if (defaultSeen && !hasDefault) fail("A required parameter cannot follow a defaulted parameter.");
                            defaultSeen = defaultSeen || hasDefault;
                            attach(*parameters, std::move(argument));
                        } while (take(",") && !is(")"));
                        expect(")"); expect("->");
                        auto returned = type();
                        attach(*result, std::move(captures)); attach(*result, std::move(parameters)); attach(*result, std::move(returned));
                        attach(*result, block()); result->range = span(start); return result;
                    }
                    auto result = node("ArrayLiteral", "", start);

                    if (!is("]"))
                    {
                        attach(*result, parseExpression(1));
                        if (take(";"))
                        {
                            result->kind = "ArrayRepeat";
                            attach(*result, parseExpression(1));
                        }
                        else
                        {
                            while (take(",") && !is("]"))
                                attach(*result, parseExpression(1));
                        }
                    }

                    expect("]");

                    result->range = span(start);
                    return result;
                }

                if (
                    token().kind == TokenKind::Integer
                    || token().kind == TokenKind::Float
                    || token().kind == TokenKind::String
                    || token().kind == TokenKind::Character
                    || is("true")
                    || is("false")
                )
                {
                    std::string value(tokens_[position_++].text);
                    return node("Literal", value, start);
                }

                return node("Name", name(), start);
            }
        };
    }

    Parser::Parser(
        std::vector<Token> tokens,
        Diagnostics& diagnostics
    )
        : tokens_(std::move(tokens)),
          diagnostics_(diagnostics)
    {
        if (
            tokens_.empty()
            || tokens_.back().kind != TokenKind::End
        )
        {
            throw std::invalid_argument(
                "Parser requires an EOF-terminated token stream."
            );
        }
    }

    NodePtr Parser::parseModule()
    {
        return Engine(tokens_, diagnostics_).module();
    }
}
