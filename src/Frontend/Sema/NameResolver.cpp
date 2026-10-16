// ========================
//
// Created By : miruku_sora2026
//
// File name : NameResolver.cpp
//
// Description :
//     선언과 재공개 원본을 연결하고 가져오기, 지역 스코프 및 접근 권한에 따라 이름을 해석한다.
//     Link declarations and re-export origins, then resolve names using imports, lexical scopes, and visibility.
//
// ========================

#include "aevix/Frontend/Sema/NameResolver.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace aevix
{
    namespace
    {
        using Candidates = std::vector<SymbolId>;

        bool hasModifier(const Node& node, std::string_view wanted)
        {
            for (const auto& child : node.children)
            {
                if (child->kind != "Modifier") continue;
                std::istringstream words(child->value);
                std::string word;
                while (words >> word)
                    if (word == wanted) return true;
            }
            return false;
        }

        bool declaration(const Node& node)
        {
            return node.kind == "Struct" || node.kind == "Enum"
                || node.kind == "Interface" || node.kind == "Function"
                || node.kind == "Variable" || node.kind == "Variant"
                || node.kind == "TypeAlias";
        }

        bool typeDeclaration(const Node& node)
        {
            return node.kind == "Struct" || node.kind == "Enum"
                || node.kind == "Interface" || node.kind == "TypeAlias";
        }

        bool append(Candidates& destination, const Candidates& source)
        {
            bool changed = false;
            for (auto id : source)
            {
                if (std::find(destination.begin(), destination.end(), id)
                    != destination.end()) continue;
                destination.push_back(id);
                changed = true;
            }
            return changed;
        }

        std::string join(const std::vector<std::string>& parts,
                         std::size_t first, std::size_t end)
        {
            std::string result;
            for (auto i = first; i < end; ++i)
            {
                if (!result.empty()) result += '.';
                result += parts[i];
            }
            return result;
        }

        const Node* childOf(const Node& node, std::string_view kind)
        {
            for (const auto& child : node.children)
                if (child->kind == kind) return child.get();
            return nullptr;
        }

        struct ModuleNames
        {
            SymbolTable direct;
            SymbolTable exports;
            SymbolTable imported;
            SymbolTable qualified;
            std::set<std::string> paths;
        };

        struct Context
        {
            ModuleId module;
            const Node* owner = nullptr;
            std::vector<SymbolTable> scopes;
        };

        class Resolver final
        {
        public:
            Resolver(const ModuleGraph& graph, Diagnostics& diagnostics)
                : graph_(graph), diagnostics_(diagnostics), modules_(graph.modules.size()) {}

            NameResolution run()
            {
                registerPrelude();

                // 순환 import에서도 모든 직접 선언이 연결 전에 존재한다.
                // Every direct declaration exists before linking, including import cycles.
                for (ModuleId id = 0; id < graph_.modules.size(); ++id)
                {
                    if (!graph_.modules[id] || !graph_.modules[id]->ast) continue;
                    for (const auto& node : graph_.modules[id]->ast->children)
                    {
                        if (!declaration(*node)) continue;
                        registerDeclaration(*node, id, modules_[id].direct, nullptr);
                    }
                    for (const auto& [name, ids] : modules_[id].direct)
                        for (auto symbol : ids)
                            if (result_.symbols[symbol].isPublic)
                                modules_[id].exports[name].push_back(symbol);
                }

                if (coreEnabled_)
                    for (ModuleId module = 0; module < graph_.modules.size(); ++module)
                        if (graph_.modules[module]->name == "$core")
                            for (const auto* name : {"Optional", "Result", "CollectionError", "InputError", "RangeError", "TextError", "IntegerError", "FloatError", "BoundsError"})
                                if (modules_[module].direct.contains(name)) prelude_[name] = modules_[module].direct.at(name);
                linkReexports();
                linkImports();

                for (ModuleId id = 0; id < graph_.modules.size(); ++id)
                {
                    if (!graph_.modules[id] || !graph_.modules[id]->ast) continue;
                    Context context{id, nullptr, {SymbolTable{}}};
                    for (const auto& node : graph_.modules[id]->ast->children)
                        if (declaration(*node)) walkDeclaration(*node, context, false);
                }

                validateAliasCycles();
                if (diagnostics_.empty()) validateAliasOverloads();
                result_.succeeded = diagnostics_.empty();
                return std::move(result_);
            }

        private:
            const ModuleGraph& graph_;
            Diagnostics& diagnostics_;
            std::vector<ModuleNames> modules_;
            NameResolution result_;
            SymbolTable prelude_;
            bool coreEnabled_ = false;
            std::unordered_map<const Node*, SymbolId> declarationIds_;
            std::unordered_map<const Node*, SymbolTable> members_;
            std::unordered_map<const Node*, const Node*> owners_;
            std::set<const Node*> resolvingAliases_;
            std::set<const Node*> resolvedAliases_;

            void error(const Node& node, std::string message)
            {
                diagnostics_.push_back({node.range, std::move(message)});
            }

            SymbolId addSymbol(const Node* node, ModuleId module,
                               std::string name, SymbolKind kind, bool visible)
            {
                const SymbolId id = result_.symbols.size();
                result_.symbols.push_back({id, module, node, std::move(name), kind, visible});
                if (node) declarationIds_[node] = id;
                return id;
            }

            void registerPrelude()
            {
                for (const auto& module : graph_.modules)
                    if (module && module->name == "$core") coreEnabled_ = true;
                for (const auto* name : {
                    "i8", "i16", "i32", "i64", "i128", "u8", "u16", "u32", "u64", "u128",
                    "isize", "usize", "f16", "bf16", "f32", "f64", "int", "float",
                    "bool", "char", "byte", "unit", "never", "void", "string",
                    "string_view", "Array", "Vector", "Slice", "MutSlice", "Tuple",
                    "Range", "Optional", "Result", "Copy", "Fn", "FnMut", "FnOnce"
                })
                {
                    if (coreEnabled_ && (std::string_view(name) == "Result" || std::string_view(name) == "Optional")) continue;
                    prelude_[name].push_back(addSymbol(nullptr, InvalidModuleId, name,
                                                       SymbolKind::Type, true));
                }
                for (const auto* name : {
                    "Integer", "Float", "Boolean", "Char", "Byte", "Unit", "Never", "checked"
                })
                {
                    prelude_[name].push_back(addSymbol(nullptr, InvalidModuleId, name,
                                                       SymbolKind::Intrinsic, true));
                }
            }

            std::string syntaxKey(const Node& node,
                                  const std::map<std::string, std::string>& parameters) const
            {
                std::string value = node.value;
                if (node.kind == "Type")
                {
                    if (auto found = parameters.find(value); found != parameters.end())
                        value = found->second;
                    if (value == "int") value = "i32";
                    if (value == "float") value = "f64";
                }
                std::string result = node.kind + "(" + value;
                for (const auto& child : node.children)
                {
                    if (child->kind == "Modifier" || child->kind == "Default") continue;
                    result += "," + syntaxKey(*child, parameters);
                }
                return result + ")";
            }

            std::string signature(const Node& node) const
            {
                std::map<std::string, std::string> genericNames;
                if (const auto* parameters = childOf(node, "GenericParameters"))
                    for (std::size_t i = 0; i < parameters->children.size(); ++i)
                        genericNames[parameters->children[i]->value] = "$" + std::to_string(i);

                std::string result = std::to_string(genericNames.size()) + "(";
                if (const auto* parameters = childOf(node, "Parameters"))
                {
                    for (const auto& parameter : parameters->children)
                    {
                        result += parameter->kind == "VariadicParameter" ? "..." : "";
                        for (const auto& child : parameter->children)
                        {
                            if (child->kind == "Modifier" || child->kind == "Default") continue;
                            result += syntaxKey(*child, genericNames);
                        }
                        result += ";";
                    }
                }
                return result + ")";
            }

            void registerDeclaration(const Node& node, ModuleId module,
                                     SymbolTable& table, const Node* owner)
            {
                SymbolKind kind = typeDeclaration(node) ? SymbolKind::Type
                    : node.kind == "Function" ? SymbolKind::Function : SymbolKind::Value;
                const bool visible = node.kind == "Variant"
                    ? !hasModifier(node, "private") : hasModifier(node, "public");
                const SymbolId id = addSymbol(&node, module, node.value, kind, visible);
                if (owner) owners_[&node] = owner;
                if (!owner && (prelude_.contains(node.value) || (coreEnabled_ && graph_.modules[module]->name != "$core" && (node.value == "Result" || node.value == "Optional" || node.value == "CollectionError" || node.value == "InputError" || node.value == "RangeError" || node.value == "TextError" || node.value == "IntegerError" || node.value == "FloatError" || node.value == "BoundsError"))))
                    error(node, "Cannot redeclare prelude name '" + node.value + "'.");

                auto& existing = table[node.value];
                for (auto previous : existing)
                {
                    const auto& symbol = result_.symbols[previous];
                    if (kind != SymbolKind::Function || symbol.kind != SymbolKind::Function)
                    {
                        error(node, "Duplicate declaration '" + node.value + "' in one scope.");
                        break;
                    }
                    if (signature(node) == signature(*symbol.declaration))
                    {
                        error(node, "Duplicate overload signature for '" + node.value
                              + "'; return types and parameter names do not distinguish overloads.");
                        break;
                    }
                }
                existing.push_back(id);
                result_.references[&node] = {id};
                if (node.kind == "TypeAlias" && !node.children.empty())
                    result_.aliasTargets[&node] = node.children.front().get();

                if (typeDeclaration(node) || node.kind == "Variant")
                    for (const auto& child : node.children)
                        if (declaration(*child))
                            registerDeclaration(*child, module, members_[&node], &node);
            }

            std::vector<const Node*> imports(ModuleId module) const
            {
                std::vector<const Node*> result;
                if (!graph_.modules[module] || !graph_.modules[module]->ast) return result;
                for (const auto& node : graph_.modules[module]->ast->children)
                    if (node->kind == "Import") result.push_back(node.get());
                return result;
            }

            Candidates exported(ModuleId module, const std::string& name) const
            {
                const auto found = modules_[module].exports.find(name);
                return found == modules_[module].exports.end() ? Candidates{} : found->second;
            }

            void linkReexports()
            {
                bool changed = true;
                while (changed)
                {
                    changed = false;
                    for (ModuleId module = 0; module < modules_.size(); ++module)
                    {
                        for (const auto* imported : imports(module))
                        {
                            if (!hasModifier(*imported, "public")) continue;
                            const auto target = graph_.find(imported->value);
                            if (target == InvalidModuleId) continue;
                            for (const auto& item : imported->children)
                            {
                                if (item->kind != "ImportName" || item->value == "*") continue;
                                changed = append(modules_[module].exports[item->value],
                                                 exported(target, item->value)) || changed;
                            }
                        }
                    }
                }

                // 재공개된 서로 다른 원본은 공개 API에서 모호한 이름으로 유지한다.
                // Keep distinct re-exported origins as ambiguous public API candidates.
            }

            void linkImports()
            {
                for (ModuleId module = 0; module < modules_.size(); ++module)
                {
                    auto& state = modules_[module];
                    if (!graph_.modules[module]) continue;
                    state.paths.insert(graph_.modules[module]->name);
                    for (const auto& [name, ids] : state.direct)
                        state.qualified[graph_.modules[module]->name + "." + name] = ids;
                    for (const auto& [name, ids] : state.exports)
                        append(state.qualified[graph_.modules[module]->name + "." + name], ids);

                    for (const auto* imported : imports(module))
                    {
                        const bool compilerImport = std::any_of(imported->children.begin(), imported->children.end(),
                            [](const auto& node) { return node->kind == "CompilerImport"; });
                        const auto target = graph_.find(imported->value);
                        if (target == InvalidModuleId)
                        {
                            error(*imported, "Imported module '" + imported->value + "' was not loaded.");
                            continue;
                        }
                        state.paths.insert(imported->value);
                        bool selected = false;
                        for (const auto& item : imported->children)
                        {
                            if (item->kind != "ImportName") continue;
                            selected = true;
                            if (item->value == "*")
                            {
                                for (const auto& [name, ids] : modules_[target].exports)
                                {
                                    append(state.imported[name], ids);
                                    append(state.qualified[imported->value + "." + name], ids);
                                }
                            }
                            else
                            {
                                auto ids = exported(target, item->value);
                                if (compilerImport)
                                    if (const auto found = modules_[target].direct.find(item->value);
                                        found != modules_[target].direct.end()) append(ids, found->second);
                                if (ids.empty())
                                {
                                    const bool privateName = modules_[target].direct.contains(item->value);
                                    error(*item, privateName
                                        ? "Cannot import private declaration '" + imported->value + "." + item->value + "'."
                                        : "Module '" + imported->value + "' has no public name '" + item->value + "'.");
                                }
                                if (!compilerImport) append(state.imported[item->value], ids);
                                append(state.qualified[imported->value + "." + item->value], ids);
                                result_.references[item.get()] = std::move(ids);
                            }
                        }
                        if (!selected)
                            for (const auto& [name, ids] : modules_[target].exports)
                                append(state.qualified[imported->value + "." + name], ids);
                    }
                }
            }

            Candidates lookup(const std::string& name, const Context& context) const
            {
                for (auto scope = context.scopes.rbegin(); scope != context.scopes.rend(); ++scope)
                {
                    if (auto found = scope->find(name); found != scope->end()) return found->second;
                }
                Candidates result;
                const auto& module = modules_[context.module];
                if (auto found = module.direct.find(name); found != module.direct.end())
                    append(result, found->second);
                if (auto found = module.imported.find(name); found != module.imported.end())
                    append(result, found->second);
                if (result.empty())
                    if (auto found = prelude_.find(name); found != prelude_.end())
                        return found->second;
                return result;
            }

            bool unambiguous(const Candidates& ids) const
            {
                if (ids.size() <= 1) return true;
                const auto& first = result_.symbols[ids.front()];
                if (first.kind != SymbolKind::Function) return false;
                for (auto id : ids)
                {
                    const auto& symbol = result_.symbols[id];
                    if (symbol.kind != SymbolKind::Function || symbol.module != first.module)
                        return false;
                    const auto leftOwner = owners_.find(first.declaration);
                    const auto rightOwner = owners_.find(symbol.declaration);
                    if ((leftOwner == owners_.end()) != (rightOwner == owners_.end())) return false;
                    if (leftOwner != owners_.end() && leftOwner->second != rightOwner->second) return false;
                }
                return true;
            }

            bool accessible(const ResolvedSymbol& symbol, const Context& context) const
            {
                if (symbol.isPublic) return true;
                const Node* declarationOwner = symbol.declaration;
                while (true)
                {
                    auto owner = owners_.find(declarationOwner);
                    if (owner == owners_.end()) break;
                    declarationOwner = owner->second;
                    if (context.owner == declarationOwner) return true;
                }
                if (declarationOwner != symbol.declaration) return false;
                return symbol.module == context.module;
            }

            Candidates member(Candidates base, const std::string& name,
                              const Node& use, const Context& context)
            {
                if (base.empty() || !unambiguous(base)) return {};
                const auto& symbol = result_.symbols[base.front()];
                if (symbol.kind == SymbolKind::Value || symbol.kind == SymbolKind::Function)
                {
                    // 인스턴스 필드는 실제 수신자 타입을 아는 TypeChecker가 검사한다.
                    // TypeChecker validates instance members once the receiver type is known.
                    return {};
                }
                if (!symbol.declaration || symbol.declaration->kind == "TypeParameter") return {};
                const auto* declaration = canonicalDeclaration(symbol.declaration);
                if (!declaration) return {};
                if (childOf(*declaration, "CompilerSimd"))
                {
                    const Node* path = &use;
                    if (path->kind == "GenericName" && !path->children.empty()) path = path->children[0].get();
                    if (path->kind == "Member" && path->value == name && !path->children.empty())
                        result_.references[path->children[0].get()] = {symbol.id};
                    return {};
                }
                auto owner = members_.find(declaration);
                if (owner == members_.end()) return {};
                auto found = owner->second.find(name);
                if (found == owner->second.end())
                {
                    error(use, "Type '" + symbol.name + "' has no member '" + name + "'.");
                    return {};
                }
                Candidates result;
                for (auto id : found->second)
                {
                    if (accessible(result_.symbols[id], context)) result.push_back(id);
                    else error(use, "Cannot access private member '" + symbol.name + "." + name + "'.");
                }
                return result;
            }

            Candidates resolvePath(const std::vector<std::string>& parts,
                                   const Node& use, const Context& context)
            {
                if (parts.empty()) return {};
                const auto& state = modules_[context.module];
                Candidates result;
                std::size_t consumed = 0;

                for (std::size_t length = parts.size(); length > 1; --length)
                {
                    auto found = state.qualified.find(join(parts, 0, length));
                    if (found == state.qualified.end()) continue;
                    result = found->second;
                    consumed = length;
                    break;
                }

                if (consumed == 0)
                {
                    // 로컬 이름이 경로의 첫 구성요소이면 인스턴스 접근이 우선이다.
                    // A lexical value used as the first component denotes instance access.
                    result = lookup(parts.front(), context);
                    if (result.empty())
                    {
                        for (std::size_t length = parts.size(); length > 1; --length)
                        {
                            const auto modulePath = join(parts, 0, length - 1);
                            if (!state.paths.contains(modulePath)) continue;
                            const auto target = graph_.find(modulePath);
                            const auto name = parts[length - 1];
                            const bool privateName = target != InvalidModuleId
                                && modules_[target].direct.contains(name)
                                && exported(target, name).empty();
                            error(use, privateName
                                ? "Cannot access private declaration '" + modulePath + "." + name + "'."
                                : "Name '" + name + "' is not imported from module '" + modulePath + "'.");
                            return {};
                        }
                        error(use, "Unknown name '" + parts.front() + "'.");
                        return {};
                    }
                    consumed = 1;
                }

                if (!unambiguous(result))
                {
                    error(use, "Ambiguous name '" + join(parts, 0, consumed)
                          + "'; use an imported module-qualified path.");
                    return {};
                }

                for (; consumed < parts.size(); ++consumed)
                    result = member(std::move(result), parts[consumed], use, context);
                return result;
            }

            bool flatten(const Node& node, std::vector<std::string>& parts) const
            {
                if (node.kind == "Name" || node.kind == "Type")
                {
                    parts.push_back(node.value);
                    return true;
                }
                if ((node.kind == "Member" || node.kind == "QualifiedType")
                    && !node.children.empty() && flatten(*node.children.front(), parts))
                {
                    parts.push_back(node.value);
                    return true;
                }
                if (node.kind == "GenericName" && !node.children.empty())
                    return flatten(*node.children.front(), parts);
                return false;
            }

            Candidates pathReference(const Node& node, Context& context, bool typeUse)
            {
                std::vector<std::string> parts;
                if (!flatten(node, parts)) return {};
                auto ids = resolvePath(parts, node, context);
                const Node* base = &node;
                while ((base->kind == "Member" || base->kind == "GenericName"
                        || base->kind == "QualifiedType") && !base->children.empty())
                    base = base->children.front().get();
                if (base->kind == "Name" || base->kind == "Type")
                {
                    auto baseIds = lookup(base->value, context);
                    if (!baseIds.empty() && unambiguous(baseIds))
                        result_.references[base] = std::move(baseIds);
                }
                if (!ids.empty())
                {
                    const auto kind = result_.symbols[ids.front()].kind;
                    if (typeUse && kind != SymbolKind::Type)
                        error(node, "Name '" + join(parts, 0, parts.size()) + "' does not denote a type.");
                    result_.references[&node] = ids;
                }
                return ids;
            }

            void bind(const Node& node, Context& context, SymbolKind kind)
            {
                auto visible = lookup(node.value, context);
                bool conflict = false;
                for (auto id : visible)
                {
                    const auto& existing = result_.symbols[id];
                    if (existing.kind == SymbolKind::Value
                        || (existing.declaration && existing.declaration->kind == "TypeParameter"))
                        conflict = true;
                }
                if (context.scopes.back().contains(node.value)) conflict = true;
                if (conflict)
                    error(node, "Declaration '" + node.value + "' would duplicate or shadow a visible variable.");
                const auto id = addSymbol(&node, context.module, node.value, kind, false);
                context.scopes.back()[node.value].push_back(id);
                result_.references[&node] = {id};
            }

            void generics(const Node& node, Context& context)
            {
                if (const auto* parameters = childOf(node, "GenericParameters"))
                    for (const auto& parameter : parameters->children)
                    {
                        const bool constant = parameter->kind == "ConstParameter";
                        if (constant && !parameter->children.empty()) walkType(*parameter->children[0], context);
                        if (const auto* initial = childOf(*parameter, "GenericDefault"))
                        {
                            for (const auto& value : initial->children)
                            {
                                if (constant) walkExpression(*value, context, false);
                                else walkType(*value, context);
                            }
                        }
                        bind(*parameter, context, constant ? SymbolKind::Value : SymbolKind::Type);
                    }
            }

            void walkGenericArgument(const Node& argument, const Node& owner,
                                     std::size_t index, Context& context)
            {
                std::vector<std::string> parts;
                bool lengthSlot = false;
                if (flatten(owner, parts))
                {
                    if (index == 1 && parts.size() == 1 && parts.front() == "Array")
                    {
                        const auto ids = lookup("Array", context);
                        lengthSlot = ids.size() == 1 && result_.symbols[ids.front()].module == InvalidModuleId;
                    }
                    const auto reference = result_.references.find(&owner);
                    if (reference != result_.references.end() && reference->second.size() == 1)
                    {
                        const auto* declaration = result_.symbols[reference->second[0]].declaration;
                        if (declaration && childOf(*declaration, "CompilerSimd"))
                            lengthSlot = index == (declaration->value == "SimdMask" ? 0u : 1u);
                    }
                    if (!lengthSlot && owner.kind == "Member" && !owner.children.empty())
                    {
                        const auto receiver = result_.references.find(owner.children[0].get());
                        if (receiver != result_.references.end() && receiver->second.size() == 1)
                        {
                            const auto* declaration = result_.symbols[receiver->second[0]].declaration;
                            if (declaration && childOf(*declaration, "CompilerSimd")) lengthSlot = index == 1;
                        }
                    }
                }

                if (!lengthSlot)
                    if (const auto known = result_.references.find(&owner); known != result_.references.end() && !known->second.empty())
                    {
                        lengthSlot = true;
                        for (const auto id : known->second)
                        {
                            const auto* declaration = result_.symbols[id].declaration;
                            const auto* parameters = declaration ? childOf(*declaration, "GenericParameters") : nullptr;
                            if (!parameters || index >= parameters->children.size() || parameters->children[index]->kind != "ConstParameter")
                                lengthSlot = false;
                        }
                    }
                // Array의 두 번째 인수는 타입이 아니라 상수 길이 이름이다.
                // Array's second argument names a constant extent rather than a type.
                if (lengthSlot && (argument.kind == "Type" || argument.kind == "QualifiedType"))
                {
                    auto ids = pathReference(argument, context, false);
                    if (!ids.empty() && result_.symbols[ids.front()].kind != SymbolKind::Value)
                        error(argument, "A collection or SIMD extent must name a value, not a type or function.");
                    walkTypeArguments(argument, context);
                }
                else walkType(argument, context);
            }

            void walkTypeArguments(const Node& node, Context& context)
            {
                if (node.kind == "Type")
                {
                    for (std::size_t i = 0; i < node.children.size(); ++i)
                        walkGenericArgument(*node.children[i], node, i, context);
                    return;
                }
                if (node.kind == "QualifiedType")
                {
                    if (!node.children.empty()) walkTypeArguments(*node.children.front(), context);
                    for (std::size_t i = 1; i < node.children.size(); ++i)
                        walkGenericArgument(*node.children[i], node, i - 1, context);
                    return;
                }
                if (node.kind == "GenericName" || node.kind == "Member")
                {
                    if (!node.children.empty()) walkTypeArguments(*node.children.front(), context);
                    for (std::size_t i = 1; i < node.children.size(); ++i)
                        if (node.children[i]->kind == "TypeArguments")
                            for (std::size_t j = 0; j < node.children[i]->children.size(); ++j)
                                walkGenericArgument(*node.children[i]->children[j], *node.children.front(), j, context);
                }
            }

            void walkType(const Node& node, Context& context)
            {
                if (node.kind == "ConstantTypeArgument")
                { for (const auto& expression : node.children) walkExpression(*expression, context, false); return; }
                if (node.kind == "InferredType") return;
                if (node.kind == "ReferenceType" || node.kind == "DynamicType")
                {
                    for (const auto& child : node.children) walkType(*child, context);
                    return;
                }
                if (node.kind == "Type" && node.value == "Self")
                {
                    if (!context.owner)
                        error(node, "Self is only available inside a type declaration.");
                    else
                        result_.references[&node] = {declarationIds_.at(context.owner)};
                }
                else pathReference(node, context, true);
                walkTypeArguments(node, context);
            }

            // 선행 별칭의 대상도 선언 위치의 모듈에서 해석한다.
            // Resolve forward alias targets in their declaration module.
            void resolveAlias(const Node& node, Context& context)
            {
                if (resolvedAliases_.contains(&node) || node.children.empty()) return;
                if (!resolvingAliases_.insert(&node).second) return;
                if (resolvingAliases_.size() > 256)
                    error(node, "Type alias expansion exceeds the limit of 256.");
                else
                    walkType(*node.children.front(), context);
                resolvingAliases_.erase(&node);
                resolvedAliases_.insert(&node);
            }

            const Node* canonicalDeclaration(const Node* declaration)
            {
                std::set<const Node*> visited;
                while (declaration && declaration->kind == "TypeAlias")
                {
                    if (!visited.insert(declaration).second) return nullptr;
                    if (!resolvedAliases_.contains(declaration))
                    {
                        Context context{result_.symbols[declarationIds_.at(declaration)].module,
                                        nullptr, {SymbolTable{}}};
                        if (const auto owner = owners_.find(declaration); owner != owners_.end())
                        {
                            context.owner = owner->second;
                            context.scopes.back()["Self"] = {declarationIds_.at(owner->second)};
                            if (const auto* parameters = childOf(*owner->second, "GenericParameters"))
                                for (const auto& parameter : parameters->children)
                                    if (const auto id = declarationIds_.find(parameter.get()); id != declarationIds_.end())
                                        context.scopes.back()[parameter->value] = {id->second};
                        }
                        resolveAlias(*declaration, context);
                    }
                    if (declaration->children.empty()) return nullptr;
                    const auto target = result_.references.find(declaration->children.front().get());
                    if (target == result_.references.end() || target->second.size() != 1) return nullptr;
                    declaration = result_.symbols[target->second.front()].declaration;
                }
                return declaration;
            }

            void validateAliasCycles()
            {
                std::map<const Node*, unsigned> states;
                std::vector<const Node*> stack;
                const auto visit = [&](const auto& self, const Node& node) -> void
                {
                    if (states[&node] == 2) return;
                    if (states[&node] == 1)
                    {
                        std::string cycle;
                        const auto begin = std::find(stack.begin(), stack.end(), &node);
                        for (auto item = begin; item != stack.end(); ++item)
                        {
                            if (!cycle.empty()) cycle += " -> ";
                            cycle += (*item)->value;
                        }
                        error(node, "Cyclic type alias: " + cycle + " -> " + node.value + ".");
                        return;
                    }
                    if (stack.size() >= 256)
                    {
                        error(node, "Type alias expansion exceeds the limit of 256.");
                        return;
                    }
                    states[&node] = 1;
                    stack.push_back(&node);
                    const auto inspect = [&](const auto& inspectSelf, const Node& syntax) -> void
                    {
                        if (const auto reference = result_.references.find(&syntax);
                            reference != result_.references.end())
                            for (const auto id : reference->second)
                            {
                                const auto* target = result_.symbols[id].declaration;
                                if (target && target->kind == "TypeAlias") self(self, *target);
                            }
                        for (const auto& nested : syntax.children) inspectSelf(inspectSelf, *nested);
                    };
                    if (!node.children.empty()) inspect(inspect, *node.children.front());
                    stack.pop_back();
                    states[&node] = 2;
                };
                for (const auto& symbol : result_.symbols)
                    if (symbol.declaration && symbol.declaration->kind == "TypeAlias")
                        visit(visit, *symbol.declaration);
            }

            std::string canonicalSyntaxKey(const Node& node,
                const std::map<std::string, std::string>& parameters,
                std::set<const Node*>& aliases) const
            {
                if (node.kind == "Type" || node.kind == "QualifiedType")
                    if (const auto reference = result_.references.find(&node);
                        reference != result_.references.end() && reference->second.size() == 1)
                    {
                        const auto& symbol = result_.symbols[reference->second.front()];
                        if (symbol.declaration && symbol.declaration->kind == "TypeAlias")
                        {
                            if (!aliases.insert(symbol.declaration).second) return "<cyclic>";
                            auto key = canonicalSyntaxKey(*symbol.declaration->children.front(), parameters, aliases);
                            aliases.erase(symbol.declaration);
                            return key;
                        }
                        if (symbol.declaration && symbol.declaration->kind != "TypeParameter")
                        {
                            std::string key = "nominal:" + std::to_string(symbol.id);
                            const std::size_t first = node.kind == "QualifiedType" ? 1 : 0;
                            for (std::size_t i = first; i < node.children.size(); ++i)
                                key += "," + canonicalSyntaxKey(*node.children[i], parameters, aliases);
                            return key;
                        }
                    }
                auto value = node.value;
                if (node.kind == "Type")
                {
                    if (const auto found = parameters.find(value); found != parameters.end()) value = found->second;
                    if (value == "int") value = "i32";
                    if (value == "float") value = "f64";
                }
                std::string key = node.kind + "(" + value;
                for (const auto& nested : node.children)
                    if (nested->kind != "Modifier" && nested->kind != "Default")
                        key += "," + canonicalSyntaxKey(*nested, parameters, aliases);
                return key + ")";
            }

            std::string canonicalSignature(const Node& function) const
            {
                std::map<std::string, std::string> parameters;
                if (const auto* generics = childOf(function, "GenericParameters"))
                    for (std::size_t i = 0; i < generics->children.size(); ++i)
                        parameters[generics->children[i]->value] = "$" + std::to_string(i);
                std::string key = std::to_string(parameters.size()) + "(";
                std::set<const Node*> aliases;
                if (const auto* arguments = childOf(function, "Parameters"))
                    for (const auto& argument : arguments->children)
                    {
                        key += argument->kind == "VariadicParameter" ? "..." : "";
                        for (const auto& nested : argument->children)
                            if (nested->kind != "Modifier" && nested->kind != "Default")
                                key += canonicalSyntaxKey(*nested, parameters, aliases);
                        key += ";";
                    }
                return key + ")";
            }

            void validateAliasOverloads()
            {
                // 별칭 철자가 달라도 같은 매개변수 타입이면 중복 오버로드다.
                // Different alias spellings still denote duplicate parameter types.
                const auto inspect = [&](const SymbolTable& table)
                {
                    for (const auto& [name, candidates] : table)
                        for (std::size_t i = 0; i < candidates.size(); ++i)
                        {
                            const auto& left = result_.symbols[candidates[i]];
                            if (left.kind != SymbolKind::Function) continue;
                            for (std::size_t j = 0; j < i; ++j)
                            {
                                const auto& right = result_.symbols[candidates[j]];
                                if (right.kind != SymbolKind::Function
                                    || signature(*left.declaration) == signature(*right.declaration)) continue;
                                if (canonicalSignature(*left.declaration) == canonicalSignature(*right.declaration))
                                {
                                    error(*left.declaration, "Duplicate overload signature for '" + name
                                        + "' after transparent type alias expansion.");
                                    break;
                                }
                            }
                        }
                };
                for (const auto& module : modules_) inspect(module.direct);
                for (const auto& [owner, members] : members_) { (void)owner; inspect(members); }
            }

            void walkDeclaration(const Node& node, Context& context, bool local)
            {
                if (node.kind == "TypeAlias")
                {
                    resolveAlias(node, context);
                    return;
                }
                if (typeDeclaration(node))
                {
                    Context nested = context;
                    nested.owner = &node;
                    nested.scopes.emplace_back();
                    nested.scopes.back()["Self"] = {declarationIds_.at(&node)};
                    generics(node, nested);
                    for (const auto& child : node.children)
                    {
                        if (declaration(*child)) walkDeclaration(*child, nested, false);
                        else if (child->kind == "Implements" || child->kind == "Constraint")
                            for (const auto& type : child->children) walkType(*type, nested);
                    }
                    return;
                }
                if (node.kind == "Variant")
                {
                    for (const auto& child : node.children)
                        if (declaration(*child)) walkDeclaration(*child, context, false);
                    return;
                }
                if (node.kind == "Function")
                {
                    Context function = context;
                    function.scopes.emplace_back();
                    generics(node, function);
                    if (!node.children.empty()) walkType(*node.children.front(), function);
                    if (const auto* parameters = childOf(node, "Parameters"))
                    {
                        for (const auto& parameter : parameters->children)
                        {
                            for (const auto& child : parameter->children)
                            {
                                if (child->kind == "Default")
                                    for (const auto& value : child->children) walkExpression(*value, function, false);
                                else if (child->kind == "ArraySuffixes") walkStatement(*child, function);
                                else if (child->kind != "Modifier") walkType(*child, function);
                            }
                            bind(*parameter, function, SymbolKind::Value);
                        }
                    }
                    for (const auto& child : node.children)
                    {
                        if (child->kind == "Constraint" || child->kind == "Implements")
                            for (const auto& type : child->children) walkType(*type, function);
                        else if (child->kind == "Block") walkStatement(*child, function);
                    }
                    return;
                }
                if (node.kind == "Variable")
                {
                    if (!node.children.empty()) walkType(*node.children.front(), context);
                    for (std::size_t i = 1; i < node.children.size(); ++i)
                    {
                        const auto& child = *node.children[i];
                        if (child.kind == "Modifier") continue;
                        if (child.kind == "ArraySuffixes") walkStatement(child, context);
                        else walkExpression(child, context, false);
                    }
                    if (local) bind(node, context, SymbolKind::Value);
                }
            }

            bool typeOperandSlot(const Node& callee, std::size_t index, Context& context)
            {
                if (index != 0) return false;
                std::vector<std::string> parts;
                if (!flatten(callee, parts) || parts.size() != 2) return false;
                const auto ids = lookup(parts.front(), context);
                if (ids.size() != 1 || result_.symbols[ids.front()].module != InvalidModuleId)
                    return false;
                static const std::map<std::string, std::set<std::string>> catalog = {
                    {"Integer", {"bit_width", "byte_width", "is_signed", "min_value", "max_value", "zero", "one", "low_mask",
                                 "convert", "saturating_convert", "wrapping_convert", "widen", "fits", "bitcast", "to_float",
                                 "to_float_exact", "parse", "parse_prefix", "parse_radix", "from_be_bytes", "from_le_bytes", "read_be", "read_le"}},
                    {"Float", {"bit_width", "byte_width", "decimal_digits", "exponent_bits", "fraction_bits", "precision_bits", "max_digits10",
                               "max_exact_integer", "min_exponent", "max_exponent", "min_exponent10", "max_exponent10", "min_value", "max_value",
                               "min_positive_normal", "min_positive_subnormal", "supports_infinity", "supports_nan", "supports_subnormal", "radix",
                               "pi", "tau", "e", "epsilon", "nan", "infinity", "neg_infinity", "positive_zero", "negative_zero", "from_bits",
                               "from_integer", "from_integer_exact", "from_be_bytes", "from_le_bytes", "from_ne_bytes", "read_be", "read_le",
                               "parse", "parse_exact", "parse_hex", "parse_with", "sum", "sum_compensated", "sum_precise", "mean", "product", "dot"}},
                    {"Array", {"alignment", "element_size", "max_len", "empty", "default", "from_copy", "from_clone", "from_fn",
                               "try_from_fn", "from_slice_copy", "from_slice_clone", "from_values"}},
                    {"Vector", {"new", "with_capacity", "max_len", "from_fn", "try_from_fn", "repeat_copy", "repeat_clone"}},
                    {"Slice", {"empty"}}
                };
                auto owner = catalog.find(parts.front());
                return owner != catalog.end() && owner->second.contains(parts.back());
            }

            void walkExpression(const Node& node, Context& context, bool typeOperand)
            {
                if (node.kind == "Closure")
                {
                    const auto* captures = childOf(node, "Captures");
                    std::set<std::string> selected;
                    if (captures) for (const auto& capture : captures->children)
                    {
                        if (!selected.insert(capture->value).second) error(*capture, "A closure capture cannot appear more than once.");
                        if (capture->children.size() > 1) walkExpression(*capture->children[1], context, false);
                    }
                    Context function = context; function.scopes.emplace_back();
                    if (const auto* parameters = childOf(node, "Parameters"))
                        for (const auto& parameter : parameters->children)
                        {
                            if (!parameter->children.empty()) walkType(*parameter->children[0], function);
                            bind(*parameter, function, SymbolKind::Value);
                        }
                    if (node.children.size() > 2) walkType(*node.children[2], function);
                    if (const auto* body = childOf(node, "Block")) walkStatement(*body, function);
                    return;
                }
                if (node.kind == "CompilerArrayResult") { for (const auto& value : node.children) walkType(*value, context); return; }
                if (node.kind == "Literal" || node.kind == "ConstantTypeArgument") return;
                if (node.kind == "CompilerBoxExpression" || node.kind == "CompilerNumericExpression"
                    || node.kind == "CompilerTextExpression" || node.kind == "CompilerSliceExpression")
                {
                    if (!node.children.empty()) walkType(*node.children[0], context);
                    for (std::size_t i = 1; i < node.children.size(); ++i)
                        walkExpression(*node.children[i], context, false);
                    return;
                }
                if (node.kind == "Name" || node.kind == "Member" || node.kind == "GenericName")
                {
                    std::vector<std::string> parts;
                    Candidates ids;
                    if (flatten(node, parts)) ids = pathReference(node, context, false);
                    else if (!node.children.empty()) walkExpression(*node.children.front(), context, false);
                    walkTypeArguments(node, context);
                    if (!typeOperand && !ids.empty()
                        && result_.symbols[ids.front()].kind == SymbolKind::Type)
                        error(node, "Type '" + join(parts, 0, parts.size()) + "' is not a runtime value.");
                    return;
                }
                if (node.kind == "Call")
                {
                    if (node.children.empty()) return;
                    const auto& callee = *node.children.front();
                    walkExpression(callee, context, true);
                    for (std::size_t i = 1; i < node.children.size(); ++i)
                    {
                        if (node.children[i]->kind == "CompilerCollectionResult" || node.children[i]->kind == "CompilerRangeResult") { walkType(*node.children[i]->children[0], context); continue; }
                        const bool operand = typeOperandSlot(callee, i - 1, context);
                        for (const auto& value : node.children[i]->children)
                            walkExpression(*value, context, operand);
                    }
                    return;
                }
                if (node.kind == "TypeOperandCandidate")
                {
                    if (!typeOperand)
                        error(node, "Compile-time type operands are only allowed in documented intrinsic argument slots.");
                    for (const auto& type : node.children) walkType(*type, context);
                    return;
                }
                if (node.kind == "NamedInitializer")
                {
                    if (node.children.empty()) return;
                    walkExpression(*node.children.front(), context, true);
                    auto found = result_.references.find(node.children.front().get());
                    Candidates ownerIds = found == result_.references.end() ? Candidates{} : found->second;
                    std::set<std::string> fields;
                    for (std::size_t i = 1; i < node.children.size(); ++i)
                    {
                        const auto& field = *node.children[i];
                        if (!fields.insert(field.value).second)
                            error(field, "Repeated field initializer '" + field.value + "'.");
                        if (!ownerIds.empty())
                        {
                            const auto& symbol = result_.symbols[ownerIds.front()];
                            auto owner = members_.find(canonicalDeclaration(symbol.declaration));
                            if (owner != members_.end())
                            {
                                auto member = owner->second.find(field.value);
                                if (member == owner->second.end())
                                    error(field, "Unknown initializer field '" + field.value + "'.");
                                else
                                    for (auto id : member->second)
                                        if (!accessible(result_.symbols[id], context))
                                            error(field, "Cannot initialize private field '" + field.value + "'.");
                            }
                        }
                        for (const auto& value : field.children) walkExpression(*value, context, false);
                    }
                    return;
                }
                for (const auto& child : node.children)
                    walkExpression(*child, context, typeOperand && node.kind == "Group");
            }

            void walkStatement(const Node& node, Context& context)
            {
                if (declaration(node))
                {
                    walkDeclaration(node, context, true);
                    return;
                }
                if (node.kind == "Block" || node.kind == "CaseScope")
                {
                    context.scopes.emplace_back();
                    for (const auto& child : node.children) walkStatement(*child, context);
                    context.scopes.pop_back();
                    return;
                }
                if (node.kind == "RangeFor" || node.kind == "EnhancedFor")
                {
                    if (node.children.size() != 3) return;
                    walkExpression(*node.children[1], context, false);
                    context.scopes.emplace_back();
                    const auto& binding = *node.children.front();
                    for (const auto& type : binding.children)
                        if (type->kind != "Modifier") walkType(*type, context);
                    bind(binding, context, SymbolKind::Value);
                    walkStatement(*node.children[2], context);
                    context.scopes.pop_back();
                    return;
                }
                if (node.kind == "For")
                {
                    context.scopes.emplace_back();
                    for (const auto& child : node.children) walkStatement(*child, context);
                    context.scopes.pop_back();
                    return;
                }
                if (node.kind == "Case" || node.kind == "Default")
                {
                    context.scopes.emplace_back();
                    const Node* variant = nullptr;
                    std::set<std::string> boundFields;
                    for (const auto& child : node.children)
                    {
                        if (child->kind == "PatternBindings")
                        {
                            for (const auto& binding : child->children)
                            {
                                if (!binding->children.empty()) walkType(*binding->children.front(), context);
                                if (variant)
                                {
                                    for (const auto& field : binding->children)
                                    {
                                        if (field->kind != "PatternField") continue;
                                        std::string fieldName = field->value;
                                        if (fieldName.starts_with("&mut ")) fieldName.erase(0, 5);
                                        else if (fieldName.starts_with("&")) fieldName.erase(0, 1);
                                        if (!boundFields.insert(fieldName).second)
                                            error(*field, "Repeated pattern field '" + fieldName + "'.");
                                        auto owner = members_.find(variant);
                                        if (owner == members_.end() || !owner->second.contains(fieldName))
                                            error(*field, "Unknown enum payload field '" + fieldName + "'.");
                                        else
                                            for (auto id : owner->second.at(fieldName))
                                            {
                                                if (!accessible(result_.symbols[id], context))
                                                    error(*field, "Cannot bind private enum payload field '" + fieldName + "'.");
                                                else result_.references[field.get()].push_back(id);
                                            }
                                    }
                                }
                                bind(*binding, context, SymbolKind::Value);
                            }
                        }
                        else if (child->kind == "CaseScope")
                        {
                            // 패턴 변수와 분기 본문은 같은 case 범위에 속한다.
                            // Pattern bindings and the branch body share the same case scope.
                            for (const auto& statement : child->children) walkStatement(*statement, context);
                        }
                        else
                        {
                            walkExpression(*child, context, false);
                            auto reference = result_.references.find(child.get());
                            if (reference != result_.references.end() && reference->second.size() == 1)
                            {
                                const auto* label = result_.symbols[reference->second.front()].declaration;
                                if (label && label->kind == "Variant") variant = label;
                            }
                        }
                    }
                    context.scopes.pop_back();
                    return;
                }
                if (node.kind == "If" || node.kind == "While")
                {
                    if (!node.children.empty()) walkExpression(*node.children.front(), context, false);
                    for (std::size_t i = 1; i < node.children.size(); ++i) walkStatement(*node.children[i], context);
                    return;
                }
                if (node.kind == "Switch")
                {
                    if (!node.children.empty()) walkExpression(*node.children.front(), context, false);
                    for (std::size_t i = 1; i < node.children.size(); ++i) walkStatement(*node.children[i], context);
                    return;
                }
                if (node.kind == "ArraySuffixes" || node.kind == "ArraySuffix" || node.kind == "RankSuffix")
                {
                    for (const auto& child : node.children) walkStatement(*child, context);
                    return;
                }
                walkExpression(node, context, false);
            }
        };
    }

    NameResolver::NameResolver(Diagnostics& diagnostics)
        : diagnostics_(diagnostics) {}

    NameResolution NameResolver::resolve(const ModuleGraph& graph)
    {
        return Resolver(graph, diagnostics_).run();
    }
}
