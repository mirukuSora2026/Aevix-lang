// ========================
//
// Created By : miruku_sora2026
//
// File name : ControlFlowChecker.cpp
//
// Description :
//     분기와 반복문의 초기화 상태를 병합하여 초기화 전 읽기와 반환 누락을 진단한다.
//     Merge branch and loop initialization states to diagnose premature reads and missing returns.
//
// ========================

#include "aevix/Frontend/Sema/ControlFlowChecker.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace aevix
{
    namespace
    {
        struct State final
        {
            std::set<SymbolId> initialized;
            std::set<SymbolId> possiblyInitialized;
            bool operator==(const State&) const = default;
        };

        struct Flow final
        {
            std::optional<State> normal;
            std::vector<State> breaks;
            std::vector<State> continues;
        };

        bool modifier(const Node& node, const std::string& wanted)
        {
            for (const auto& child : node.children)
            {
                if (child->kind != "Modifier") continue;
                std::istringstream words(child->value);
                std::string word;
                while (words >> word) if (word == wanted) return true;
            }
            return false;
        }

        const Node* initializer(const Node& node)
        {
            for (std::size_t i = 1; i < node.children.size(); ++i)
                if (node.children[i]->kind != "Modifier"
                    && node.children[i]->kind != "ArraySuffixes")
                    return node.children[i].get();
            return nullptr;
        }

        const Node& ungroup(const Node& node)
        {
            if (node.kind == "Group" && node.children.size() == 1)
                return ungroup(*node.children.front());
            return node;
        }

        std::optional<bool> truth(const Node& expression)
        {
            const auto& node = ungroup(expression);
            if (node.kind == "Literal" && node.value == "true") return true;
            if (node.kind == "Literal" && node.value == "false") return false;
            if (node.kind == "Prefix" && node.value == "!" && node.children.size() == 1)
                if (const auto value = truth(*node.children.front())) return !*value;
            return std::nullopt;
        }

        State merge(const State& left, const State& right)
        {
            State result;
            std::set_intersection(left.initialized.begin(), left.initialized.end(),
                                  right.initialized.begin(), right.initialized.end(),
                                  std::inserter(result.initialized, result.initialized.end()));
            result.possiblyInitialized = left.possiblyInitialized;
            result.possiblyInitialized.insert(right.possiblyInitialized.begin(),
                                              right.possiblyInitialized.end());
            return result;
        }

        void include(std::optional<State>& destination, const std::optional<State>& source)
        {
            if (source) destination = destination ? merge(*destination, *source) : *source;
        }

        void exits(Flow& destination, const Flow& source)
        {
            destination.breaks.insert(destination.breaks.end(), source.breaks.begin(), source.breaks.end());
            destination.continues.insert(destination.continues.end(), source.continues.begin(), source.continues.end());
        }

        class Checker final
        {
        public:
            Checker(const ModuleGraph& graph, const NameResolution& names,
                    const TypeCheckResult& types, Diagnostics& diagnostics)
                : graph_(graph), names_(names), types_(types), diagnostics_(diagnostics) {}

            bool run()
            {
                State globals;
                for (const auto& module : graph_.modules)
                {
                    if (!module || !module->ast) continue;
                    for (const auto& node : module->ast->children)
                        if (node->kind == "Variable")
                        {
                            const auto id = symbol(*node);
                            if (id != InvalidSymbolId)
                            {
                                variables_.insert(id);
                                if (initializer(*node)) initialize(globals, id);
                            }
                        }
                }
                for (const auto& module : graph_.modules)
                    if (module && module->ast)
                        for (const auto& node : module->ast->children)
                            if (node->kind == "Variable")
                                if (const auto* value = initializer(*node))
                                {
                                    auto state = globals;
                                    state.initialized.erase(symbol(*node));
                                    read(*value, state);
                                }
                for (const auto& module : graph_.modules)
                    if (module && module->ast) functions(*module->ast, globals);
                return diagnostics_.empty();
            }

        private:
            const ModuleGraph& graph_;
            const NameResolution& names_;
            const TypeCheckResult& types_;
            Diagnostics& diagnostics_;
            std::set<SymbolId> variables_;
            std::set<std::pair<const Node*, std::string>> reported_;

            void error(const Node& node, std::string message)
            {
                if (reported_.emplace(&node, message).second)
                    diagnostics_.push_back({node.range, std::move(message)});
            }

            SymbolId symbol(const Node& node) const
            {
                const auto found = names_.references.find(&node);
                return found != names_.references.end() && found->second.size() == 1
                    ? found->second.front() : InvalidSymbolId;
            }

            static void initialize(State& state, SymbolId id)
            {
                state.initialized.insert(id);
                state.possiblyInitialized.insert(id);
            }

            bool never(const Node& node) const
            {
                const auto found = types_.nodeTypes.find(&node);
                if (found != types_.nodeTypes.end()
                    && types_.types.get(found->second).kind == TypeKind::Never) return true;
                if (node.kind == "Type" || node.kind == "QualifiedType"
                    || node.kind == "ReferenceType" || node.kind == "TypeArguments"
                    || node.kind == "TypeOperandCandidate") return false;
                if (node.kind == "Conditional" && node.children.size() == 3)
                {
                    if (never(*node.children[0])) return true;
                    const auto condition = truth(*node.children[0]);
                    if (condition) return never(*node.children[*condition ? 1 : 2]);
                    return never(*node.children[1]) && never(*node.children[2]);
                }
                if (node.kind == "Binary" && (node.value == "&&" || node.value == "||")
                    && node.children.size() == 2)
                {
                    if (never(*node.children[0])) return true;
                    const auto left = truth(*node.children[0]);
                    return left && (node.value == "&&" ? *left : !*left)
                        && never(*node.children[1]);
                }
                for (const auto& child : node.children)
                    if (never(*child)) return true;
                return false;
            }

            void functions(const Node& node, const State& globals)
            {
                for (const auto& child : node.children)
                    if (child->kind == "CompilerTemplate" || child->kind == "CompilerContract" || child->kind == "CompilerIntrinsic") return;
                if (node.kind != "Function")
                {
                    for (const auto& child : node.children) functions(*child, globals);
                    return;
                }
                State state = globals;
                const Node* body = nullptr;
                for (const auto& child : node.children)
                {
                    if (child->kind == "Parameters")
                        for (const auto& parameter : child->children)
                        {
                            const auto id = symbol(*parameter);
                            if (id != InvalidSymbolId)
                            {
                                variables_.insert(id);
                                initialize(state, id);
                            }
                        }
                    if (child->kind == "Block") body = child.get();
                }
                if (!body) return;
                validateExits(*body, 0, 0);
                const auto flow = statement(*body, state);
                const auto function = types_.symbolTypes.find(symbol(node));
                if (function == types_.symbolTypes.end()) return;
                const auto& signature = types_.types.get(function->second);
                const auto kind = types_.types.get(signature.result).kind;
                if (flow.normal && kind != TypeKind::Void && kind != TypeKind::Error)
                    error(node, kind == TypeKind::Never
                        ? "A never-returning function has a reachable end."
                        : "Non-void function '" + node.value + "' can reach its end without returning a value.");
            }

            void validateExits(const Node& node, std::size_t loops, std::size_t switches)
            {
                if (node.kind == "Function") return;
                if (node.kind == "Break" && loops == 0 && switches == 0)
                    error(node, "break is only allowed inside a loop or switch.");
                if (node.kind == "Continue" && loops == 0)
                    error(node, "continue is only allowed inside a loop.");
                const bool loop = node.kind == "While" || node.kind == "For"
                    || node.kind == "RangeFor" || node.kind == "EnhancedFor";
                for (const auto& child : node.children)
                    validateExits(*child, loops + (loop ? 1 : 0),
                                  switches + (node.kind == "Switch" ? 1 : 0));
            }

            void read(const Node& expression, State& state)
            {
                const auto& node = ungroup(expression);
                if (node.kind == "Type" || node.kind == "QualifiedType"
                    || node.kind == "ReferenceType" || node.kind == "Modifier"
                    || node.kind == "PatternField" || node.kind == "TypeArguments") return;
                const auto id = symbol(node);
                if (variables_.contains(id) && !state.initialized.contains(id))
                    error(node, "Variable '" + names_.symbols[id].name + "' may be used before initialization.");
                if (node.kind == "Conditional" && node.children.size() == 3)
                {
                    read(*node.children[0], state);
                    const auto condition = truth(*node.children[0]);
                    if (!condition || *condition) read(*node.children[1], state);
                    if (!condition || !*condition) read(*node.children[2], state);
                    return;
                }
                if (node.kind == "Binary" && (node.value == "&&" || node.value == "||")
                    && node.children.size() == 2)
                {
                    read(*node.children[0], state);
                    const auto left = truth(*node.children[0]);
                    if (!left || (node.value == "&&" ? *left : !*left))
                        read(*node.children[1], state);
                    return;
                }
                for (const auto& child : node.children)
                {
                    read(*child, state);
                    if (never(*child)) break;
                }
            }

            Flow sequence(const Node& node, State state)
            {
                Flow result{std::move(state), {}, {}};
                for (const auto& child : node.children)
                {
                    if (!result.normal) break;
                    auto next = statement(*child, *result.normal);
                    exits(result, next);
                    result.normal = std::move(next.normal);
                }
                return result;
            }

            Flow loop(const Node& node, State state)
            {
                const bool counted = node.kind == "For";
                const bool enhanced = node.kind == "EnhancedFor" || node.kind == "RangeFor";
                if ((counted && node.children.size() != 4)
                    || (enhanced && node.children.size() != 3)
                    || (!counted && !enhanced && node.children.size() != 2)) return {state, {}, {}};
                if (counted)
                {
                    auto init = statement(*node.children[0], state);
                    if (!init.normal) return init;
                    state = *init.normal;
                }
                const auto& condition = *node.children[counted || enhanced ? 1 : 0];
                const auto& body = *node.children[counted ? 3 : enhanced ? 2 : 1];
                const auto constant = enhanced ? std::nullopt
                    : condition.kind == "Empty" ? std::optional<bool>(true) : truth(condition);
                read(condition, state);
                if (never(condition)) return {};
                if (constant && !*constant) return {state, {}, {}};
                if (enhanced)
                {
                    const auto id = symbol(*node.children[0]);
                    if (id != InvalidSymbolId)
                    {
                        variables_.insert(id);
                        initialize(state, id);
                    }
                }

                // 초기 진입과 역방향 경로의 교집합/합집합을 안정될 때까지 병합한다.
                // Merge entry and back edges by intersection/union until their states stabilize.
                State entry = state;
                Flow bodyFlow;
                std::optional<State> back;
                for (;;)
                {
                    if (!enhanced) read(condition, entry);
                    bodyFlow = statement(body, entry);
                    back = bodyFlow.normal;
                    for (const auto& continued : bodyFlow.continues) include(back, continued);
                    if (counted && back)
                        back = statement(*node.children[2], *back).normal;
                    const auto next = back ? merge(state, *back) : state;
                    if (next == entry) break;
                    entry = next;
                }
                Flow result;
                if (!constant || !*constant) result.normal = entry;
                for (const auto& broken : bodyFlow.breaks) include(result.normal, broken);
                return result;
            }

            Flow switchFlow(const Node& node, State state)
            {
                if (node.children.empty()) return {state, {}, {}};
                read(*node.children[0], state);
                if (never(*node.children[0])) return {};
                Flow result;
                bool hasDefault = false;
                for (std::size_t i = 1; i < node.children.size(); ++i)
                {
                    const auto& branch = *node.children[i];
                    hasDefault = hasDefault || branch.kind == "Default";
                    State branchState = state;
                    for (const auto& child : branch.children)
                    {
                        if (child->kind == "PatternBindings")
                            for (const auto& binding : child->children)
                            {
                                const auto id = symbol(*binding);
                                if (id != InvalidSymbolId)
                                {
                                    variables_.insert(id);
                                    initialize(branchState, id);
                                }
                            }
                        else if (child->kind == "CaseScope")
                        {
                            auto flow = sequence(*child, branchState);
                            include(result.normal, flow.normal);
                            for (const auto& broken : flow.breaks) include(result.normal, broken);
                            result.continues.insert(result.continues.end(), flow.continues.begin(), flow.continues.end());
                        }
                        else read(*child, branchState);
                    }
                }
                if (!hasDefault && !types_.exhaustiveSwitches.contains(&node)) include(result.normal, state);
                return result;
            }

            Flow statement(const Node& node, State state)
            {
                if (node.kind == "Block" || node.kind == "CaseScope") return sequence(node, std::move(state));
                if (node.kind == "Variable")
                {
                    const auto id = symbol(node);
                    if (id != InvalidSymbolId)
                    {
                        variables_.insert(id);
                        state.initialized.erase(id);
                        state.possiblyInitialized.erase(id);
                    }
                    if (const auto* value = initializer(node))
                    {
                        read(*value, state);
                        if (never(*value)) return {};
                        if (id != InvalidSymbolId) initialize(state, id);
                    }
                    return {state, {}, {}};
                }
                if (node.kind == "Assignment" && node.children.size() == 2)
                {
                    const auto& target = ungroup(*node.children[0]);
                    const auto id = symbol(target);
                    const bool direct = (target.kind == "Name" || target.kind == "Member")
                        && variables_.contains(id);
                    if (!direct || node.value != "=")
                    {
                        read(target, state);
                        if (never(target)) return {};
                    }
                    read(*node.children[1], state);
                    if (never(*node.children[1])) return {};
                    if (direct)
                    {
                        const auto* declaration = names_.symbols[id].declaration;
                        if (declaration && !modifier(*declaration, "mut")
                            && state.possiblyInitialized.contains(id))
                            error(target, "Immutable variable '" + names_.symbols[id].name + "' may already be initialized.");
                        initialize(state, id);
                    }
                    return {state, {}, {}};
                }
                if (node.kind == "Return")
                {
                    for (const auto& child : node.children) read(*child, state);
                    return {};
                }
                if (node.kind == "Break") return {std::nullopt, {state}, {}};
                if (node.kind == "Continue") return {std::nullopt, {}, {state}};
                if (node.kind == "If" && node.children.size() >= 2)
                {
                    read(*node.children[0], state);
                    if (never(*node.children[0])) return {};
                    const auto condition = truth(*node.children[0]);
                    Flow left, right;
                    if (!condition || *condition) left = statement(*node.children[1], state);
                    if (!condition || !*condition)
                        right = node.children.size() > 2 ? statement(*node.children[2], state) : Flow{state, {}, {}};
                    include(left.normal, right.normal);
                    exits(left, right);
                    return left;
                }
                if (node.kind == "While" || node.kind == "For"
                    || node.kind == "EnhancedFor" || node.kind == "RangeFor") return loop(node, std::move(state));
                if (node.kind == "Switch") return switchFlow(node, std::move(state));
                if (node.kind == "Function" || node.kind == "Struct"
                    || node.kind == "Enum" || node.kind == "Interface") return {state, {}, {}};
                if (node.kind == "ExpressionStatement" && !node.children.empty())
                {
                    read(*node.children[0], state);
                    if (never(*node.children[0])) return {};
                    return {state, {}, {}};
                }
                read(node, state);
                return never(node) ? Flow{} : Flow{state, {}, {}};
            }
        };
    }

    ControlFlowChecker::ControlFlowChecker(Diagnostics& diagnostics)
        : diagnostics_(diagnostics) {}

    bool ControlFlowChecker::check(const ModuleGraph& graph, const NameResolution& names,
                                   const TypeCheckResult& types)
    {
        return Checker(graph, names, types, diagnostics_).run();
    }
}
