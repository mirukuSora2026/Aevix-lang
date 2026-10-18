// ========================
//
// Created By : miruku_sora2026
//
// File name : GenericResolver.cpp
//
// Description :
//     제네릭을 제한된 수의 구체 선언으로 확장하고 정적 인터페이스 계약을 검증한다.
//     Expand generics into bounded concrete declarations and validate static interface contracts.
//
// ========================

#include "aevix/Frontend/Sema/GenericResolver.hpp"
#include "aevix/Native/AtomicOperation.hpp"
#include "aevix/Native/SyncOperation.hpp"
#include "aevix/Native/TaskOperation.hpp"
#include "aevix/Frontend/Lexer/LiteralText.hpp"
#include "aevix/Frontend/Sema/NumericLiteral.hpp"
#include "aevix/Frontend/Sema/ConstEvaluator.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace aevix
{
    namespace
    {
        const Node* child(const Node& node, std::string_view kind)
        {
            for (const auto& item : node.children) if (item->kind == kind) return item.get();
            return nullptr;
        }
        Node* child(Node& node, std::string_view kind)
        {
            for (auto& item : node.children) if (item->kind == kind) return item.get();
            return nullptr;
        }
        bool modifier(const Node& node, std::string_view flag)
        {
            const auto* flags = child(node, "Modifier");
            return flags && (" " + flags->value + " ").find(" " + std::string(flag) + " ") != std::string::npos;
        }
        NodePtr makeGenericNode(std::string kind, std::string value, SourceRange range)
        {
            auto result = std::make_unique<Node>();
            result->kind = std::move(kind); result->value = std::move(value); result->range = range;
            return result;
        }
        bool typeDeclaration(const Node& item)
        {
            return item.kind == "Struct" || item.kind == "Enum" || item.kind == "Interface";
        }
        bool generic(const Node& item)
        {
            const auto* parameters = child(item, "GenericParameters");
            return parameters && !parameters->children.empty();
        }
        std::string spelling(const Node& item)
        {
            if (item.kind == "ReferenceType") return "&" + item.value + spelling(*item.children[0]);
            if (item.kind == "QualifiedType" || item.kind == "Member")
                return spelling(*item.children[0]) + "." + item.value;
            std::string result = item.value == "int" ? "i32" : item.value == "float" ? "f64" : item.value;
            if (!item.children.empty())
            {
                result += '<';
                for (const auto& argument : item.children) result += spelling(*argument) + ',';
                result += '>';
            }
            return result;
        }
        std::string flatPath(const Node& item)
        {
            if (item.kind == "GenericName") return flatPath(*item.children[0]);
            if (item.kind == "QualifiedType" || item.kind == "Member") return flatPath(*item.children[0]) + "." + item.value;
            return item.value;
        }
        struct Declaration
        {
            Node* node = nullptr;
            ModuleId module = InvalidModuleId;
            SymbolId symbol = InvalidSymbolId;
            Node* owner = nullptr;
        };
        using Bindings = std::map<std::string, NodePtr>;
        constexpr std::size_t maximumGeneratedDeclarations = 4096;

        class Resolver final
        {
        public:
            Resolver(ModuleGraph& graph, const NameResolution& names, Diagnostics& diagnostics)
                : graph_(graph), names_(names), diagnostics_(diagnostics), initial_(diagnostics.size())
            {
                for (const auto& symbol : names.symbols)
                    if (symbol.declaration)
                    {
                        declarations_[symbol.declaration] = {const_cast<Node*>(symbol.declaration), symbol.module, symbol.id, nullptr};
                        symbols_[symbol.id] = symbol.declaration;
                    }
                const auto rememberOriginal = [&](const auto& self, const Node& value) -> void
                {
                    originalNodes_.insert(&value);
                    if (value.kind == "Name" || value.kind == "Type" || value.kind == "Member"
                        || value.kind == "QualifiedType" || value.kind == "GenericName")
                        referencePaths_[&value] = flatPath(value);
                    for (const auto& nested : value.children) self(self, *nested);
                };
                for (ModuleId module = 0; module < graph.modules.size(); ++module)
                {
                    rememberOriginal(rememberOriginal, *graph.modules[module]->ast);
                    index(*graph.modules[module]->ast, module, nullptr);
                }
                for (const auto& [expression, ids] : names.references)
                {
                    (void)ids;
                    referencePaths_[expression] = flatPath(*expression);
                }
            }
            std::optional<NameResolution> run()
            {
                // 사용하지 않은 제네릭 선언도 봉인된 내장 형식 계약을 구현할 수 없다.
                // Even uninstantiated templates cannot implement sealed builtin format contracts.
                for (const auto& [address, declaration] : declarations_)
                {
                    (void)address;
                    validateLifecycleConstraints(*declaration.node);
                    if (declaration.node->kind != "Struct" && declaration.node->kind != "Enum") continue;
                    validateDropContract(*declaration.node, declaration.module);
                    if (const auto* implemented = child(*declaration.node, "Implements"))
                        for (const auto& requirement : implemented->children)
                            if (floatingContract(*requirement, declaration.module))
                                error(*requirement, "Sealed numeric format contracts can only be implemented by builtin scalar formats.");
                }
                if (diagnostics_.size() != initial_) return std::nullopt;
                for (const auto& [address, declaration] : declarations_)
                {
                    (void)address;
                    if (declaration.node->kind == "Function") rewriteCallableTemplate(*declaration.node, declaration.owner);
                }
                for (const auto& [address, declaration] : declarations_)
                {
                    (void)address;
                    if (declaration.node->kind == "Function" && (generic(*declaration.node) || (declaration.owner && generic(*declaration.owner))))
                        validateTemplateContract(declaration);
                }
                if (diagnostics_.size() != initial_) return std::nullopt;
                for (auto& [address, declaration] : declarations_)
                {
                    (void)address;
                    if (generic(*declaration.node)) mark(*declaration.node, "CompilerTemplate");
                    if (declaration.node->kind == "Interface") mark(*declaration.node, "CompilerContract");
                }
                for (ModuleId module = 0; module < graph_.modules.size(); ++module)
                    for (std::size_t index = 0; index < graph_.modules[module]->ast->children.size(); ++index)
                    {
                        auto* declaration = graph_.modules[module]->ast->children[index].get();
                        if (declaration->kind != "Import") process(*declaration, module, {}, nullptr, 0);
                    }
                // 기본 구현을 복사한 뒤 새 호출도 같은 구체화 경로로 처리한다.
                // Process newly copied default bodies through the same specialization path.
                std::size_t previousGeneration;
                do
                {
                    previousGeneration = generated_;
                    for (ModuleId module = 0; module < graph_.modules.size(); ++module)
                        for (std::size_t index = 0; index < graph_.modules[module]->ast->children.size(); ++index)
                        {
                            auto* declaration = graph_.modules[module]->ast->children[index].get();
                            if ((declaration->kind == "Struct" || declaration->kind == "Enum") && !child(*declaration, "CompilerTemplate"))
                                validateInterfaces(*declaration, module);
                        }
                    for (ModuleId module = 0; module < graph_.modules.size(); ++module)
                        for (std::size_t index = 0; index < graph_.modules[module]->ast->children.size(); ++index)
                        {
                            auto* declaration = graph_.modules[module]->ast->children[index].get();
                            if (declaration->kind != "Import") process(*declaration, module, {}, nullptr, 0);
                        }
                } while (generated_ != previousGeneration && diagnostics_.size() == initial_);
                if (diagnostics_.size() != initial_) return std::nullopt;
                auto answer = NameResolver(diagnostics_).resolve(graph_);
                if (!answer.succeeded || diagnostics_.size() != initial_) return std::nullopt;
                return answer;
            }

        private:
            ModuleGraph& graph_;
            const NameResolution& names_;
            Diagnostics& diagnostics_;
            std::size_t initial_;
            std::unordered_map<const Node*, Declaration> declarations_;
            std::unordered_map<SymbolId, const Node*> symbols_;
            std::unordered_map<const Node*, const Node*> origins_;
            std::unordered_map<const Node*, std::string> referencePaths_;
            std::map<std::string, Declaration> specializations_;
            std::unordered_map<const Node*, std::string> canonicalKeys_;
            std::unordered_map<const Node*, Node*> selfOwners_;
            std::unordered_set<const Node*> rewritten_;
            std::unordered_set<const Node*> synthesized_;
            std::unordered_set<const Node*> originalNodes_;
            std::unordered_set<const Node*> validated_;
            std::unordered_set<const Node*> validating_;
            std::size_t generated_ = 0;
            std::size_t closureSequence_ = 0;
            const Node* expected_ = nullptr;
            const Node* functionResult_ = nullptr;
            std::unordered_map<const Node*, std::pair<NodePtr, ModuleId>> logicalTypes_;
            std::unordered_set<const Node*> contractsChecked_;
            struct ExpectedGuard
            {
                const Node*& slot; const Node* previous;
                explicit ExpectedGuard(const Node*& target) : slot(target), previous(target) {}
                ~ExpectedGuard() { slot = previous; }
            };

            NodePtr node(std::string kind, std::string value, SourceRange range)
            {
                auto result = makeGenericNode(std::move(kind), std::move(value), range);
                // 임시 추론 노드의 주소 재사용이 이전 원본 정보를 되살리지 않게 한다.
                // Address reuse by temporary inference nodes must not revive stale provenance.
                origins_.erase(result.get()); selfOwners_.erase(result.get()); rewritten_.erase(result.get());
                synthesized_.insert(result.get());
                return result;
            }
            void error(const Node& location, std::string message)
            {
                diagnostics_.push_back({location.range, std::move(message)});
            }
            bool implementsDrop(const Node& owner) const
            {
                const auto* implemented = child(owner, "Implements");
                return implemented && std::any_of(implemented->children.begin(), implemented->children.end(),
                    [](const auto& marker) { return marker->value == "Drop"; });
            }
            void validateLifecycleConstraints(const Node& declaration)
            {
                std::map<std::string, std::pair<bool, bool>> markers;
                for (const auto& item : declaration.children)
                    if (item->kind == "Constraint" && item->children.size() >= 2)
                        for (std::size_t index = 1; index < item->children.size(); ++index)
                        {
                            const auto& marker = *item->children[index];
                            auto& selected = markers[item->children[0]->value];
                            if (marker.value == "Copy") selected.first = true;
                            if (marker.value == "Drop")
                            {
                                selected.second = true;
                                if (!marker.children.empty()) error(marker, "Drop is a lifecycle marker without type arguments.");
                            }
                        }
                for (const auto& [name, selected] : markers)
                {
                    (void)name;
                    if (selected.first && selected.second) error(declaration, "A generic type parameter cannot require both Copy and Drop.");
                }
            }
            void validateDropContract(const Node& owner, ModuleId module)
            {
                const auto* implemented = child(owner, "Implements");
                if (!implemented || !implementsDrop(owner)) return;
                unsigned markers = 0;
                for (const auto& marker : implemented->children)
                {
                    if (marker->value == "Copy") error(owner, "A type cannot implement both Copy and Drop.");
                    if (marker->value == "Drop")
                    {
                        ++markers;
                        if (!marker->children.empty()) error(*marker, "Drop is a lifecycle marker without type arguments.");
                    }
                    else if (marker->value != "Copy")
                    {
                        const auto contract = declaration(*marker, module);
                        if (contract.node && contract.node->kind == "Interface")
                            for (const auto& member : contract.node->children)
                                if (member->kind == "Function" && member->value == "drop")
                                    error(*marker, "A Drop cleanup method cannot implement a callable interface method named drop.");
                    }
                }
                if (markers != 1) error(owner, "A type must list the Drop lifecycle marker exactly once.");
                const Node* method = nullptr;
                unsigned methods = 0;
                for (const auto& member : owner.children)
                    if (member->kind == "Function" && member->value == "drop") { method = member.get(); ++methods; }
                if (methods != 1)
                {
                    error(owner, "Drop requires exactly one public void drop(&mut Self self) method.");
                    return;
                }
                bool valid = modifier(*method, "public") && !modifier(*method, "private")
                    && !modifier(*method, "override") && !modifier(*method, "mut")
                    && !modifier(*method, "const") && !modifier(*method, "persist") && !modifier(*method, "extern")
                    && !child(*method, "Constraint") && child(*method, "Block")
                    && !method->children.empty() && key(*method->children[0], module) == "void";
                if (const auto* generics = child(*method, "GenericParameters"); generics && !generics->children.empty()) valid = false;
                const auto* parameters = child(*method, "Parameters");
                if (!parameters || parameters->children.size() != 1) valid = false;
                else
                {
                    const auto& self = *parameters->children[0];
                    valid = valid && self.kind == "Parameter" && self.value == "self"
                        && !child(self, "Default") && !child(self, "ArraySuffixes") && !child(self, "Modifier")
                        && self.children.size() == 1 && self.children[0]->kind == "ReferenceType"
                        && self.children[0]->value == "mut" && self.children[0]->children.size() == 1;
                    if (valid)
                    {
                        const auto& receiver = *self.children[0]->children[0];
                        auto canonical = portableType(receiver, module, module);
                        const auto target = declaration(*canonical, module);
                        valid = target.node == &owner;
                        const auto arguments = typeArguments(*canonical);
                        const auto* generics = child(owner, "GenericParameters");
                        if (receiver.value == "Self") valid = valid && receiver.children.empty();
                        else if (generics)
                        {
                            valid = valid && arguments.size() == generics->children.size();
                            if (valid)
                                for (std::size_t index = 0; index < arguments.size(); ++index)
                                    valid = valid && arguments[index]->value == generics->children[index]->value
                                        && arguments[index]->children.empty();
                        }
                        else valid = valid && arguments.empty();
                    }
                }
                if (!valid) error(*method, "Drop requires the exact public void drop(&mut Self self) contract without generic, variadic, default, or extra parameters.");
            }
            void mark(Node& declaration, std::string kind)
            {
                if (!child(declaration, kind)) declaration.children.push_back(node(std::move(kind), "", declaration.range));
            }
            void index(Node& declaration, ModuleId module, Node* owner, Node* function = nullptr)
            {
                if (owner && declaration.value == "Self") selfOwners_[&declaration] = owner;
                if (typeDeclaration(declaration) || declaration.kind == "Function")
                {
                    auto found = declarations_.find(&declaration);
                    if (found == declarations_.end()) declarations_[&declaration] = {&declaration, module, InvalidSymbolId, owner};
                    else found->second.owner = owner;
                }
                // 선행 해석의 필드·항목·지역 심벌도 실제 소유 범위를 보존해야 한다.
                // Preserve lexical owners of original fields, variants, and locals in fallback lookup.
                if (!typeDeclaration(declaration) && declaration.kind != "Function")
                    if (const auto found = declarations_.find(&declaration); found != declarations_.end())
                        found->second.owner = function ? function : owner;
                Node* nested = typeDeclaration(declaration) || declaration.kind == "Variant" ? &declaration : owner;
                auto* enclosingFunction = declaration.kind == "Function" ? &declaration : function;
                for (auto& item : declaration.children) index(*item, module, nested, enclosingFunction);
            }
            NodePtr clone(const Node& original)
            {
                auto result = node(original.kind, original.value, original.range);
                result->comments = original.comments; result->height = original.height;
                if (const auto self = selfOwners_.find(&original); self != selfOwners_.end()) selfOwners_[result.get()] = self->second;
                const auto origin = origins_.find(&original);
                if (origin != origins_.end() && originalNodes_.contains(origin->second)) origins_[result.get()] = origin->second;
                else if (!synthesized_.contains(&original) && originalNodes_.contains(&original)) origins_[result.get()] = &original;
                for (const auto& item : original.children) result->children.push_back(clone(*item));
                return result;
            }
            const Node* originalSyntax(const Node& expression) const
            {
                if (rewritten_.contains(&expression)) return nullptr;
                const auto cloned = origins_.find(&expression);
                if (cloned == origins_.end() && synthesized_.contains(&expression)) return nullptr;
                const auto* origin = cloned == origins_.end() ? &expression : cloned->second;
                if (!originalNodes_.contains(origin)) return nullptr;
                const auto spelling = referencePaths_.find(origin);
                if (spelling == referencePaths_.end() || spelling->second != flatPath(expression)) return nullptr;
                return origin;
            }
            const std::vector<SymbolId>* originalReferences(const Node& expression) const
            {
                const auto* origin = originalSyntax(expression);
                if (!origin) return nullptr;
                const auto found = names_.references.find(origin);
                return found == names_.references.end() ? nullptr : &found->second;
            }
            NameResolution constantNamesFor(const Node& expression) const
            {
                auto result = names_;
                const auto bind = [&](const auto& self, const Node& value) -> void
                {
                    result.references.erase(&value);
                    if (const auto* ids = originalReferences(value)) result.references[&value] = *ids;
                    for (const auto& nested : value.children) self(self, *nested);
                };
                bind(bind, expression); return result;
            }
            bool builtinReceiver(const Node& expression, std::string_view expected, ModuleId module, const std::map<std::string, const Node*>& locals)
            {
                if (expression.kind != "Name" || expression.value != expected || locals.contains(expression.value)) return false;
                if (const auto* ids = originalReferences(expression))
                {
                    if (ids->size() != 1 || ids->front() >= names_.symbols.size()) return false;
                    const auto& symbol = names_.symbols[ids->front()];
                    return !symbol.declaration && symbol.module == InvalidModuleId && symbol.name == expected
                        && (symbol.kind == SymbolKind::Type || symbol.kind == SymbolKind::Intrinsic);
                }
                // 새 노드의 주소는 선행 AST의 심벌 키가 아니다. 현재 선언과 내장 이름으로 재해석한다.
                // A generated address is not an original AST symbol key; resolve current declarations and builtin names.
                if (!references(expression, module).empty()) return false;
                return std::any_of(names_.symbols.begin(), names_.symbols.end(), [&](const auto& symbol)
                { return !symbol.declaration && symbol.module == InvalidModuleId && symbol.name == expected
                    && (symbol.kind == SymbolKind::Type || symbol.kind == SymbolKind::Intrinsic); });
            }
            bool builtinMember(const Node& callee, std::string_view expected, ModuleId module, const std::map<std::string, const Node*>& locals)
            {
                // 가져온 모듈 함수와 같은 철자의 내장 네임스페이스를 혼동하지 않는다.
                // Do not confuse an imported module function with an identically spelled builtin namespace.
                if (callee.kind != "Member" || callee.children.size() != 1
                    || !builtinReceiver(*callee.children[0], expected, module, locals)) return false;
                if (originalSyntax(callee) && originalReferences(*callee.children[0]))
                {
                    // 선행 이름 해석의 내장 선택은 간접 의존 모듈 때문에 바뀌지 않는다.
                    // A canonical builtin binding cannot change because a transitive module is loaded.
                    return originalReferences(callee) == nullptr;
                }
                bool visible = graph_.modules[module]->name == expected;
                for (const auto& imported : graph_.modules[module]->ast->children)
                {
                    if (imported->kind != "Import" || imported->value != expected) continue;
                    bool selected = false;
                    for (const auto& name : imported->children)
                        if (name->kind == "ImportName")
                        {
                            selected = true;
                            if (name->value == "*" || name->value == callee.value) visible = true;
                        }
                    if (!selected) visible = true;
                }
                return !visible || references(callee, module).empty();
            }
            std::vector<Declaration> references(const Node& expression, ModuleId module)
            {
                std::vector<Declaration> result;
                if ((expression.kind == "Member" || expression.kind == "QualifiedType") && !expression.children.empty())
                {
                    const auto parent = declaration(*expression.children[0], module);
                    if (parent.node && canonicalKeys_.contains(parent.node))
                    {
                        // Self의 철자가 같아도 구체 소유자의 멤버를 원본 템플릿으로 되돌리지 않는다.
                        // An unchanged Self spelling must still resolve members on its materialized owner.
                        for (const auto& member : parent.node->children)
                            if (member->value == expression.value && (member->kind == "Variant" || member->kind == "Function" || member->kind == "Variable"))
                            {
                                const auto known = declarations_.find(member.get());
                                result.push_back(known == declarations_.end() ? Declaration{member.get(), parent.module, InvalidSymbolId, parent.node} : known->second);
                            }
                        return result;
                    }
                }
                if (const auto* ids = originalReferences(expression))
                    for (const auto id : *ids)
                    {
                        const auto symbol = symbols_.find(id);
                        if (symbol != symbols_.end() && declarations_.contains(symbol->second)) result.push_back(declarations_.at(symbol->second));
                    }
                if (!result.empty()) return result;
                const auto path = flatPath(expression);
                for (const auto& [address, declaration] : declarations_)
                {
                    (void)address;
                    if (declaration.owner) continue;
                    const auto full = graph_.modules[declaration.module]->name + "." + declaration.node->value;
                    if ((declaration.module == module && path == declaration.node->value) || path == full) result.push_back(declaration);
                }
                if (result.empty() && (expression.kind == "Member" || expression.kind == "QualifiedType") && !expression.children.empty())
                {
                    const auto parent = declaration(*expression.children[0], module);
                    if (parent.node)
                        for (const auto& member : parent.node->children)
                            if (member->value == expression.value && (member->kind == "Variant" || member->kind == "Function" || member->kind == "Variable"))
                            {
                                const auto known = declarations_.find(member.get());
                                result.push_back(known == declarations_.end() ? Declaration{member.get(), parent.module, InvalidSymbolId, parent.node} : known->second);
                            }
                }
                return result;
            }
            Declaration declaration(const Node& expression, ModuleId module)
            {
                if (expression.value == "Self")
                    if (const auto owner = selfOwners_.find(&expression); owner != selfOwners_.end())
                        if (const auto declared = declarations_.find(owner->second); declared != declarations_.end()) return declared->second;
                const auto found = references(expression, module);
                return found.size() == 1 ? found[0] : Declaration{};
            }
            NodePtr qualified(std::string_view path, std::string kind, SourceRange range)
            {
                std::size_t begin = 0;
                const auto firstDot = path.find('.');
                auto result = node(kind == "Type" ? "Type" : "Name", std::string(path.substr(0, firstDot)), range);
                if (firstDot == std::string_view::npos) return result;
                begin = firstDot + 1;
                while (begin < path.size())
                {
                    const auto end = path.find('.', begin);
                    auto next = node(kind == "Type" ? "QualifiedType" : "Member", std::string(path.substr(begin, end - begin)), range);
                    next->children.push_back(std::move(result)); result = std::move(next);
                    if (end == std::string_view::npos) break;
                    begin = end + 1;
                }
                return result;
            }
            void compilerImport(ModuleId into, const Declaration& from, SourceRange range)
            {
                if (into == from.module) return;
                auto& root = *graph_.modules[into]->ast;
                for (const auto& item : root.children)
                    if (item->kind == "Import" && item->value == graph_.modules[from.module]->name && child(*item, "CompilerImport"))
                        for (const auto& selected : item->children)
                            if (selected->kind == "ImportName" && selected->value == from.node->value) return;
                auto import = node("Import", graph_.modules[from.module]->name, range);
                import->children.push_back(node("ImportName", from.node->value, range));
                import->children.push_back(node("CompilerImport", "", range));
                root.children.push_back(std::move(import));
            }
            NodePtr use(const Declaration& target, ModuleId into, std::string kind, SourceRange range)
            {
                compilerImport(into, target, range);
                auto result = qualified(into == target.module ? target.node->value : graph_.modules[target.module]->name + "." + target.node->value, kind, range);
                return result;
            }
            NodePtr portableType(const Node& original, ModuleId from, ModuleId into)
            {
                const auto alias = declaration(original, from);
                if (original.kind == "Type" && original.value == "Self" && alias.node)
                {
                    // 제네릭 계약의 추상 Self를 타입 인자가 없는 소유 템플릿으로 바꾸지 않는다.
                    // Keep abstract Self in a generic contract instead of producing a bare owner template.
                    return generic(*alias.node) ? clone(original) : use(alias, into, "Type", original.range);
                }
                if (alias.node && alias.node->kind == "TypeAlias" && !alias.node->children.empty())
                {
                    auto result = portableType(*alias.node->children[0], alias.module, into);
                    result->range = original.range;
                    return result;
                }
                auto result = clone(original);
                if (original.kind == "ReferenceType" || original.kind == "DynamicType")
                {
                    result->children[0] = portableType(*original.children[0], from, into);
                    return result;
                }
                const auto declared = declaration(original, from);
                if (declared.node && typeDeclaration(*declared.node))
                {
                    auto arguments = std::move(result->children);
                    result = use(declared, into, "Type", original.range);
                    if (original.kind == "Type")
                        for (auto& argument : arguments) result->children.push_back(portableType(*argument, from, into));
                    else if (original.kind == "QualifiedType")
                        for (std::size_t index = 1; index < arguments.size(); ++index) result->children.push_back(portableType(*arguments[index], from, into));
                }
                else if (original.kind == "Type")
                {
                    // 내장 컨테이너에도 원래 모듈의 명목 타입이 중첩될 수 있다.
                    // Builtin containers may nest nominal types that still belong to the original module.
                    for (std::size_t index = 0; index < original.children.size(); ++index)
                    {
                        if (original.value == "Array" && index == 1)
                        {
                            if (const auto value = constantGenericValue(*original.children[index]))
                            {
                                result->children[index] = node("ConstantTypeArgument", std::to_string(*value), original.children[index]->range);
                                continue;
                            }
                        }
                        result->children[index] = portableType(*original.children[index], from, into);
                    }
                }
                return result;
            }
            void substitute(Node& item, const Bindings& bindings, ModuleId argumentModule, ModuleId into)
            {
                if (item.kind == "Type" || item.kind == "Name")
                {
                    const auto found = bindings.find(item.value);
                    if (found != bindings.end() && item.children.empty()
                        && (item.kind == "Type" || found->second->kind == "ConstantTypeArgument"))
                    {
                        auto replacement = portableType(*found->second, argumentModule, into);
                        if (item.kind == "Name" && replacement->kind == "ConstantTypeArgument") replacement->kind = "Literal";
                        const auto range = item.range;
                        const auto origin = origins_.find(replacement.get());
                        const auto* replacementOrigin = origin == origins_.end() ? nullptr : origin->second;
                        const auto self = selfOwners_.find(replacement.get());
                        auto* replacementOwner = self == selfOwners_.end() ? nullptr : self->second;
                        item = std::move(*replacement); item.range = range;
                        if (replacementOrigin) origins_[&item] = replacementOrigin; else origins_.erase(&item);
                        if (replacementOwner) selfOwners_[&item] = replacementOwner; else selfOwners_.erase(&item);
                        rewritten_.erase(&item);
                        synthesized_.insert(&item);
                        return;
                    }
                }
                for (auto& nested : item.children) substitute(*nested, bindings, argumentModule, into);
            }
            std::vector<const Node*> typeArguments(const Node& expression)
            {
                std::vector<const Node*> result;
                if (expression.kind == "GenericName")
                {
                    if (const auto* args = child(expression, "TypeArguments"))
                        for (const auto& argument : args->children) result.push_back(argument.get());
                }
                else if (expression.kind != "Member")
                    for (std::size_t index = expression.kind == "QualifiedType" ? 1 : 0; index < expression.children.size(); ++index)
                        result.push_back(expression.children[index].get());
                return result;
            }
            std::string key(const Node& type, ModuleId module)
            {
                if (type.kind == "ConstantTypeArgument" || type.kind == "Literal")
                    if (const auto value = constantGenericValue(type)) return std::to_string(*value);
                if (type.kind == "ReferenceType") return "&" + type.value + key(*type.children[0], module);
                auto declared = declaration(type, module);
                if (declared.node && declared.node->kind == "Variable" && modifier(*declared.node, "const"))
                    if (const auto value = constantGenericValue(type)) return std::to_string(*value);
                if (declared.node && declared.node->kind == "TypeAlias" && !declared.node->children.empty())
                    return key(*declared.node->children[0], declared.module);
                if (declared.node && canonicalKeys_.contains(declared.node)) return canonicalKeys_.at(declared.node);
                std::string result = declared.node ? graph_.modules[declared.module]->name + "." + declared.node->value : flatPath(type);
                if (result == "int") result = "i32";
                if (result == "float") result = "f64";
                auto arguments = typeArguments(type);
                Bindings completed;
                if (declared.node && typeDeclaration(*declared.node))
                    if (const auto* parameters = child(*declared.node, "GenericParameters"); parameters
                        && arguments.size() < parameters->children.size())
                    {
                        for (std::size_t index = 0; index < arguments.size(); ++index)
                            completed[parameters->children[index]->value] = clone(*arguments[index]);
                        if (completeBindings(declared, completed, module, false))
                        {
                            // 생략한 기본 타입 인자도 중첩된 특수화의 동일성에 포함한다.
                            // Include omitted default type arguments in nested specialization identity.
                            arguments.clear();
                            for (const auto& parameter : parameters->children) arguments.push_back(completed.at(parameter->value).get());
                        }
                    }
                if (!arguments.empty())
                {
                    result += '<';
                    for (const auto* argument : arguments) result += key(*argument, module) + ',';
                    result += '>';
                }
                return result;
            }
            bool copy(const Node& type, ModuleId module, std::set<std::string>& visiting)
            {
                const auto alias = declaration(type, module);
                if (alias.node && alias.node->kind == "TypeAlias" && !alias.node->children.empty())
                    return copy(*alias.node->children[0], alias.module, visiting);
                if (type.kind == "ReferenceType") return type.value != "mut";
                const auto name = flatPath(type);
                if (name == "string") return false;
                if (name == "Range" && type.children.size() == 1) return copy(*type.children[0], module, visiting);
                if (name == "Slice" && type.children.size() == 1) return true;
                // 집합의 명시적 복사와 Copy 계약은 다르며 배열과 튜플은 이동 전용이다.
                // Explicit aggregate copying is distinct from Copy; arrays and tuples remain move-only.
                if (name == "Array" || name == "Tuple") return false;
                static const std::set<std::string> scalars = {"i8","i16","i32","i64","i128","u8","u16","u32","u64","u128","int","isize","usize","f16","bf16","f32","f64","float","bool","char","byte","unit","never","string_view"};
                if (scalars.contains(name)) return true;
                const auto declared = declaration(type, module);
                if (!declared.node) return false;
                if (child(*declared.node, "CompilerShared") || child(*declared.node, "CompilerWeak") || child(*declared.node, "CompilerAtomic") || child(*declared.node, "CompilerMutex") || child(*declared.node, "CompilerRwLock") || child(*declared.node, "CompilerMutexGuard") || child(*declared.node, "CompilerReadGuard") || child(*declared.node, "CompilerWriteGuard") || child(*declared.node, "CompilerCancellationSource") || child(*declared.node, "CompilerCancellationToken") || child(*declared.node, "CompilerTask") || child(*declared.node, "CompilerTaskScope")) return false;
                if (child(*declared.node, "CompilerSimd")) return true;
                const auto identity = key(type, module);
                if (!visiting.insert(identity).second) return false;
                const auto* implemented = child(*declared.node, "Implements");
                bool answer = false;
                if (implemented)
                    for (const auto& interface : implemented->children) if (interface->value == "Copy") answer = true;
                if (implementsDrop(*declared.node)) answer = false;
                if (answer)
                {
                    Bindings parameters;
                    const auto* generics = child(*declared.node, "GenericParameters");
                    const auto args = typeArguments(type);
                    if (generics && args.size() == generics->children.size())
                        for (std::size_t index = 0; index < args.size(); ++index) parameters[generics->children[index]->value] = clone(*args[index]);
                    const auto fields = [&](const Node& owner)
                    {
                        for (const auto& field : owner.children)
                            if (field->kind == "Variable" && !field->children.empty())
                            {
                                auto fieldType = clone(*field->children[0]);
                                substitute(*fieldType, parameters, module, declared.module);
                                if (!copy(*fieldType, declared.module, visiting)) return false;
                            }
                        return true;
                    };
                    if (declared.node->kind == "Enum")
                    {
                        for (const auto& variant : declared.node->children)
                            if (variant->kind == "Variant" && !fields(*variant)) answer = false;
                    }
                    else answer = fields(*declared.node);
                }
                visiting.erase(identity);
                return answer;
            }
            bool callableConstraint(const Node& concrete, ModuleId module, const Node& required)
            {
                const auto candidate = declaration(concrete, module);
                const auto* marker = candidate.node ? child(*candidate.node, "CompilerClosure") : nullptr;
                const auto wanted = typeArguments(required);
                if (!marker || wanted.size() != 2 || wanted[0]->value != "Tuple") return false;
                const auto rank = [](std::string_view name) { return name == "Fn" ? 0 : name == "FnMut" ? 1 : 2; };
                if (rank(marker->value) > rank(required.value)) return false;
                const auto* method = child(*candidate.node, "Function");
                if (!method || method->value != "__invoke" || method->children.empty()) return false;
                const auto* parameters = child(*method, "Parameters");
                if (!parameters || parameters->children.size() != wanted[0]->children.size() + 1) return false;
                if (key(*method->children[0], candidate.module) != key(*wanted[1], module)) return false;
                for (std::size_t index = 0; index < wanted[0]->children.size(); ++index)
                    if (key(*parameters->children[index + 1]->children[0], candidate.module) != key(*wanted[0]->children[index], module)) return false;
                return true;
            }
            bool floatingContract(const Node& type, ModuleId module)
            {
                const auto target = declaration(type, module);
                if (!target.node) return false;
                if (target.node->kind == "TypeAlias" && !target.node->children.empty()) return floatingContract(*target.node->children[0], target.module);
                const auto* marker = child(*target.node, "CompilerNumericContract");
                return marker && marker->value == "float";
            }
            bool floatingScalar(const Node& type, ModuleId module)
            {
                const auto target = declaration(type, module);
                if (target.node && target.node->kind == "TypeAlias" && !target.node->children.empty()) return floatingScalar(*target.node->children[0], target.module);
                const auto name = key(type, module);
                return name == "f16" || name == "bf16" || name == "f32" || name == "f64";
            }
            bool constraint(const Node& concrete, ModuleId module, const Node& required)
            {
                if (floatingContract(required, module)) return floatingScalar(concrete, module);
                if (required.value == "Fn" || required.value == "FnMut" || required.value == "FnOnce") return callableConstraint(concrete, module, required);
                if (required.value == "Copy") { std::set<std::string> visiting; return copy(concrete, module, visiting); }
                const auto type = declaration(concrete, module);
                if (required.value == "Drop")
                {
                    if (!required.children.empty()) return false;
                    if (type.node && type.node->kind == "TypeAlias" && !type.node->children.empty())
                        return constraint(*type.node->children[0], type.module, required);
                    return type.node && (type.node->kind == "Struct" || type.node->kind == "Enum") && implementsDrop(*type.node);
                }
                const auto* interfaces = type.node ? child(*type.node, "Implements") : nullptr;
                if (!interfaces) return false;
                for (const auto& interface : interfaces->children)
                    if (key(*interface, type.module) == key(required, module)) return true;
                return false;
            }
            std::optional<std::uint64_t> constantGenericValue(const Node& argument)
            {
                const auto constantNames = constantNamesFor(argument);
                const Node* expression = &argument;
                if (expression->kind == "ConstantTypeArgument" && expression->children.size() == 1) expression = expression->children[0].get();
                TypeContext constants;
                const std::unordered_map<const Node*, TypeId> nodeTypes;
                const std::unordered_map<SymbolId, TypeId> symbolTypes;
                const auto evaluated = ConstEvaluator(constantNames, constants, nodeTypes, symbolTypes).evaluate(*expression, constants.builtin("usize"));
                if (evaluated.status != ConstStatus::Constant || evaluated.value.kind != ConstantKind::Integer
                    || evaluated.value.negative || evaluated.value.highMagnitude != 0) return std::nullopt;
                return evaluated.value.magnitude;
            }

            NodePtr constantGenericArgument(const Node& argument, const Node& parameter, ModuleId declarationModule, ModuleId module, bool diagnose)
            {
                const auto declared = !parameter.children.empty() ? portableType(*parameter.children[0], declarationModule, module) : NodePtr{};
                if (!declared || key(*declared, module) != "usize")
                { if (diagnose) error(parameter, "Const generic parameters currently require the usize type."); return {}; }
                const auto value = constantGenericValue(argument);
                if (!value)
                { if (diagnose) error(argument, "Const generic arguments must be nonnegative compile-time usize values."); return {}; }
                return node("ConstantTypeArgument", std::to_string(*value), argument.range);
            }

            bool completeBindings(const Declaration& original, Bindings& bindings, ModuleId module, bool diagnose)
            {
                const auto* parameters = child(*original.node, "GenericParameters");
                if (!parameters) return bindings.empty();
                for (const auto& parameter : parameters->children)
                {
                    if (!bindings.contains(parameter->value))
                    {
                        const auto* initial = child(*parameter, "GenericDefault");
                        if (!initial || initial->children.empty()) return false;
                        auto value = portableType(*initial->children[0], original.module, module);
                        substitute(*value, bindings, module, module);
                        bindings[parameter->value] = std::move(value);
                    }
                    auto& argument = bindings.at(parameter->value);
                    if (parameter->kind == "ConstParameter")
                    {
                        auto normalized = constantGenericArgument(*argument, *parameter, original.module, module, diagnose);
                        if (!normalized) return false;
                        argument = std::move(normalized);
                    }
                    else if (argument->kind == "ConstantTypeArgument" || argument->kind == "Literal")
                    { if (diagnose) error(*argument, "A type generic argument cannot be a constant value."); return false; }
                }
                return true;
            }

            Declaration specialize(const Declaration& original, const std::vector<const Node*>& arguments, ModuleId argumentModule, const Node& site, std::size_t depth)
            {
                if (!original.node) return {};
                const auto* parameters = child(*original.node, "GenericParameters");
                if (!parameters || parameters->children.size() < arguments.size())
                {
                    error(site, "Generic type argument count does not match its declaration."); return {};
                }
                Bindings bindings;
                for (std::size_t index = 0; index < arguments.size(); ++index) bindings[parameters->children[index]->value] = clone(*arguments[index]);
                if (!completeBindings(original, bindings, argumentModule, true))
                { if (diagnostics_.size() == initial_) error(site, "Generic arguments are missing and have no default."); return {}; }
                std::vector<const Node*> completed;
                for (const auto& parameter : parameters->children) completed.push_back(bindings.at(parameter->value).get());
                // 구체 소유 타입과 원본 위치를 포함해 복제된 메서드끼리 충돌하지 않게 한다.
                // Include the concrete owner and source location to distinguish cloned methods.
                std::string identity = graph_.modules[original.module]->name + ':'
                    + (original.owner ? key(*use(declarations_.at(original.owner), original.module, "Type", original.node->range), original.module) : std::string{})
                    + ':' + std::to_string(original.node->range.begin.source) + ':' + std::to_string(original.node->range.begin.offset)
                    + ':' + original.node->value + ':';
                for (const auto* argument : completed) identity += key(*argument, argumentModule) + ';';
                const auto existing = specializations_.find(identity);
                if (existing != specializations_.end()) return existing->second;
                if (generated_ >= maximumGeneratedDeclarations || depth > 64)
                { error(site, "Generic specialization exceeds the 4096 generated-declaration limit or nesting depth 64."); return {}; }
                for (const auto& requirement : original.node->children)
                    if (requirement->kind == "Constraint" && requirement->children.size() >= 2)
                    {
                        auto concrete = portableType(*requirement->children[0], original.module, argumentModule);
                        substitute(*concrete, bindings, argumentModule, argumentModule);
                        for (std::size_t index = 1; index < requirement->children.size(); ++index)
                        {
                            auto required = portableType(*requirement->children[index], original.module, argumentModule);
                            substitute(*required, bindings, argumentModule, argumentModule);
                            if (!constraint(*concrete, argumentModule, *required))
                            { error(site, "Concrete type '" + key(*concrete, argumentModule) + "' does not satisfy implements " + flatPath(*requirement->children[index]) + "."); return {}; }
                        }
                    }
                auto result = clone(*original.node);
                result->value += "$" + std::to_string(++generated_);
                result->children.erase(std::remove_if(result->children.begin(), result->children.end(), [](const auto& item)
                { return item->kind == "GenericParameters" || item->kind == "Constraint" || item->kind == "CompilerTemplate"; }), result->children.end());
                substitute(*result, bindings, argumentModule, original.module);
                if (original.owner)
                {
                    Bindings self; self["Self"] = use(declarations_.at(original.owner), original.module, "Type", original.node->range);
                    substitute(*result, self, original.module, original.module);
                }
                if (original.owner) mark(*result, "CompilerMethodSpecialization");
                mark(*result, "CompilerGenerated");
                Declaration concrete{result.get(), original.module, InvalidSymbolId, original.owner};
                declarations_[result.get()] = concrete;
                specializations_[identity] = concrete;
                if (typeDeclaration(*original.node))
                {
                    std::string canonical = graph_.modules[original.module]->name + "." + original.node->value + '<';
                    for (const auto* argument : completed) canonical += key(*argument, argumentModule) + ',';
                    canonicalKeys_[concrete.node] = canonical + '>';
                    auto logical = use(original, argumentModule, "Type", site.range);
                    for (const auto* argument : completed) logical->children.push_back(clone(*argument));
                    logicalTypes_[concrete.node] = {std::move(logical), argumentModule};
                }
                if (original.owner) original.owner->children.push_back(std::move(result));
                else graph_.modules[original.module]->ast->children.push_back(std::move(result));
                index(*concrete.node, original.module, original.owner);
                if (concrete.node->kind == "Struct" || concrete.node->kind == "Enum")
                {
                    // 구체 소유자의 모든 메서드 서명과 본문에는 동일한 구체 Self를 사용한다.
                    // Give every materialized owner's signatures and bodies the same concrete Self identity.
                    Bindings self; self["Self"] = use(concrete, original.module, "Type", site.range);
                    substitute(*concrete.node, self, original.module, original.module);
                }
                if (typeDeclaration(*concrete.node))
                    for (auto& method : concrete.node->children)
                        if (method->kind == "Function" && !generic(*method) && child(*method, "Constraint"))
                        {
                            bool satisfied = true;
                            for (const auto& requirement : method->children)
                                if (requirement->kind == "Constraint" && requirement->children.size() >= 2)
                                    for (std::size_t index = 1; index < requirement->children.size(); ++index)
                                        satisfied = constraint(*requirement->children[0], original.module, *requirement->children[index]) && satisfied;
                            // 구체 소유 타입의 조건부 메서드는 검증된 경우만 실행 가능한 선언이 된다.
                            // A conditional method on a concrete owner becomes executable only when its bounds hold.
                            if (satisfied)
                                std::erase_if(method->children, [](const auto& value) { return value->kind == "Constraint"; });
                            else { mark(*method, "CompilerUnavailable"); mark(*method, "CompilerTemplate"); }
                        }
                process(*concrete.node, original.module, {}, nullptr, depth + 1);
                return concrete;
            }
            using Locals = std::map<std::string, const Node*>;
            std::vector<NodePtr> inferred_;
            const Node* localParameterType(const Node& parameter)
            {
                if (parameter.children.empty()) return nullptr;
                if (parameter.kind == "VariadicParameter")
                {
                    auto view = node("Type", "Slice", parameter.range);
                    view->children.push_back(clone(*parameter.children[0]));
                    const auto* result = view.get(); inferred_.push_back(std::move(view)); return result;
                }
                const auto* suffixes = child(parameter, "ArraySuffixes");
                if (!suffixes || suffixes->children.empty()) return parameter.children[0].get();
                auto value = clone(*parameter.children[0]);
                for (auto index = suffixes->children.size(); index > 0; --index)
                {
                    const auto& suffix = *suffixes->children[index - 1];
                    if (suffix.children.empty()) return parameter.children[0].get();
                    auto array = node("Type", "Array", parameter.range);
                    auto size = node("ConstantTypeArgument", "", suffix.range);
                    size->children.push_back(clone(*suffix.children[0]));
                    array->children.push_back(std::move(value));
                    array->children.push_back(std::move(size));
                    value = std::move(array);
                }
                const auto* result = value.get(); inferred_.push_back(std::move(value)); return result;
            }
            Declaration receiverDeclaration(NodePtr type, ModuleId module, Node* owner, bool* dynamic = nullptr)
            {
                for (std::size_t depth = 0; type && depth < 32; ++depth)
                {
                    if (type->kind == "ReferenceType" || type->kind == "DynamicType")
                    {
                        if (dynamic && type->kind == "DynamicType") *dynamic = true;
                        type = type->children.empty() ? NodePtr{} : std::move(type->children[0]); continue;
                    }
                    if (type->value == "Self" && owner) return declarations_.at(owner);
                    const auto target = declaration(*type, module);
                    if (!target.node || !child(*target.node, "CompilerBox")) return target;
                    NodePtr payload;
                    for (const auto& field : target.node->children)
                        if (field->kind == "Variable" && !field->children.empty())
                        { payload = portableType(*field->children[0], target.module, module); break; }
                    type = std::move(payload);
                }
                return {};
            }
            NodePtr infer(const Node& expression, ModuleId module, const Locals& locals, Node* owner, std::size_t depth = 0)
            {
                if (depth > 64) return {};
                if ((expression.kind == "CompilerSharedExpression" || expression.kind == "CompilerAtomicExpression" || expression.kind == "CompilerSyncExpression" || expression.kind == "CompilerTaskExpression") && !expression.children.empty()) return clone(*expression.children[0]);
                if (expression.kind == "CompilerMemoryExpression" && !expression.children.empty())
                    return expression.value == "swap" ? node("Type", "unit", expression.range) : clone(*expression.children[0]);
                if ((expression.kind == "CompilerTextExpression" || expression.kind == "CompilerSliceExpression") && !expression.children.empty()) return clone(*expression.children[0]);
                for (const auto* kind : {"CompilerRangeResult", "CompilerCollectionResult", "CompilerArrayResult"})
                    if (const auto* marker = child(expression, kind); marker && !marker->children.empty()) return clone(*marker->children[0]);
                if ((expression.kind == "CompilerBoxExpression" || expression.kind == "CompilerNumericExpression" || expression.kind == "CompilerFloatBitsExpression" || expression.kind == "CompilerCharScalarExpression" || expression.kind == "CompilerRangeInspectExpression") && !expression.children.empty()) return clone(*expression.children[0]);
                if (expression.kind == "Postfix" && expression.value == "?")
                {
                    auto carrier = infer(*expression.children[0], module, locals, owner, depth + 1);
                    if (carrier)
                    {
                        auto arguments = carrierArguments(*carrier, module, "Result");
                        if (!arguments.empty()) return std::move(arguments[0]);
                    }
                    return {};
                }
                if (expression.kind == "Group") return infer(*expression.children[0], module, locals, owner, depth + 1);
                if (expression.kind == "Literal")
                {
                    const auto& text = expression.value;
                    if (!numericSuffix(text).empty()) return node("Type", std::string(numericSuffix(text)), expression.range);
                    return node("Type", text == "true" || text == "false" ? "bool" : isStringLiteralSpelling(text) ? "string_view" : text.starts_with('\'') ? "char"
                        : !text.starts_with("0x") && !text.starts_with("0X") && text.find_first_of(".eE") != std::string::npos ? "f64" : "i32", expression.range);
                }
                if (expression.kind == "Name")
                {
                    const auto local = locals.find(expression.value);
                    if (local != locals.end()) return clone(*local->second);
                    const auto* referenced = originalReferences(expression);
                    if (referenced && referenced->size() == 1)
                    {
                        const auto id = referenced->front();
                        if (id < names_.symbols.size())
                        {
                            const auto* declaration = names_.symbols[id].declaration;
                            if (declaration && (declaration->kind == "Variable" || declaration->kind == "Parameter" || declaration->kind == "PatternBinding") && !declaration->children.empty())
                            {
                                if (declaration->children[0]->kind != "InferredType") return portableType(*localParameterType(*declaration), names_.symbols[id].module, module);
                                for (std::size_t index = 1; index < declaration->children.size(); ++index)
                                    if (declaration->children[index]->kind != "ArraySuffixes" && declaration->children[index]->kind != "Modifier")
                                        return infer(*declaration->children[index], names_.symbols[id].module, locals, owner, depth + 1);
                            }
                        }
                    }
                    return {};
                }
                if (expression.kind == "Prefix")
                {
                    auto value = infer(*expression.children[0], module, locals, owner, depth + 1);
                    if (!value) return {};
                    if (expression.value == "&" || expression.value == "&mut")
                    {
                        auto result = node("ReferenceType", expression.value == "&mut" ? "mut" : "read", expression.range);
                        result->children.push_back(std::move(value)); return result;
                    }
                    if (expression.value == "*" && value->kind == "ReferenceType") return std::move(value->children[0]);
                    if (expression.value == "!") return node("Type", "bool", expression.range);
                    return value;
                }
                if (expression.kind == "Range" || (expression.kind == "Binary" && (expression.value == ".." || expression.value == "..=")))
                {
                    auto element = infer(*expression.children[0], module, locals, owner, depth + 1);
                    if (numericLiteral(*expression.children[0]) && !numericLiteral(*expression.children[1])) element = infer(*expression.children[1], module, locals, owner, depth + 1);
                    if (!element) return {};
                    auto range = node("Type", "Range", expression.range); range->children.push_back(std::move(element)); return range;
                }
                if (expression.kind == "Binary")
                {
                    static const std::set<std::string> comparisons = {"==","!=","<","<=",">",">=","&&","||"};
                    if (comparisons.contains(expression.value)) return node("Type", "bool", expression.range);
                    return infer(*expression.children[0], module, locals, owner, depth + 1);
                }
                if (expression.kind == "Conditional") return infer(*expression.children[1], module, locals, owner, depth + 1);
                if (expression.kind == "NamedInitializer")
                {
                    const auto target = declaration(*expression.children[0], module);
                    if (target.node && target.node->kind == "TypeAlias" && !target.node->children.empty())
                        return portableType(*target.node->children[0], target.module, module);
                    if (target.node && typeDeclaration(*target.node)) return use(target, module, "Type", expression.range);
                    if (target.node && target.node->kind == "Variant" && target.owner) return use(declarations_.at(target.owner), module, "Type", expression.range);
                    return {};
                }
                if (expression.kind == "Call")
                {
                    const Node* tupleCallee = expression.children.empty() ? nullptr : expression.children[0].get();
                    if (tupleCallee && tupleCallee->kind == "GenericName" && !tupleCallee->children.empty()) tupleCallee = tupleCallee->children[0].get();
                    if (tupleCallee && expression.children[0]->kind != "GenericName")
                    {
                        const auto& method = tupleCallee->value;
                        const bool oneArgument = expression.children.size() == 2 && expression.children[1]->children.size() == 1;
                        if (oneArgument && method == "into_inner" && tupleCallee->kind == "Member" && !tupleCallee->children.empty())
                        {
                            // 추상 계약에서도 실제 봉인 Box 정체성과 인수 T만 사용한다.
                            // Abstract contracts use only the actual sealed Box identity and its argument T.
                            const auto box = declaration(*tupleCallee->children[0], module);
                            if (box.node && child(*box.node, "CompilerBox"))
                            {
                                const auto input = infer(*expression.children[1]->children[0], module, locals, owner, depth + 1);
                                const auto inputBox = input ? declaration(*input, module) : Declaration{};
                                if (input && input->kind != "ReferenceType" && inputBox.node && child(*inputBox.node, "CompilerBox"))
                                {
                                    const auto arguments = typeArguments(*input);
                                    if (arguments.size() == 1) return clone(*arguments[0]);
                                    for (const auto& field : inputBox.node->children)
                                        if (field->kind == "Variable" && !field->children.empty()) return portableType(*field->children[0], inputBox.module, module);
                                }
                            }
                        }
                        // 고정 반환형의 내장 값 연산도 중첩 호출의 타입 추론에 참여한다.
                        // Fixed-result builtin value operations participate in nested-call inference.
                        // 전체 멤버의 정체성을 확인하므로 같은 이름의 사용자 모듈 함수에는 적용하지 않는다.
                        // Full member identity prevents these shapes from intercepting same-named user module functions.
                        if (oneArgument && ((method == "len" && builtinMember(*tupleCallee, "Slice", module, locals))
                            || ((method == "len" || method == "capacity") && builtinMember(*tupleCallee, "Vector", module, locals))
                            || (method == "len" && builtinMember(*tupleCallee, "Tuple", module, locals))))
                            return node("Type", "usize", expression.range);
                        if (oneArgument && method == "is_empty" && builtinMember(*tupleCallee, "Tuple", module, locals))
                            return node("Type", "bool", expression.range);
                        if (builtinMember(*tupleCallee, "string", module, locals))
                        {
                            if (oneArgument && (method == "byte_len" || method == "scalar_count")) return node("Type", "usize", expression.range);
                            if ((oneArgument && method == "is_empty") || (expression.children.size() == 3 && method == "equals"))
                                return node("Type", "bool", expression.range);
                            if (oneArgument && method == "view") return node("Type", "string_view", expression.range);
                            if (oneArgument && method == "clear") return node("Type", "unit", expression.range);
                            if (oneArgument && method == "as_bytes")
                            {
                                auto result = node("Type", "Slice", expression.range);
                                result->children.push_back(node("Type", "byte", expression.range)); return result;
                            }
                        }
                    }
                    if (tupleCallee && ((tupleCallee->value == "from_copy" && builtinMember(*tupleCallee, "Array", module, locals))
                        || (tupleCallee->value == "copy" && builtinMember(*tupleCallee, "Tuple", module, locals)))
                        && expression.children.size() == 2 && expression.children[1]->value.empty()
                        && expression.children[1]->children.size() == 1 && expression.children[0]->kind != "GenericName")
                    {
                        auto reference = infer(*expression.children[1]->children[0], module, locals, owner, depth + 1);
                        if (reference && reference->kind == "ReferenceType" && reference->children.size() == 1)
                        {
                            const auto& target = *reference->children[0];
                            if ((tupleCallee->value == "from_copy" && target.value == "Array" && target.children.size() == 2)
                                || (tupleCallee->value == "copy" && target.value == "Tuple")) return clone(target);
                        }
                        return {};
                    }
                    if (tupleCallee && builtinMember(*tupleCallee, "Tuple", module, locals)
                        && (tupleCallee->value == "get" || tupleCallee->value == "get_mut") && expression.children.size() == 2
                        && expression.children[1]->children.size() == 1)
                    {
                        const auto arguments = typeArguments(*expression.children[0]);
                        const auto index = arguments.size() == 1 ? constantGenericValue(*arguments[0]) : std::nullopt;
                        auto reference = infer(*expression.children[1]->children[0], module, locals, owner, depth + 1);
                        const bool mutableView = tupleCallee->value == "get_mut";
                        if (index && reference && reference->kind == "ReferenceType" && reference->children.size() == 1
                            && (!mutableView || reference->value == "mut"))
                        {
                            const auto& tuple = *reference->children[0];
                            if (tuple.value == "Tuple" && *index < tuple.children.size())
                            {
                                auto result = node("ReferenceType", mutableView ? "mut" : "read", expression.range);
                                result->children.push_back(clone(*tuple.children[*index])); return result;
                            }
                        }
                        return {};
                    }
                    if (!expression.children.empty() && builtinMember(*expression.children[0], "Byte", module, locals))
                    {
                        const auto method = expression.children[0]->value;
                        if (method == "from_u8" || method == "to_u8")
                            return node("Type", method == "from_u8" ? "byte" : "u8", expression.range);
                    }
                    if (expression.children.size() == 2 && builtinMember(*expression.children[0], "Slice", module, locals))
                    {
                        const auto& method = expression.children[0]->value;
                        if (method == "from_array" || method == "from_array_mut" || method == "from_vector" || method == "from_vector_mut")
                        {
                            const auto& input = *expression.children[1]->children[0];
                            auto collection = infer(input, module, locals, owner, depth + 1);
                            if (collection && collection->kind == "ReferenceType") collection = std::move(collection->children[0]);
                            NodePtr element;
                            if (collection && (collection->value == "Array" || collection->value == "Vector") && !collection->children.empty()) element = clone(*collection->children[0]);
                            if (!element && method.starts_with("from_array") && input.kind == "Prefix" && !input.children.empty())
                            {
                                const auto target = declaration(*input.children[0], module);
                                const auto* suffixes = target.node ? child(*target.node, "ArraySuffixes") : nullptr;
                                if (suffixes && !suffixes->children.empty())
                                {
                                    element = portableType(*target.node->children[0], target.module, module);
                                    for (std::size_t index = suffixes->children.size(); index > 1; --index)
                                    {
                                        const auto& suffix = *suffixes->children[index - 1];
                                        if (suffix.children.empty()) { element.reset(); break; }
                                        auto outer = node("Type", "Array", suffix.range); outer->children.push_back(std::move(element));
                                        auto extent = node("ConstantTypeArgument", "", suffix.range); extent->children.push_back(clone(*suffix.children[0]));
                                        outer->children.push_back(std::move(extent)); element = std::move(outer);
                                    }
                                }
                            }
                            if (element)
                            {
                                auto result = node("Type", method.ends_with("_mut") ? "MutSlice" : "Slice", expression.range);
                                result->children.push_back(std::move(element)); return result;
                            }
                        }
                    }
                    if (!expression.children.empty() && expression.children[0]->value == "make"
                        && builtinMember(*expression.children[0], "Tuple", module, locals))
                    {
                        auto result = node("Type", "Tuple", expression.range);
                        for (std::size_t index = 1; index < expression.children.size(); ++index)
                        {
                            if (expression.children[index]->children.empty()) return {};
                            auto field = infer(*expression.children[index]->children[0], module, locals, owner, depth + 1);
                            if (!field) return {};
                            result->children.push_back(std::move(field));
                        }
                        return result;
                    }
                    if (expression.children.size() == 3 && builtinMember(*expression.children[0], "Range", module, locals))
                    {
                        const auto& name = expression.children[0]->value;
                        if (name == "new" || name == "closed" || name == "descending" || name == "descending_closed")
                        {
                            const auto& first = *expression.children[1]->children[0]; const auto& second = *expression.children[2]->children[0];
                            auto element = infer(numericLiteral(first) && !numericLiteral(second) ? second : first, module, locals, owner, depth + 1);
                            if (!element) return {};
                            if (numericLiteral(first) && numericLiteral(second) && expected_ && expected_->value == "Range" && expected_->children.size() == 1) element = clone(*expected_->children[0]);
                            auto range = node("Type", "Range", expression.range); range->children.push_back(std::move(element)); return range;
                        }
                    }
                    if (expression.children.size() == 1 && expression.children[0]->value == "value"
                        && builtinMember(*expression.children[0], "Unit", module, locals)) return node("Type", "unit", expression.range);
                    const auto target = declaration(*expression.children[0], module);
                    if (target.node && target.node->kind == "Function" && !target.node->children.empty())
                    {
                        auto result = portableType(*target.node->children[0], target.module, module);
                        if (generic(*target.node))
                        {
                            Bindings bindings;
                            if (expression.children[0]->kind == "GenericName")
                            {
                                const auto arguments = typeArguments(*expression.children[0]);
                                const auto* parameters = child(*target.node, "GenericParameters");
                                if (parameters && arguments.size() <= parameters->children.size())
                                    for (std::size_t index = 0; index < arguments.size(); ++index) bindings[parameters->children[index]->value] = clone(*arguments[index]);
                            }
                            if (inferArguments(target, expression, module, locals, owner, bindings)) substitute(*result, bindings, module, module);
                        }
                        return result;
                    }
                    // 중첩 호출도 실제 호출과 같은 후보 순위로 반환 타입을 결정한다.
                    // Resolve nested-call return types with the same candidate ranking as ordinary calls.
                    const Node* receiver = nullptr;
                    const auto candidates = callCandidates(*expression.children[0], module, locals, owner, receiver);
                    bool failedRequirements = false;
                    auto matches = matchCalls(expression, candidates, receiver, module, locals, owner, failedRequirements);
                    const auto* best = uniqueBestCall(matches);
                    if (best)
                    {
                        auto result = portableType(*best->declaration.node->children[0], best->declaration.module, module);
                        substitute(*result, best->bindings, module, module);
                        return result;
                    }
                }
                if (expression.kind == "Index" && !expression.children.empty())
                {
                    auto collection = infer(*expression.children[0], module, locals, owner, depth + 1);
                    if (collection && collection->kind == "ReferenceType") collection = std::move(collection->children[0]);
                    if (collection && (collection->value == "Slice" || collection->value == "MutSlice" || collection->value == "Vector" || collection->value == "Array") && !collection->children.empty())
                        return clone(*collection->children[0]);
                }
                if (expression.kind == "Member")
                {
                    const auto known = declaration(expression, module);
                    // 모듈의 명시적 변수 타입은 인스턴스 필드나 추론 선언으로 추측하지 않는다.
                    // Read a module variable's explicit type without guessing instance fields or inferred declarations.
                    if (known.node && known.node->kind == "Variable" && !known.owner && !known.node->children.empty()
                        && known.node->children[0]->kind != "InferredType")
                        return portableType(*localParameterType(*known.node), known.module, module);
                    if (known.node && known.node->kind == "Variant" && known.owner) return use(declarations_.at(known.owner), module, "Type", expression.range);
                    auto receiver = infer(*expression.children[0], module, locals, owner, depth + 1);
                    if (receiver && receiver->kind == "ReferenceType") receiver = std::move(receiver->children[0]);
                    const auto type = receiver && receiver->value == "Self" && owner ? declarations_.at(owner)
                        : receiver ? declaration(*receiver, module) : Declaration{};
                    if (type.node)
                        for (const auto& field : type.node->children)
                            if (field->kind == "Variable" && field->value == expression.value && !field->children.empty())
                                return portableType(*localParameterType(*field), type.module, module);
                }
                return {};
            }
            bool unify(const Node& parameter, const Node& actual, const std::set<std::string>& variables, Bindings& bindings, ModuleId module)
            {
                if (parameter.kind == "ConstantTypeArgument" || actual.kind == "ConstantTypeArgument")
                {
                    const auto left = constantGenericValue(parameter), right = constantGenericValue(actual);
                    if (left && right) return *left == *right;
                    if (parameter.kind == "ConstantTypeArgument" && parameter.children.size() == 1)
                        return unify(*parameter.children[0], actual, variables, bindings, module);
                    if (actual.kind == "ConstantTypeArgument" && actual.children.size() == 1)
                        return unify(parameter, *actual.children[0], variables, bindings, module);
                }
                const auto parameterAlias = declaration(parameter, module);
                if (parameterAlias.node && parameterAlias.node->kind == "TypeAlias")
                {
                    auto expanded = portableType(*parameterAlias.node->children[0], parameterAlias.module, module);
                    return unify(*expanded, actual, variables, bindings, module);
                }
                const auto actualAlias = declaration(actual, module);
                if (actualAlias.node && actualAlias.node->kind == "TypeAlias")
                {
                    auto expanded = portableType(*actualAlias.node->children[0], actualAlias.module, module);
                    return unify(parameter, *expanded, variables, bindings, module);
                }
                if ((parameter.kind == "Type" || parameter.kind == "Name") && variables.contains(parameter.value) && parameter.children.empty())
                {
                    const auto existing = bindings.find(parameter.value);
                    if (existing == bindings.end()) { bindings[parameter.value] = clone(actual); return true; }
                    return key(*existing->second, module) == key(actual, module);
                }
                const auto parameterDeclaration = declaration(parameter, module);
                if (parameterDeclaration.node && logicalTypes_.contains(parameterDeclaration.node))
                {
                    const auto& logical = logicalTypes_.at(parameterDeclaration.node);
                    auto restored = portableType(*logical.first, logical.second, module);
                    return unify(*restored, actual, variables, bindings, module);
                }
                const auto actualDeclaration = declaration(actual, module);
                if (actualDeclaration.node && logicalTypes_.contains(actualDeclaration.node))
                {
                    const auto& logical = logicalTypes_.at(actualDeclaration.node);
                    auto restored = portableType(*logical.first, logical.second, module);
                    return unify(parameter, *restored, variables, bindings, module);
                }
                if (parameter.kind == "ReferenceType")
                    return actual.kind == "ReferenceType" && parameter.value == actual.value && unify(*parameter.children[0], *actual.children[0], variables, bindings, module);
                if (flatPath(parameter) != flatPath(actual) && key(parameter, module) != key(actual, module)) return false;
                const auto parameters = typeArguments(parameter), values = typeArguments(actual);
                if (parameters.size() != values.size()) return false;
                for (std::size_t index = 0; index < parameters.size(); ++index)
                    if (!unify(*parameters[index], *values[index], variables, bindings, module)) return false;
                return true;
            }
            void rewriteCallableTemplate(Node& function, const Node* owner)
            {
                std::set<std::string> callableVariables;
                const auto collect = [&](const Node& source)
                {
                    for (const auto& requirement : source.children)
                        if (requirement->kind == "Constraint" && requirement->children.size() >= 2)
                            for (std::size_t index = 1; index < requirement->children.size(); ++index)
                                if (requirement->children[index]->value == "Fn" || requirement->children[index]->value == "FnMut" || requirement->children[index]->value == "FnOnce")
                                    callableVariables.insert(requirement->children[0]->value);
                };
                if (owner) collect(*owner); collect(function);
                if (callableVariables.empty()) return;
                Locals parameters;
                if (const auto* arguments = child(function, "Parameters"))
                    for (const auto& argument : arguments->children)
                        if (!argument->children.empty()) parameters[argument->value] = argument->children[0].get();
                std::function<void(Node&, Locals)> rewrite = [&](Node& current, Locals scope)
                {
                    if (current.kind == "Closure") return;
                    if (current.kind == "Call" && !current.children.empty() && current.children[0]->kind == "Name")
                    {
                        const auto found = scope.find(current.children[0]->value);
                        const Node* type = found == scope.end() ? nullptr : found->second;
                        if (type && type->kind == "ReferenceType" && !type->children.empty()) type = type->children[0].get();
                        if (type && callableVariables.contains(type->value))
                        {
                            auto method = node("Member", "__invoke", current.children[0]->range);
                            method->children.push_back(std::move(current.children[0])); current.children[0] = std::move(method);
                            rewritten_.insert(current.children[0].get());
                        }
                    }
                    for (auto& nested : current.children)
                    {
                        rewrite(*nested, scope);
                        if (current.kind == "Block" && nested->kind == "Variable" && !nested->children.empty()) scope[nested->value] = nested->children[0].get();
                    }
                };
                if (auto* body = child(function, "Block")) rewrite(*body, parameters);
            }
            bool declaresMethod(const Node& type, std::string_view name) const
            {
                return std::any_of(type.children.begin(), type.children.end(), [&](const auto& item) { return item->kind == "Function" && item->value == name; });
            }
            void validateTemplateContract(const Declaration& declaration)
            {
                if (!contractsChecked_.insert(declaration.node).second) return;
                std::set<std::string> variables, copyVariables, floatingVariables, dropVariables, zeroArgumentCallables;
                std::map<std::string, std::set<std::string>> requirements;
                std::map<std::string, std::vector<NodePtr>> methods;
                auto collect = [&](const Node& source)
                {
                    if (const auto* parameters = child(source, "GenericParameters"))
                        for (const auto& parameter : parameters->children) variables.insert(parameter->value);
                    for (const auto& requirement : source.children)
                        if (requirement->kind == "Constraint" && requirement->children.size() >= 2)
                            for (std::size_t index = 1; index < requirement->children.size(); ++index)
                            {
                                const auto& wanted = *requirement->children[index];
                                const auto& subject = requirement->children[0]->value;
                                requirements[subject].insert(key(wanted, declaration.module));
                                if (wanted.value == "Copy") copyVariables.insert(subject);
                                if (wanted.value == "Drop")
                                {
                                    dropVariables.insert(subject);
                                    if (!wanted.children.empty()) error(wanted, "Drop is a lifecycle marker without type arguments.");
                                    continue;
                                }
                                if (floatingContract(wanted, declaration.module))
                                { floatingVariables.insert(subject); copyVariables.insert(subject); }
                                if (wanted.value == "Fn" || wanted.value == "FnMut" || wanted.value == "FnOnce")
                                {
                                    const auto signatureTypes = typeArguments(wanted);
                                    if (signatureTypes.size() != 2 || signatureTypes[0]->value != "Tuple")
                                    { error(wanted, "Callable constraints use Fn<Tuple<Arguments...>, Output>."); continue; }
                                    const auto canonicalBuiltin = [&](const Node& type, std::string_view name) {
                                        const auto* references = originalReferences(type);
                                        if (!references || references->size() != 1 || references->front() >= names_.symbols.size()) return false;
                                        const auto& symbol = names_.symbols[references->front()];
                                        return !symbol.declaration && symbol.module == InvalidModuleId && symbol.name == name;
                                    };
                                    if (signatureTypes[0]->children.empty() && canonicalBuiltin(wanted, wanted.value)
                                        && canonicalBuiltin(*signatureTypes[0], "Tuple")) zeroArgumentCallables.insert(subject);
                                    auto signature = node("Function", "__invoke", wanted.range);
                                    signature->children.push_back(clone(*signatureTypes[1]));
                                    auto parameters = node("Parameters", "", wanted.range);
                                    auto self = node("Parameter", "self", wanted.range);
                                    auto receiver = node("Type", subject, wanted.range);
                                    if (wanted.value != "FnOnce")
                                    { auto reference = node("ReferenceType", wanted.value == "FnMut" ? "mut" : "read", wanted.range); reference->children.push_back(std::move(receiver)); receiver = std::move(reference); }
                                    self->children.push_back(std::move(receiver)); parameters->children.push_back(std::move(self));
                                    for (std::size_t slot = 0; slot < signatureTypes[0]->children.size(); ++slot)
                                    { auto parameter = node("Parameter", "$argument" + std::to_string(slot), wanted.range); parameter->children.push_back(clone(*signatureTypes[0]->children[slot])); parameters->children.push_back(std::move(parameter)); }
                                    signature->children.push_back(std::move(parameters)); methods[subject].push_back(std::move(signature));
                                    continue;
                                }
                                const auto contract = this->declaration(wanted, declaration.module);
                                if (!contract.node) continue;
                                Bindings bindings; bindings["Self"] = node("Type", subject, wanted.range);
                                const auto arguments = typeArguments(wanted);
                                if (const auto* parameters = child(*contract.node, "GenericParameters"); parameters && parameters->children.size() == arguments.size())
                                    for (std::size_t slot = 0; slot < arguments.size(); ++slot) bindings[parameters->children[slot]->value] = clone(*arguments[slot]);
                                for (const auto& member : contract.node->children)
                                    if (member->kind == "Function")
                                    {
                                        auto signature = clone(*member);
                                        relocate(*signature, contract.module, declaration.module);
                                        substitute(*signature, bindings, declaration.module, declaration.module);
                                        methods[subject].push_back(std::move(signature));
                                    }
                            }
                };
                if (declaration.owner) collect(*declaration.owner);
                collect(*declaration.node);
                for (const auto& variable : dropVariables)
                    if (copyVariables.contains(variable)) error(*declaration.node, "A generic type parameter cannot require both Copy and Drop.");
                Locals locals;
                if (const auto* parameters = child(*declaration.node, "Parameters"))
                    for (const auto& parameter : parameters->children) if (const auto* type = localParameterType(*parameter)) locals[parameter->value] = type;
                auto stripReference = [](NodePtr type)
                {
                    if (type && type->kind == "ReferenceType") return std::move(type->children[0]);
                    return type;
                };
                // 추상 매개변수의 멤버 반환형도 계약에서만 가져와 연쇄 접근을 검사한다.
                // Resolve abstract member results from bounds so chained calls cannot escape checking.
                std::function<NodePtr(const Node&, const Locals&, std::size_t)> abstractType;
                std::function<bool(const Node&, const Node&, const Locals&, Bindings*)> methodAccepts;
                abstractType = [&](const Node& expression, const Locals& scope, std::size_t depth) -> NodePtr
                {
                    if (depth > 128) return {};
                    if (expression.kind == "Group") return abstractType(*expression.children[0], scope, depth + 1);
                    if (expression.kind == "Prefix")
                    {
                        auto value = abstractType(*expression.children[0], scope, depth + 1);
                        if (!value) return {};
                        if (expression.value == "*") return value->kind == "ReferenceType" ? std::move(value->children[0]) : NodePtr{};
                        if (expression.value == "&" || expression.value == "&mut")
                        { auto result = node("ReferenceType", expression.value == "&mut" ? "mut" : "read", expression.range); result->children.push_back(std::move(value)); return result; }
                        return value;
                    }
                    if (expression.kind == "NamedInitializer" && !expression.children.empty())
                    {
                        const auto& constructor = *expression.children[0];
                        const auto selected = this->declaration(constructor, declaration.module);
                        if (selected.node && selected.node->kind == "Variant" && selected.owner)
                            return use(declarations_.at(selected.owner), declaration.module, "Type", expression.range);
                        const Node& base = constructor.kind == "GenericName" ? *constructor.children[0] : constructor;
                        auto result = qualified(flatPath(base), "Type", expression.range);
                        if (constructor.kind == "GenericName") for (const auto* argument : typeArguments(constructor)) result->children.push_back(clone(*argument));
                        return result;
                    }
                    if (expression.kind == "Index" && !expression.children.empty())
                    {
                        auto collection = stripReference(abstractType(*expression.children[0], scope, depth + 1));
                        if (collection && (collection->value == "Slice" || collection->value == "MutSlice" || collection->value == "Vector" || collection->value == "Array") && !collection->children.empty())
                            return clone(*collection->children[0]);
                    }
                    if (expression.kind == "Member")
                    {
                        auto receiver = stripReference(abstractType(*expression.children[0], scope, depth + 1));
                        auto target = receiver ? this->declaration(*receiver, declaration.module) : Declaration{};
                        if (receiver && receiver->value == "Self" && declaration.owner) target = declarations_.at(declaration.owner);
                        if (target.node)
                            for (const auto& field : target.node->children)
                                if (field->kind == "Variable" && field->value == expression.value && !field->children.empty())
                                {
                                    auto result = portableType(*localParameterType(*field), target.module, declaration.module);
                                    const auto arguments = receiver ? typeArguments(*receiver) : std::vector<const Node*>{};
                                    if (const auto* parameters = child(*target.node, "GenericParameters"); parameters && parameters->children.size() == arguments.size())
                                    {
                                        Bindings bindings;
                                        for (std::size_t index = 0; index < arguments.size(); ++index) bindings[parameters->children[index]->value] = clone(*arguments[index]);
                                        substitute(*result, bindings, declaration.module, declaration.module);
                                    }
                                    return result;
                                }
                    }
                    if (expression.kind == "Call" && !expression.children.empty())
                    {
                        const Node& callee = expression.children[0]->kind == "GenericName" ? *expression.children[0]->children[0] : *expression.children[0];
                        if (callee.kind == "Member" && !callee.children.empty())
                        {
                            auto receiver = stripReference(abstractType(*callee.children[0], scope, depth + 1));
                            if (receiver && variables.contains(receiver->value))
                                for (const auto& method : methods[receiver->value])
                                    if (method->value == callee.value && !method->children.empty())
                                    {
                                        Bindings bindings;
                                        if (methodAccepts(*method, expression, scope, &bindings))
                                        { auto result = clone(*method->children[0]); substitute(*result, bindings, declaration.module, declaration.module); return result; }
                                    }
                        }
                    }
                    return infer(expression, declaration.module, scope, declaration.owner);
                };
                methodAccepts = [&](const Node& method, const Node& call, const Locals& scope, Bindings* result) -> bool
                {
                    const auto* parameters = child(method, "Parameters");
                    if (!parameters || parameters->children.empty() || parameters->children[0]->value != "self") return false;
                    std::set<std::string> methodVariables;
                    Bindings bindings;
                    if (const auto* generics = child(method, "GenericParameters"))
                    {
                        for (const auto& parameter : generics->children) methodVariables.insert(parameter->value);
                        if (call.children[0]->kind == "GenericName")
                        {
                            const auto explicitTypes = typeArguments(*call.children[0]);
                            if (explicitTypes.size() != generics->children.size()) return false;
                            for (std::size_t index = 0; index < explicitTypes.size(); ++index) bindings[generics->children[index]->value] = clone(*explicitTypes[index]);
                        }
                    }
                    else if (call.children[0]->kind == "GenericName") return false;
                    std::vector<bool> assigned(parameters->children.size(), false); assigned[0] = true;
                    std::size_t positional = 1;
                    for (std::size_t index = 1; index < call.children.size(); ++index)
                    {
                        const auto& argument = *call.children[index];
                        std::size_t slot = positional;
                        if (argument.value.empty()) ++positional;
                        else
                        {
                            slot = parameters->children.size();
                            for (std::size_t candidate = 1; candidate < parameters->children.size(); ++candidate)
                                if (parameters->children[candidate]->value == argument.value) { slot = candidate; break; }
                        }
                        if (slot >= assigned.size() || assigned[slot] || argument.children.empty()) return false;
                        assigned[slot] = true;
                        auto wanted = clone(*parameters->children[slot]->children[0]);
                        substitute(*wanted, bindings, declaration.module, declaration.module);
                        const auto& expression = *argument.children[0];
                        auto actual = abstractType(expression, scope, 0);
                        if (!actual) return false;
                        if (const auto* literal = numericLiteral(expression); literal && !methodVariables.contains(wanted->value))
                        {
                            const auto target = key(*wanted, declaration.module);
                            const bool floating = literal->value.find_first_of(".eE") != std::string::npos && !literal->value.starts_with("0x") && !literal->value.starts_with("0X");
                            const bool accepts = floating ? target == "f32" || target == "f64"
                                : target == "i8" || target == "i16" || target == "i32" || target == "i64" || target == "i128" || target == "u8" || target == "u16" || target == "u32" || target == "u64" || target == "u128" || target == "isize" || target == "usize";
                            if (!accepts) return false;
                        }
                        else if (!unify(*wanted, *actual, methodVariables, bindings, declaration.module)) return false;
                    }
                    for (std::size_t slot = 1; slot < assigned.size(); ++slot)
                        if (!assigned[slot] && !child(*parameters->children[slot], "Default")) return false;
                    for (const auto& variable : methodVariables) if (!bindings.contains(variable)) return false;
                    for (const auto& required : method.children)
                        if (required->kind == "Constraint" && required->children.size() >= 2)
                        {
                            const auto actual = bindings.find(required->children[0]->value);
                            if (actual == bindings.end()) return false;
                            for (std::size_t index = 1; index < required->children.size(); ++index)
                            {
                                auto wanted = clone(*required->children[index]);
                                substitute(*wanted, bindings, declaration.module, declaration.module);
                                if (variables.contains(actual->second->value))
                                { if (!requirements[actual->second->value].contains(key(*wanted, declaration.module))) return false; }
                                else if (!constraint(*actual->second, declaration.module, *wanted)) return false;
                            }
                        }
                    if (result) *result = std::move(bindings);
                    return true;
                };
                auto abstractVariable = [&](const Node& expression, const Locals& scope) -> std::string
                {
                    auto type = stripReference(abstractType(expression, scope, 0));
                    return type && type->kind == "Type" && variables.contains(type->value) ? type->value : std::string{};
                };
                // 계약에 없는 구체 동작을 우연히 사용해도 구체화로 합법화하지 않는다.
                // Specialization must not legitimize incidental concrete behavior absent from a bound.
                auto walk = [&](auto&& visit, const Node& item, Locals scope, const Node* parent,
                                const Node* returnType, std::size_t depth) -> void
                {
                    if (depth > 256) { error(item, "Generic contract traversal limit exceeded."); return; }
                    // 중첩 클로저는 바깥 함수와 독립된 반환 계약과 매개변수 범위를 갖는다.
                    // Nested closures have their own return contract and parameter scope.
                    if (item.kind == "Closure" && item.children.size() == 4)
                    {
                        returnType = item.children[2].get();
                        for (const auto& parameter : item.children[1]->children)
                            if (const auto* type = localParameterType(*parameter)) scope[parameter->value] = type;
                    }
                    if (item.kind == "Member" && !item.children.empty())
                    {
                        const auto variable = abstractVariable(*item.children[0], scope);
                        if (!variable.empty() && std::none_of(methods[variable].begin(), methods[variable].end(), [&](const auto& method) { return method->value == item.value; }))
                            error(item, "Generic contract does not declare member '" + item.value + "'; add an interface requirement.");
                    }
                    if (item.kind == "Return" && !item.children.empty() && returnType)
                    {
                        const auto& wanted = *returnType;
                        auto actual = abstractType(*item.children[0], scope, 0);
                        if (wanted.kind == "Type" && variables.contains(wanted.value))
                        {
                            if (actual && key(wanted, declaration.module) != key(*actual, declaration.module))
                                error(item, "Generic contract cannot produce an arbitrary type parameter from a concrete return value.");
                        }
                        else if (actual && actual->kind == "Type" && variables.contains(actual->value))
                        {
                            const auto target = key(wanted, declaration.module);
                            static const std::set<std::string> integers = {"i8", "i16", "i32", "i64", "i128", "u8", "u16", "u32", "u64", "u128", "isize", "usize"};
                            if (integers.contains(target))
                                error(item, "Generic contract cannot return an arbitrary type parameter as a concrete integer.");
                        }
                    }
                    if (item.kind == "Binary" && item.children.size() >= 2)
                    {
                        const auto left = abstractVariable(*item.children[0], scope), right = abstractVariable(*item.children[1], scope);
                        const bool numeric = item.value == "+" || item.value == "-" || item.value == "*" || item.value == "/"
                            || item.value == "==" || item.value == "!=" || item.value == "<" || item.value == "<=" || item.value == ">" || item.value == ">=";
                        if ((!left.empty() || !right.empty()) && !(numeric && (left.empty() || floatingVariables.contains(left)) && (right.empty() || floatingVariables.contains(right))))
                            error(item, "Generic contract does not guarantee operator '" + item.value + "' for every permitted type.");
                    }
                    if (item.kind == "Prefix" && !item.children.empty() && item.value != "&" && item.value != "&mut" && item.value != "*")
                        if (const auto variable = abstractVariable(*item.children[0], scope); !variable.empty()
                            && !((item.value == "+" || item.value == "-") && floatingVariables.contains(variable)))
                            error(item, "Generic contract does not guarantee this unary operator.");
                    const bool projectionBase = parent && (parent->kind == "Member" || parent->kind == "Index"
                        || (parent->kind == "Prefix" && (parent->value == "&" || parent->value == "&mut"))
                        || (parent->kind == "Assignment" && parent->children[0].get() == &item));
                    if (!projectionBase && (item.kind == "Member" || item.kind == "Index" || (item.kind == "Prefix" && item.value == "*")))
                    {
                        bool borrowed = item.kind == "Prefix" || item.kind == "Index";
                        if (item.kind == "Member" && !item.children.empty())
                        {
                            const auto receiver = abstractType(*item.children[0], scope, 0);
                            const auto& source = *item.children[0];
                            borrowed = (receiver && receiver->kind == "ReferenceType") || (source.kind == "Group" && !source.children.empty() && source.children[0]->kind == "Prefix" && source.children[0]->value == "*")
                                || (source.kind == "Prefix" && source.value == "*");
                        }
                        const auto projectedType = abstractType(item, scope, 0);
                        const auto variable = projectedType && projectedType->kind == "Type" && variables.contains(projectedType->value) ? projectedType->value : std::string{};
                        if (borrowed && !variable.empty() && !copyVariables.contains(variable))
                            error(item, "Generic contract requires implements Copy to read an owned value from borrowed storage.");
                    }
                    if (item.kind == "Call" && !item.children.empty())
                    {
                        const Node& callee = item.children[0]->kind == "GenericName" ? *item.children[0]->children[0] : *item.children[0];
                        if (callee.kind == "Member" && callee.children.size() == 1 && item.children.size() == 3 && !item.children[2]->children.empty())
                        {
                            const auto& namespaceExpression = *callee.children[0];
                            const auto& namespaceName = namespaceExpression.kind == "GenericName" && !namespaceExpression.children.empty()
                                ? *namespaceExpression.children[0] : namespaceExpression;
                            const auto task = this->declaration(namespaceName, declaration.module);
                            if (task.node && child(*task.node, "CompilerTask") && graph_.modules[task.module]->package == "$task")
                                for (const auto& method : task.node->children)
                                {
                                    const auto* marker = method->kind == "Function" && method->value == callee.value
                                        && child(*method, "CompilerIntrinsic") ? child(*method, "CompilerTaskOperation") : nullptr;
                                    const auto* operation = marker ? taskOperation(marker->value) : nullptr;
                                    if (!operation || !operation->worker) continue;
                                    const auto variable = abstractVariable(*item.children[2]->children[0], scope);
                                    // 구체화할 때 우연히 클로저가 들어와도 선언에 없는 호출 계약을 추가하지 않는다.
                                    // Incidental concrete closure arguments cannot add a callable contract absent from the template.
                                    if (!variable.empty() && !zeroArgumentCallables.contains(variable))
                                        error(item, "Generic Task workers require a zero-argument Fn, FnMut, or FnOnce interface requirement.");
                                }
                        }
                        if (callee.kind == "Member" && !callee.children.empty())
                        {
                            const auto variable = abstractVariable(*callee.children[0], scope);
                            if (!variable.empty())
                            {
                                const bool accepts = std::any_of(methods[variable].begin(), methods[variable].end(), [&](const auto& method)
                                { return method->value == callee.value && methodAccepts(*method, item, scope, nullptr); });
                                if (!accepts) error(item, "Generic contract has no matching method signature for this call.");
                            }
                        }
                        const Node* receiver = nullptr;
                        const auto candidates = callCandidates(*item.children[0], declaration.module, scope, declaration.owner, receiver);
                        for (const auto& target : candidates)
                        {
                            if (target.node->value == "drop" && target.owner && implementsDrop(*target.owner))
                                error(item, "Drop cleanup methods cannot be called explicitly.");
                            if (generic(*target.node))
                            {
                                Bindings bindings;
                                if (item.children[0]->kind == "GenericName")
                                {
                                    const auto explicitTypes = typeArguments(*item.children[0]);
                                    const auto* parameters = child(*target.node, "GenericParameters");
                                    if (parameters && explicitTypes.size() == parameters->children.size())
                                        for (std::size_t index = 0; index < explicitTypes.size(); ++index) bindings[parameters->children[index]->value] = clone(*explicitTypes[index]);
                                }
                                if (!inferArguments(target, item, declaration.module, scope, declaration.owner, bindings, receiver)) continue;
                                for (const auto& requirement : target.node->children)
                                    if (requirement->kind == "Constraint" && requirement->children.size() >= 2)
                                    {
                                        const auto actual = bindings.find(requirement->children[0]->value);
                                        if (actual == bindings.end() || !variables.contains(actual->second->value)) continue;
                                        for (std::size_t index = 1; index < requirement->children.size(); ++index)
                                        {
                                            auto wanted = portableType(*requirement->children[index], target.module, declaration.module);
                                            substitute(*wanted, bindings, declaration.module, declaration.module);
                                            if (!requirements[actual->second->value].contains(key(*wanted, declaration.module)))
                                                error(item, "Generic contract does not guarantee the called function's implements requirement.");
                                        }
                                    }
                            }
                        }
                    }
                    for (const auto& nested : item.children)
                    {
                        visit(visit, *nested, scope, &item, returnType, depth + 1);
                        if ((item.kind == "Block" || item.kind == "CaseScope" || item.kind == "For") && nested->kind == "Variable" && !nested->children.empty())
                        {
                            if (nested->children[0]->kind != "InferredType") scope[nested->value] = nested->children[0].get();
                            else if (nested->children.size() > 1)
                            {
                                auto type = abstractType(*nested->children.back(), scope, 0);
                                if (type) { scope[nested->value] = type.get(); inferred_.push_back(std::move(type)); }
                            }
                        }
                    }
                };
                if (const auto* body = child(*declaration.node, "Block"))
                    walk(walk, *body, locals, nullptr, declaration.node->children.empty() ? nullptr : declaration.node->children[0].get(), 0);
                // Copy 보장이 없는 매개변수는 구체 호출의 우연한 Copy 특성과 무관하게 이동한다.
                // Unbounded values move regardless of an incidental concrete caller being Copy.
                using Moves = std::map<std::string, bool>;
                auto tracked = [&](const Node& type) { return type.kind == "Type" && variables.contains(type.value) && !copyVariables.contains(type.value); };
                Moves initialMoves;
                for (const auto& [name, type] : locals) if (tracked(*type)) initialMoves[name] = false;
                auto merge = [](Moves& destination, const Moves& left, const Moves& right)
                {
                    for (auto& [name, moved] : destination)
                    {
                        const auto first = left.find(name), second = right.find(name);
                        moved = (first != left.end() && first->second) || (second != right.end() && second->second);
                    }
                };
                std::function<void(const Node&, Moves&, const Locals&, bool, std::size_t)> consume;
                consume = [&](const Node& item, Moves& state, const Locals& scope, bool owning, std::size_t depth)
                {
                    if (depth > 256) { error(item, "Generic move traversal limit exceeded."); return; }
                    if (item.kind == "Name")
                    {
                        if (const auto found = state.find(item.value); found != state.end())
                        {
                            if (found->second) error(item, "Generic contract uses a value after a possible move; require Copy or retain ownership.");
                            if (owning) found->second = true;
                        }
                        return;
                    }
                    if (item.kind == "Group" && !item.children.empty()) { consume(*item.children[0], state, scope, owning, depth + 1); return; }
                    if (item.kind == "Prefix" && !item.children.empty() && (item.value == "&" || item.value == "&mut" || item.value == "*"))
                    { consume(*item.children[0], state, scope, false, depth + 1); return; }
                    if (item.kind == "Member" && !item.children.empty()) { consume(*item.children[0], state, scope, false, depth + 1); return; }
                    if (item.kind == "Conditional" && item.children.size() == 3)
                    {
                        consume(*item.children[0], state, scope, true, depth + 1);
                        auto left = state, right = state;
                        consume(*item.children[1], left, scope, owning, depth + 1);
                        consume(*item.children[2], right, scope, owning, depth + 1);
                        merge(state, left, right); return;
                    }
                    if (item.kind == "Assignment" && item.children.size() >= 2)
                    {
                        consume(*item.children[1], state, scope, true, depth + 1);
                        if (item.children[0]->kind == "Name" && state.contains(item.children[0]->value)) state[item.children[0]->value] = false;
                        else consume(*item.children[0], state, scope, false, depth + 1);
                        return;
                    }
                    if (item.kind == "Call" && !item.children.empty())
                    {
                        const Node& callee = item.children[0]->kind == "GenericName" ? *item.children[0]->children[0] : *item.children[0];
                        if (callee.kind == "Member" && !callee.children.empty())
                        {
                            bool consumesReceiver = false;
                            const auto variable = abstractVariable(*callee.children[0], scope);
                            if (!variable.empty())
                                for (const auto& method : methods[variable])
                                    if (method->value == callee.value && methodAccepts(*method, item, scope, nullptr))
                                    {
                                        const auto* parameters = child(*method, "Parameters");
                                        consumesReceiver = parameters && !parameters->children.empty() && parameters->children[0]->children[0]->kind != "ReferenceType";
                                    }
                            consume(*callee.children[0], state, scope, consumesReceiver, depth + 1);
                        }
                        for (std::size_t index = 1; index < item.children.size(); ++index) consume(*item.children[index], state, scope, true, depth + 1);
                        return;
                    }
                    for (const auto& nested : item.children) consume(*nested, state, scope, owning, depth + 1);
                };
                std::function<bool(const Node&, Moves&, Locals, std::size_t)> statements;
                const auto returnsFromFunction = [&](const auto& self, const Node& statement) -> bool
                {
                    if (statement.kind == "Return") return true;
                    if (statement.kind == "If" && statement.children.size() >= 3)
                        return self(self, *statement.children[1]) && self(self, *statement.children[2]);
                    if (statement.kind == "Block" || statement.kind == "CaseScope")
                        for (const auto& nested : statement.children)
                        {
                            if (nested->kind == "Break" || nested->kind == "Continue") return false;
                            if (self(self, *nested)) return true;
                        }
                    return false;
                };
                statements = [&](const Node& item, Moves& state, Locals scope, std::size_t depth) -> bool
                {
                    if (depth > 256) { error(item, "Generic move traversal limit exceeded."); return false; }
                    if (item.kind == "Block" || item.kind == "CaseScope")
                    {
                        for (const auto& nested : item.children)
                        {
                            if (!statements(*nested, state, scope, depth + 1)) return false;
                            if (nested->kind == "Variable" && !nested->children.empty())
                            {
                                auto type = nested->children[0]->kind == "InferredType" && nested->children.size() > 1
                                    ? abstractType(*nested->children.back(), scope, 0) : clone(*nested->children[0]);
                                if (type) { scope[nested->value] = type.get(); inferred_.push_back(std::move(type)); }
                            }
                        }
                        return true;
                    }
                    if (item.kind == "Variable")
                    {
                        const Node* initializer = nullptr;
                        for (std::size_t index = 1; index < item.children.size(); ++index)
                            if (item.children[index]->kind != "Modifier" && item.children[index]->kind != "ArraySuffixes") initializer = item.children[index].get();
                        if (initializer) consume(*initializer, state, scope, true, depth + 1);
                        auto type = !item.children.empty() && item.children[0]->kind == "InferredType" && initializer ? abstractType(*initializer, scope, 0)
                            : !item.children.empty() ? clone(*item.children[0]) : NodePtr{};
                        if (type && tracked(*type)) state[item.value] = false;
                        return true;
                    }
                    if (item.kind == "Return") { for (const auto& value : item.children) consume(*value, state, scope, true, depth + 1); return false; }
                    if (item.kind == "Break" || item.kind == "Continue") return false;
                    if (item.kind == "If" && item.children.size() >= 2)
                    {
                        consume(*item.children[0], state, scope, true, depth + 1);
                        auto left = state, right = state;
                        const bool leftLives = statements(*item.children[1], left, scope, depth + 1);
                        const bool rightLives = item.children.size() < 3 || statements(*item.children[2], right, scope, depth + 1);
                        if (leftLives && rightLives) merge(state, left, right);
                        else if (leftLives) state = left;
                        else if (rightLives) state = right;
                        return leftLives || rightLives;
                    }
                    if (item.kind == "While" || item.kind == "For" || item.kind == "RangeFor" || item.kind == "EnhancedFor")
                    {
                        auto after = state;
                        for (const auto& nested : item.children) statements(*nested, after, scope, depth + 1);
                        for (const auto& [name, moved] : after)
                            if (moved && state.contains(name) && !state[name]) error(item, "Generic contract cannot move an unbounded outer value across a loop iteration.");
                        merge(state, state, after); return true;
                    }
                    if (item.kind == "Switch" && !item.children.empty())
                    {
                        consume(*item.children[0], state, scope, item.value == "move", depth + 1);
                        const auto before = state;
                        for (std::size_t index = 1; index < item.children.size(); ++index)
                        {
                            auto branch = before;
                            bool returned = false;
                            for (const auto& nested : item.children[index]->children)
                                if (nested->kind == "CaseScope")
                                {
                                    statements(*nested, branch, scope, depth + 1);
                                    returned = returnsFromFunction(returnsFromFunction, *nested);
                                }
                            // 함수를 반환한 case의 이동은 switch 뒤로 합류하지 않는다. break는 합류한다.
                            // Moves in a returning case do not rejoin after the switch; moves before break do.
                            if (!returned) merge(state, state, branch);
                        }
                        return true;
                    }
                    consume(item, state, scope, true, depth + 1); return true;
                };
                if (const auto* body = child(*declaration.node, "Block")) statements(*body, initialMoves, locals, 0);
            }
            const Node* numericLiteral(const Node& expression) const
            {
                const Node* value = &expression;
                while ((value->kind == "Group" || (value->kind == "Prefix" && (value->value == "-" || value->value == "+"))) && !value->children.empty()) value = value->children[0].get();
                return value->kind == "Literal" && !value->value.empty() && (value->value[0] >= '0' && value->value[0] <= '9') ? value : nullptr;
            }
            bool requirementsHold(const Declaration& function, const Bindings& bindings, ModuleId module)
            {
                for (const auto& requirement : function.node->children)
                    if (requirement->kind == "Constraint" && requirement->children.size() >= 2)
                    {
                        auto concrete = portableType(*requirement->children[0], function.module, module);
                        substitute(*concrete, bindings, module, module);
                        for (std::size_t index = 1; index < requirement->children.size(); ++index)
                        {
                            auto required = portableType(*requirement->children[index], function.module, module);
                            substitute(*required, bindings, module, module);
                            if (!constraint(*concrete, module, *required)) return false;
                        }
                    }
                return true;
            }
            void fillExpected(const Node& pattern, const Node& actual, const std::set<std::string>& variables, Bindings& bindings, ModuleId module)
            {
                const auto patternAlias = declaration(pattern, module);
                if (patternAlias.node && patternAlias.node->kind == "TypeAlias")
                {
                    auto expanded = portableType(*patternAlias.node->children[0], patternAlias.module, module);
                    fillExpected(*expanded, actual, variables, bindings, module);
                    return;
                }
                const auto actualAlias = declaration(actual, module);
                if (actualAlias.node && actualAlias.node->kind == "TypeAlias")
                {
                    auto expanded = portableType(*actualAlias.node->children[0], actualAlias.module, module);
                    fillExpected(pattern, *expanded, variables, bindings, module);
                    return;
                }
                if (pattern.kind == "Type" && variables.contains(pattern.value) && pattern.children.empty())
                {
                    if (!bindings.contains(pattern.value)) bindings[pattern.value] = clone(actual);
                    return;
                }
                const auto actualDeclaration = declaration(actual, module);
                if (actualDeclaration.node && logicalTypes_.contains(actualDeclaration.node))
                {
                    const auto& logical = logicalTypes_.at(actualDeclaration.node);
                    auto restored = portableType(*logical.first, logical.second, module);
                    fillExpected(pattern, *restored, variables, bindings, module); return;
                }
                if (pattern.kind == "ReferenceType" && actual.kind == "ReferenceType" && pattern.value == actual.value)
                { fillExpected(*pattern.children[0], *actual.children[0], variables, bindings, module); return; }
                if (flatPath(pattern) != flatPath(actual)) return;
                const auto patterns = typeArguments(pattern), arguments = typeArguments(actual);
                if (patterns.size() == arguments.size())
                    for (std::size_t index = 0; index < patterns.size(); ++index) fillExpected(*patterns[index], *arguments[index], variables, bindings, module);
            }
            std::vector<Declaration> callCandidates(const Node& expression, ModuleId module, const Locals& locals, Node* owner, const Node*& receiver)
            {
                const Node& callee = expression.kind == "GenericName" ? *expression.children[0] : expression;
                auto candidates = references(callee, module);
                if (!candidates.empty() || callee.kind != "Member" || callee.children.empty()) return candidates;
                auto type = infer(*callee.children[0], module, locals, owner);
                bool dynamic = false;
                const auto target = receiverDeclaration(std::move(type), module, owner, &dynamic);
                if (target.node)
                {
                    receiver = callee.children[0].get();
                    for (const auto& method : target.node->children)
                        if (method->kind == "Function" && method->value == callee.value && (!dynamic || !generic(*method)))
                        {
                            const auto known = declarations_.find(method.get());
                            candidates.push_back(known == declarations_.end() ? Declaration{method.get(), target.module, InvalidSymbolId, target.node} : known->second);
                        }
                }
                return candidates;
            }
            bool inferArguments(const Declaration& function, const Node& call, ModuleId module, const Locals& locals, Node* owner, Bindings& bindings, const Node* receiver = nullptr, int* literalPreference = nullptr)
            {
                const auto* parameters = child(*function.node, "Parameters");
                const auto* generics = child(*function.node, "GenericParameters");
                std::set<std::string> variables;
                if (generics) for (const auto& item : generics->children) variables.insert(item->value);
                const bool instance = parameters && !parameters->children.empty() && parameters->children[0]->value == "self";
                if (receiver && !instance) return false;
                if (receiver && function.owner)
                    if (const auto* ownerParameters = child(*function.owner, "GenericParameters"))
                    {
                        auto actualOwner = infer(*receiver, module, locals, owner);
                        if (actualOwner && actualOwner->kind == "ReferenceType") actualOwner = std::move(actualOwner->children[0]);
                        if (actualOwner)
                        {
                            const auto actualDeclaration = declaration(*actualOwner, module);
                            if (actualDeclaration.node && logicalTypes_.contains(actualDeclaration.node))
                            {
                                const auto& logical = logicalTypes_.at(actualDeclaration.node);
                                actualOwner = portableType(*logical.first, logical.second, module);
                            }
                            const auto ownerArguments = typeArguments(*actualOwner);
                            if (ownerArguments.size() == ownerParameters->children.size())
                                for (std::size_t index = 0; index < ownerArguments.size(); ++index)
                                {
                                    // 수신자 타입의 인자를 먼저 연결하여 외부 모듈의 추상 필드 타입을 그대로 비교하지 않는다.
                                    // Bind receiver-owner arguments before comparing abstract member types across modules.
                                    const auto& name = ownerParameters->children[index]->value;
                                    if (!variables.contains(name)) bindings[name] = clone(*ownerArguments[index]);
                                }
                        }
                    }
                const std::size_t offset = receiver ? 1 : 0;
                if (!parameters) return false;
                const bool variadic = !parameters->children.empty() && parameters->children.back()->kind == "VariadicParameter";
                const auto pack = variadic ? parameters->children.size() - 1 : parameters->children.size();
                if (!variadic && call.children.size() - 1 + offset > parameters->children.size()) return false;
                std::vector<bool> assigned(parameters->children.size(), false);
                if (receiver) assigned[0] = true;
                std::vector<std::pair<const Node*, const Node*>> arguments;
                std::size_t position = offset;
                for (std::size_t index = 1; index < call.children.size(); ++index)
                {
                    const auto& argument = *call.children[index];
                    std::size_t parameter = position;
                    if (!argument.value.empty())
                    {
                        parameter = parameters->children.size();
                        for (std::size_t candidate = offset; candidate < parameters->children.size(); ++candidate)
                            if (parameters->children[candidate]->value == argument.value) { parameter = candidate; break; }
                    }
                    else ++position;
                    if (variadic && argument.value.empty() && parameter >= pack) parameter = pack;
                    if (parameter >= parameters->children.size() || (assigned[parameter] && parameter != pack)
                        || (variadic && parameter == pack && !argument.value.empty()) || argument.children.empty()) return false;
                    assigned[parameter] = true;
                    arguments.emplace_back(parameters->children[parameter]->kind == "VariadicParameter"
                        ? parameters->children[parameter]->children[0].get() : localParameterType(*parameters->children[parameter]), argument.children[0].get());
                }
                for (std::size_t index = offset; index < pack; ++index)
                    if (!assigned[index] && !child(*parameters->children[index], "Default")) return false;
                // 이미 타입이 있는 인수를 먼저 모아 리터럴의 소스 위치가 추론을 바꾸지 않게 한다.
                // Gather typed arguments first so literal source position cannot change inference.
                for (const auto& [parameter, value] : arguments)
                    if (!numericLiteral(*value))
                    {
                        auto actual = infer(*value, module, locals, owner);
                        auto wanted = portableType(*parameter, function.module, module);
                        substitute(*wanted, bindings, module, module);
                        if (!actual || !unify(*wanted, *actual, variables, bindings, module)) return false;
                    }
                for (const auto& [parameter, value] : arguments)
                    if (const auto* literal = numericLiteral(*value))
                    {
                        auto wanted = portableType(*parameter, function.module, module);
                        substitute(*wanted, bindings, module, module);
                        const auto literalType = infer(*literal, module, locals, owner);
                        const bool unresolved = wanted->kind == "Type" && variables.contains(wanted->value);
                        if (unresolved)
                        { if (!unify(*wanted, *literalType, variables, bindings, module)) return false; wanted = clone(*literalType); }
                        const auto numeric = key(*wanted, module);
                        const bool integral = numeric == "i8" || numeric == "i16" || numeric == "i32" || numeric == "i64" || numeric == "i128" || numeric == "isize" || numeric == "u8" || numeric == "u16" || numeric == "u32" || numeric == "u64" || numeric == "u128" || numeric == "usize";
                        const bool floating = numeric == "f16" || numeric == "bf16" || numeric == "f32" || numeric == "f64";
                        if (!numericSuffix(literal->value).empty()) { if (numeric != literalType->value) return false; }
                        else if (literalType->value == "f64" ? !floating : !integral) return false;
                        if (literalPreference && numeric == literalType->value) ++*literalPreference;
                    }
                if (expected_ && expected_->kind != "InferredType" && !function.node->children.empty())
                {
                    auto resultType = portableType(*function.node->children[0], function.module, module);
                    fillExpected(*resultType, *expected_, variables, bindings, module);
                }
                if (generics && !completeBindings(function, bindings, module, false)) return false;
                return true;
            }
            struct CallMatch
            {
                Declaration declaration;
                Bindings bindings;
                int literals;
                bool generic;
                bool variadic;
            };
            static bool worseCall(const CallMatch& left, const CallMatch& right)
            {
                return left.literals != right.literals ? left.literals < right.literals
                    : left.generic != right.generic ? left.generic && !right.generic : left.variadic && !right.variadic;
            }
            static bool sameCallRank(const CallMatch& left, const CallMatch& right)
            {
                return left.literals == right.literals && left.generic == right.generic && left.variadic == right.variadic;
            }
            static const CallMatch* uniqueBestCall(const std::vector<CallMatch>& matches)
            {
                if (matches.empty()) return nullptr;
                const auto best = std::max_element(matches.begin(), matches.end(), worseCall);
                return std::count_if(matches.begin(), matches.end(), [&](const CallMatch& value) { return sameCallRank(value, *best); }) == 1 ? &*best : nullptr;
            }
            std::vector<CallMatch> matchCalls(const Node& call, const std::vector<Declaration>& candidates, const Node* receiver,
                ModuleId module, const Locals& locals, Node* owner, bool& failedRequirements)
            {
                const auto& callee = *call.children[0];
                const bool explicitArguments = callee.kind == "GenericName";
                std::vector<CallMatch> matches;
                for (const auto& candidate : candidates)
                {
                    if (candidate.node->kind != "Function") continue;
                    if (child(*candidate.node, "CompilerUnavailable")) { failedRequirements = true; continue; }
                    const bool isGeneric = generic(*candidate.node);
                    if (explicitArguments && !isGeneric) continue;
                    Bindings bindings;
                    if (explicitArguments)
                    {
                        const auto arguments = typeArguments(callee);
                        const auto* parameters = child(*candidate.node, "GenericParameters");
                        if (arguments.size() > parameters->children.size()) continue;
                        for (std::size_t index = 0; index < arguments.size(); ++index) bindings[parameters->children[index]->value] = clone(*arguments[index]);
                    }
                    int literals = 0;
                    if (!inferArguments(candidate, call, module, locals, owner, bindings, receiver, &literals)) continue;
                    if (!requirementsHold(candidate, bindings, module)) { failedRequirements = true; continue; }
                    const auto* parameters = child(*candidate.node, "Parameters");
                    const bool variadic = parameters && !parameters->children.empty() && parameters->children.back()->kind == "VariadicParameter";
                    matches.push_back({candidate, std::move(bindings), literals, isGeneric, variadic});
                }
                return matches;
            }
            std::string carrierName(const Declaration& declaration)
            {
                const auto* carrier = declaration.node ? child(*declaration.node, "CompilerCarrier") : nullptr;
                return carrier ? carrier->value : std::string{};
            }
            std::vector<NodePtr> carrierArguments(const Node& type, ModuleId module, std::string_view wanted)
            {
                std::vector<NodePtr> result;
                const auto target = declaration(type, module);
                if (carrierName(target) != wanted) return result;
                const auto arguments = typeArguments(type);
                if (!arguments.empty())
                {
                    for (const auto* argument : arguments) result.push_back(clone(*argument));
                    return result;
                }
                for (const auto& variant : target.node->children)
                    if (variant->kind == "Variant")
                        for (const auto& field : variant->children)
                            if (field->kind == "Variable" && !field->children.empty()) result.push_back(portableType(*field->children[0], target.module, module));
                return result;
            }
            bool coreConstructor(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                if (call.children.empty()) return false;
                const Node* callee = call.children[0].get();
                const Node* explicitList = callee->kind == "GenericName" ? child(*callee, "TypeArguments") : nullptr;
                if (explicitList) callee = callee->children[0].get();
                if (callee->kind != "Member" || callee->children.empty()) return false;
                const auto target = declaration(*callee->children[0], module);
                const auto kind = carrierName(target);
                const bool result = kind == "Result";
                const bool optional = kind == "Optional";
                const auto method = callee->value;
                if ((!result || (method != "ok" && method != "err")) && (!optional || (method != "some" && method != "none"))) return false;
                const bool empty = optional && method == "none";
                if (call.children.size() != (empty ? 1U : 2U) || (!empty && call.children[1]->children.empty()))
                { error(call, "Carrier constructor has an invalid argument count."); return true; }
                if (!empty && !call.children[1]->value.empty() && call.children[1]->value != (method == "err" ? "error" : "value"))
                { error(call, "Unknown carrier constructor argument name."); return true; }
                std::vector<NodePtr> arguments;
                if (explicitList)
                    for (const auto& argument : explicitList->children) arguments.push_back(clone(*argument));
                else
                {
                    const auto ownerArguments = typeArguments(*callee->children[0]);
                    for (const auto* argument : ownerArguments) arguments.push_back(clone(*argument));
                    if (arguments.empty() && !generic(*target.node)) arguments = carrierArguments(*callee->children[0], module, kind);
                    if (arguments.empty() && expected_) arguments = carrierArguments(*expected_, module, kind);
                    if (arguments.empty() && optional && !empty)
                    {
                        // 문맥 없는 Some의 중첩 호출부터 구체화하여 템플릿의 T를 값 타입으로 오인하지 않는다.
                        // Materialize a context-free Some argument before inferring its concrete payload type.
                        { ExpectedGuard context(expected_); expected_ = nullptr; process(*call.children[1]->children[0], module, locals, owner, depth + 1); }
                        auto inferred = infer(*call.children[1]->children[0], module, locals, owner);
                        if (inferred) arguments.push_back(std::move(inferred));
                    }
                }
                if (arguments.size() != (result ? 2U : 1U))
                { error(call, "Carrier constructor needs a complete contextual type or explicit type arguments (" + kind + "." + method + (expected_ ? ", context " + spelling(*expected_) : ", no context") + ")."); return true; }
                auto core = target;
                if (!generic(*core.node))
                    for (const auto& [address, candidate] : declarations_)
                    {
                        (void)address;
                        if (generic(*candidate.node) && carrierName(candidate) == kind) { core = candidate; break; }
                    }
                std::vector<const Node*> argumentPointers;
                for (const auto& argument : arguments) argumentPointers.push_back(argument.get());
                const auto concrete = specialize(core, argumentPointers, module, call, depth);
                if (!concrete.node) return true;
                const auto variant = result ? method == "ok" ? "Ok" : "Err" : method == "some" ? "Some" : "None";
                auto selected = node("Member", variant, call.range);
                selected->children.push_back(use(concrete, module, "Name", call.range));
                NodePtr replacement;
                if (empty) replacement = std::move(selected);
                else
                {
                    replacement = node("NamedInitializer", "", call.range);
                    replacement->children.push_back(std::move(selected));
                    auto field = node("FieldInitializer", method == "err" ? "error" : "value", call.children[1]->range);
                    field->children.push_back(std::move(call.children[1]->children[0]));
                    replacement->children.push_back(std::move(field));
                }
                call = std::move(*replacement); rewritten_.insert(&call);
                process(call, module, locals, owner, depth + 1);
                return true;
            }
            Declaration builtinDeclaration(std::string_view name)
            {
                for (const auto& [address, candidate] : declarations_)
                {
                    (void)address;
                    if (candidate.node->value == name && graph_.modules[candidate.module]->name == "$core") return candidate;
                }
                return {};
            }
            NodePtr specializeBuiltin(std::string_view name, std::vector<NodePtr> arguments, ModuleId module, const Node& source, std::size_t depth)
            {
                const auto declaration = builtinDeclaration(name);
                if (!declaration.node) { error(source, "Missing core type " + std::string(name) + "."); return {}; }
                if (!generic(*declaration.node)) return use(declaration, module, "Type", source.range);
                std::vector<const Node*> pointers;
                for (const auto& argument : arguments) pointers.push_back(argument.get());
                const auto concrete = specialize(declaration, pointers, module, source, depth + 1);
                return concrete.node ? use(concrete, module, "Type", source.range) : NodePtr{};
            }
            NodePtr closureCaptureType(const Node& expression, ModuleId module, const Locals& locals, Node* owner)
            {
                auto type = infer(expression, module, locals, owner);
                const auto target = declaration(expression, module);
                const auto* suffixes = target.node ? child(*target.node, "ArraySuffixes") : nullptr;
                if (suffixes && !suffixes->children.empty())
                {
                    type = portableType(*target.node->children[0], target.module, module);
                    for (auto suffix = suffixes->children.rbegin(); suffix != suffixes->children.rend(); ++suffix)
                    {
                        if ((*suffix)->children.empty()) return {};
                        auto outer = node("Type", "Array", (*suffix)->range); outer->children.push_back(std::move(type));
                        auto extent = node("ConstantTypeArgument", "", (*suffix)->range); extent->children.push_back(clone(*(*suffix)->children[0]));
                        outer->children.push_back(std::move(extent)); type = std::move(outer);
                    }
                }
                if (type && owner)
                {
                    Bindings bindings; bindings["Self"] = use(declarations_.at(owner), module, "Type", expression.range);
                    substitute(*type, bindings, module, module);
                }
                return type;
            }
            void lowerClosure(Node& closure, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                if (closure.children.size() != 4) { error(closure, "Malformed closure expression."); return; }
                if (generated_ >= maximumGeneratedDeclarations)
                { error(closure, "Closure generation exceeds the 4096 generated-declaration limit."); return; }
                const auto identity = "$Closure" + std::to_string(++closureSequence_);
                auto environment = node("Struct", identity, closure.range);
                environment->children.push_back(node("Modifier", "public", closure.range));
                auto construction = node("NamedInitializer", "", closure.range);
                construction->children.push_back(node("Name", identity, closure.range));
                struct CaptureInfo { std::string field; std::string mode; };
                std::map<std::string, CaptureInfo> captures;
                std::set<std::string> owningArrays;
                bool exclusive = false; bool consuming = false;
                for (auto& capture : closure.children[0]->children)
                {
                    if (capture->children.size() != 2) { error(*capture, "Malformed closure capture."); return; }
                    const auto mode = capture->children[0]->value;
                    auto type = closureCaptureType(*capture->children[1], module, locals, owner);
                    if (!type) { error(*capture, "Cannot infer closure capture type for '" + capture->value + "'."); return; }
                    std::set<std::string> visiting;
                    if (mode == "value" && !copy(*type, module, visiting))
                    {
                        if (type->value == "Array") owningArrays.insert(capture->value);
                        else consuming = true;
                    }
                    if (mode == "mut") exclusive = true;
                    if (mode != "value")
                    { auto reference = node("ReferenceType", mode == "mut" ? "mut" : "read", capture->range); reference->children.push_back(std::move(type)); type = std::move(reference); }
                    const auto fieldName = "$capture_" + capture->value;
                    captures[capture->value] = {fieldName, mode};
                    auto field = node("Variable", fieldName, capture->range); field->children.push_back(std::move(type));
                    field->children.push_back(node("Modifier", "public", capture->range)); environment->children.push_back(std::move(field));
                    auto initial = node("FieldInitializer", fieldName, capture->range);
                    auto expression = std::move(capture->children[1]);
                    if (mode != "value")
                    { auto borrow = node("Prefix", mode == "mut" ? "&mut" : "&", capture->range); borrow->children.push_back(std::move(expression)); expression = std::move(borrow); }
                    initial->children.push_back(std::move(expression)); construction->children.push_back(std::move(initial));
                }
                // 배열을 캡처할 때의 이동과 본문에서 배열을 소비하는 호출 권한은 구별한다.
                // Moving an array into a capture differs from consuming it during invocation.
                const auto classifyArrayUse = [&](const auto& self, const Node& expression, bool owning) -> void
                {
                    if (expression.kind == "Name")
                    { if (owning && owningArrays.contains(expression.value)) consuming = true; return; }
                    if (expression.kind == "Closure")
                    {
                        if (const auto* nested = child(expression, "Captures"))
                            for (const auto& capture : nested->children)
                                if (capture->children.size() == 2) self(self, *capture->children[1], capture->children[0]->value == "value");
                        return;
                    }
                    if (expression.kind == "Index" && expression.children.size() == 2)
                    {
                        self(self, *expression.children[0], false); self(self, *expression.children[1], true);
                        if (owning)
                        {
                            auto element = infer(expression, module, locals, owner); std::set<std::string> visiting;
                            if (element && !copy(*element, module, visiting))
                            {
                                const auto& base = *expression.children[0];
                                if (base.kind == "Name" && owningArrays.contains(base.value)) consuming = true;
                            }
                        }
                        return;
                    }
                    if (expression.kind == "Prefix" && !expression.children.empty()
                        && (expression.value == "&" || expression.value == "&mut" || expression.value == "*"))
                    { self(self, *expression.children[0], false); return; }
                    if (expression.kind == "Member" && !expression.children.empty())
                    { self(self, *expression.children[0], false); return; }
                    if (expression.kind == "Assignment" && expression.children.size() == 2)
                    { self(self, *expression.children[0], false); self(self, *expression.children[1], true); return; }
                    if ((expression.kind == "Prefix" || expression.kind == "Postfix") && (expression.value == "++" || expression.value == "--"))
                    { for (const auto& value : expression.children) self(self, *value, false); return; }
                    for (const auto& value : expression.children) self(self, *value, owning);
                };
                classifyArrayUse(classifyArrayUse, *closure.children[3], true);
                const auto mentions = [&](const auto& self, const Node& expression) -> bool
                {
                    if (expression.kind == "Name" && captures.contains(expression.value)) return true;
                    for (const auto& nested : expression.children) if (self(self, *nested)) return true;
                    return false;
                };
                const auto classify = [&](const auto& self, const Node& expression) -> void
                {
                    if (expression.kind == "Closure") return;
                    if (((expression.kind == "Assignment") || ((expression.kind == "Prefix" || expression.kind == "Postfix")
                        && (expression.value == "++" || expression.value == "--" || expression.value == "&mut")))
                        && !expression.children.empty() && mentions(mentions, *expression.children[0])) exclusive = true;
                    if (expression.kind == "Call" && !expression.children.empty())
                    {
                        const auto* callee = expression.children[0].get();
                        if (callee->kind == "GenericName" && !callee->children.empty()) callee = callee->children[0].get();
                        if (callee->kind == "Member" && !callee->children.empty() && mentions(mentions, *callee->children[0]))
                        {
                            const Node* receiver = nullptr;
                            const auto candidates = callCandidates(*expression.children[0], module, locals, owner, receiver);
                            const bool shared = !candidates.empty() && std::all_of(candidates.begin(), candidates.end(), [&](const auto& candidate) {
                                const auto* parameters = child(*candidate.node, "Parameters");
                                if (!parameters || parameters->children.empty() || parameters->children[0]->value != "self"
                                    || parameters->children[0]->children.empty()) return false;
                                const auto& mode = *parameters->children[0]->children[0];
                                return mode.kind == "ReferenceType" && mode.value != "mut";
                            });
                            if (!shared) exclusive = true;
                        }
                    }
                    for (const auto& nested : expression.children) self(self, *nested);
                };
                classify(classify, *closure.children[3]);
                const auto mode = consuming ? "FnOnce" : exclusive ? "FnMut" : "Fn";
                environment->children.push_back(node("CompilerClosure", mode, closure.range));
                auto method = node("Function", "__invoke", closure.range);
                auto returned = std::move(closure.children[2]);
                if (owner)
                { Bindings bindings; bindings["Self"] = use(declarations_.at(owner), module, "Type", closure.range); substitute(*returned, bindings, module, module); }
                method->children.push_back(std::move(returned));
                auto parameters = node("Parameters", "", closure.range);
                auto self = node("Parameter", "self", closure.range); auto receiver = node("Type", identity, closure.range);
                if (!consuming)
                { auto reference = node("ReferenceType", exclusive ? "mut" : "read", closure.range); reference->children.push_back(std::move(receiver)); receiver = std::move(reference); }
                self->children.push_back(std::move(receiver));
                // 소비 호출의 자체 환경만 수정 가능하게 하며 원래 바인딩의 권한은 바꾸지 않는다.
                // Permit mutation of the consumed invocation environment without changing caller binding permissions.
                if (consuming && exclusive) self->children.push_back(node("Modifier", "mut", closure.range));
                parameters->children.push_back(std::move(self));
                for (auto& parameter : closure.children[1]->children)
                {
                    if (owner)
                    { Bindings bindings; bindings["Self"] = use(declarations_.at(owner), module, "Type", closure.range); substitute(*parameter->children[0], bindings, module, module); }
                    parameters->children.push_back(std::move(parameter));
                }
                method->children.push_back(std::move(parameters));
                auto body = std::move(closure.children[3]);
                const auto rewrite = [&](const auto& rewriteSelf, Node& expression) -> void
                {
                    if (expression.kind == "Closure")
                    {
                        if (auto* nestedCaptures = child(expression, "Captures"))
                            for (auto& capture : nestedCaptures->children)
                                if (capture->children.size() > 1) rewriteSelf(rewriteSelf, *capture->children[1]);
                        return;
                    }
                    if (expression.kind == "Name")
                        if (const auto selected = captures.find(expression.value); selected != captures.end())
                        {
                            auto field = node("Member", selected->second.field, expression.range);
                            field->children.push_back(node("Name", "self", expression.range));
                            if (selected->second.mode != "value")
                            { auto dereference = node("Prefix", "*", expression.range); dereference->children.push_back(std::move(field)); field = std::move(dereference); }
                            expression = std::move(*field); rewritten_.insert(&expression); return;
                        }
                    for (auto& nested : expression.children) rewriteSelf(rewriteSelf, *nested);
                };
                rewrite(rewrite, *body); method->children.push_back(std::move(body));
                method->children.push_back(node("Modifier", "public", closure.range));
                environment->children.push_back(std::move(method));
                auto* address = environment.get(); graph_.modules[module]->ast->children.push_back(std::move(environment));
                index(*address, module, nullptr); ++generated_;
                closure = std::move(*construction); rewritten_.insert(closure.children[0].get());
                process(closure, module, locals, owner, depth + 1);
            }
            void rewriteClosureCall(Node& call, ModuleId module, const Locals& locals, Node* owner)
            {
                if (call.children.empty() || (call.children[0]->kind != "Name" && call.children[0]->kind != "Prefix" && call.children[0]->kind != "Group" && call.children[0]->kind != "Member")) return;
                auto type = infer(*call.children[0], module, locals, owner);
                if (type && type->kind == "ReferenceType") type = std::move(type->children[0]);
                const auto target = type ? declaration(*type, module) : Declaration{};
                if (!target.node || !child(*target.node, "CompilerClosure")) return;
                auto method = node("Member", "__invoke", call.children[0]->range);
                method->children.push_back(std::move(call.children[0])); call.children[0] = std::move(method);
                rewritten_.insert(call.children[0].get());
            }
            bool sliceResultCall(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                const auto& callee = *call.children[0];
                if (callee.kind != "Member" || callee.children.size() != 1 || callee.children[0]->kind != "Name"
                    || callee.children[0]->value != "Slice" || (callee.value != "slice" && callee.value != "slice_mut")) return false;
                if (!builtinMember(callee, "Slice", module, locals)) return false;
                if (call.children.size() != 4) { error(call, "Slice slicing requires a view, start, and end."); return true; }
                for (std::size_t index = 1; index < call.children.size(); ++index)
                {
                    if (!call.children[index]->value.empty() || call.children[index]->children.size() != 1)
                    { error(call, "Slice slicing requires three positional arguments."); return true; }
                    ExpectedGuard context(expected_); expected_ = nullptr;
                    process(*call.children[index], module, locals, owner, depth + 1);
                }
                auto view = infer(*call.children[1]->children[0], module, locals, owner, depth + 1);
                if (!view || (view->value != "Slice" && view->value != "MutSlice") || view->children.size() != 1)
                { error(call, "Cannot infer the Slice element type; provide an explicitly typed view."); return true; }
                const bool mutableView = callee.value == "slice_mut";
                if ((view->value == "MutSlice") != mutableView)
                { error(call, "Slice.slice takes Slice<T>; Slice.slice_mut takes MutSlice<T>."); return true; }
                std::vector<NodePtr> arguments; arguments.push_back(std::move(view));
                arguments.push_back(specializeBuiltin("BoundsError", {}, module, call, depth));
                if (!arguments.back()) return true;
                auto result = specializeBuiltin("Result", std::move(arguments), module, call, depth);
                if (!result) return true;
                std::vector<NodePtr> children; children.push_back(std::move(result));
                for (std::size_t index = 1; index < call.children.size(); ++index) children.push_back(std::move(call.children[index]->children[0]));
                call.kind = "CompilerSliceExpression"; call.value = mutableView ? "slice_mut" : "slice"; call.children = std::move(children);
                return true;
            }
            bool textResultCall(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                const auto& callee = *call.children[0];
                if (callee.kind != "Member" || callee.children.size() != 1 || callee.children[0]->kind != "Name" || callee.children[0]->value != "string") return false;
                if (!builtinMember(callee, "string", module, locals)) return false;
                const auto method = callee.value;
                if (method != "from_view" && method != "from_utf8" && method != "append" && method != "push_char" && method != "slice_bytes") return false;
                for (std::size_t index = 1; index < call.children.size(); ++index)
                {
                    ExpectedGuard context(expected_); expected_ = nullptr;
                    process(*call.children[index], module, locals, owner, depth + 1);
                }
                std::vector<NodePtr> arguments;
                arguments.push_back(node("Type", method == "slice_bytes" ? "string_view" : method == "append" || method == "push_char" ? "unit" : "string", call.range));
                arguments.push_back(specializeBuiltin("TextError", {}, module, call, depth));
                if (!arguments.back()) return true;
                auto result = specializeBuiltin("Result", std::move(arguments), module, call, depth);
                if (!result) return true;
                std::vector<NodePtr> children; children.push_back(std::move(result));
                for (std::size_t index = 1; index < call.children.size(); ++index)
                    if (!call.children[index]->children.empty()) children.push_back(std::move(call.children[index]->children[0]));
                call.kind = "CompilerTextExpression"; call.value = method; call.children = std::move(children);
                return true;
            }
            bool rangeInspectCall(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                if (call.children.empty()) return false;
                const auto* callee = call.children[0].get();
                const auto* arguments = callee->kind == "GenericName" ? child(*callee, "TypeArguments") : nullptr;
                if (arguments && originalReferences(*callee)) return false;
                if (callee->kind == "GenericName" && !callee->children.empty()) callee = callee->children[0].get();
                static const std::set<std::string> methods = {"start", "end", "step", "step_magnitude", "is_inclusive", "is_descending", "is_empty"};
                if (callee->kind != "Member" || !methods.contains(callee->value)
                    || !builtinMember(*callee, "Range", module, locals)) return false;
                if (arguments || call.children.size() != 2 || call.children[1]->children.size() != 1
                    || (!call.children[1]->value.empty() && call.children[1]->value != "range"))
                { error(call, "Range inspection requires one explicit range borrow and no type arguments; use the argument name range."); return true; }
                { ExpectedGuard context(expected_); expected_ = nullptr; process(*call.children[1]->children[0], module, locals, owner, depth + 1); }
                const auto input = infer(*call.children[1]->children[0], module, locals, owner);
                const auto* range = input && input->kind == "ReferenceType" && input->children.size() == 1 ? input->children[0].get() : nullptr;
                if (!range || range->kind != "Type" || range->value != "Range" || range->children.size() != 1)
                { error(call, "Range inspection requires an explicit borrow of a concrete integer Range<T> descriptor."); return true; }
                const auto element = key(*range->children[0], module);
                TypeContext scalars;
                const auto& scalar = scalars.get(scalars.builtin(element));
                if (scalar.kind != TypeKind::Integer)
                { error(call, "Range inspection requires an integer element type."); return true; }
                const auto method = callee->value;
                NodePtr destination;
                if (method == "start" || method == "end") destination = clone(*range->children[0]);
                else if (method == "step" || method == "step_magnitude")
                    destination = node("Type", element == "isize" || element == "usize" ? "usize" : "u" + std::to_string(scalar.bits), call.range);
                else destination = node("Type", "bool", call.range);
                auto replacement = node("CompilerRangeInspectExpression", "Range." + method, call.range);
                replacement->children.push_back(std::move(destination));
                replacement->children.push_back(std::move(call.children[1]->children[0]));
                call = std::move(*replacement); rewritten_.insert(&call); return true;
            }
            bool rangeResultCall(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                const auto& callee = *call.children[0];
                if (callee.kind != "Member" || callee.children.empty() || callee.children[0]->kind != "Name" || callee.children[0]->value != "Range"
                    || (callee.value != "with_step" && callee.value != "len")) return false;
                if (child(call, "CompilerRangeResult")) return true;
                if (!builtinMember(callee, "Range", module, locals)) return false;
                const auto count = callee.value == "len" ? 2U : 3U;
                if (call.children.size() != count || call.children[1]->children.empty()) return false;
                ExpectedGuard resultContext(expected_);
                NodePtr expectedDescriptor;
                if (callee.value == "with_step" && expected_)
                {
                    auto arguments = carrierArguments(*expected_, module, "Result");
                    if (!arguments.empty() && arguments[0]->value == "Range")
                    {
                        // Result의 성공 타입도 범위 리터럴과 보폭의 숫자 문맥을 제공한다.
                        // A Result success type also supplies the numeric context for its Range and step.
                        expectedDescriptor = std::move(arguments[0]); expected_ = expectedDescriptor.get();
                    }
                }
                auto descriptor = infer(*call.children[1]->children[0], module, locals, owner);
                if (descriptor && descriptor->kind == "ReferenceType") descriptor = std::move(descriptor->children[0]);
                if (!descriptor || descriptor->value != "Range" || descriptor->children.size() != 1)
                { error(call, "Range operation requires a concrete Range<T> descriptor."); return true; }
                for (std::size_t i = 1; i < call.children.size(); ++i)
                {
                    ExpectedGuard context(expected_); expected_ = i == 1 && callee.value == "with_step" ? descriptor.get() : nullptr;
                    process(*call.children[i], module, locals, owner, depth + 1);
                }
                auto errorType = specializeBuiltin("RangeError", {}, module, call, depth + 1);
                if (!errorType) return true;
                std::vector<NodePtr> arguments;
                arguments.push_back(callee.value == "len" ? node("Type", "usize", call.range) : std::move(descriptor));
                arguments.push_back(std::move(errorType));
                auto resultType = specializeBuiltin("Result", std::move(arguments), module, call, depth + 1);
                if (!resultType) return true;
                auto marker = node("CompilerRangeResult", "", call.range); marker->children.push_back(std::move(resultType)); call.children.push_back(std::move(marker));
                return true;
            }
            bool collectionCall(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                const auto& callee = *call.children[0];
                static const std::set<std::string> methods = {"push", "reserve", "pop", "truncate", "clear", "remove", "swap_remove", "insert"};
                if (callee.kind != "Member" || callee.children.empty() || callee.children[0]->kind != "Name"
                    || callee.children[0]->value != "Vector" || !methods.contains(callee.value)) return false;
                if (child(call, "CompilerCollectionResult")) return true;
                if (!builtinMember(callee, "Vector", module, locals)) return false;
                const bool input = callee.value == "push" || callee.value == "insert";
                const auto count = callee.value == "pop" || callee.value == "clear" ? 2u : callee.value == "insert" ? 4u : 3u;
                if (call.children.size() != count || call.children[1]->children.empty())
                { error(call, "Vector." + callee.value + " has an invalid argument count."); return true; }
                auto receiver = infer(*call.children[1]->children[0], module, locals, owner);
                if (!receiver || receiver->kind != "ReferenceType" || receiver->children[0]->value != "Vector" || receiver->children[0]->children.size() != 1)
                { error(call, "Vector mutation requires an explicit concrete Vector borrow."); return true; }
                auto element = clone(*receiver->children[0]->children[0]);
                for (std::size_t i = 1; i < call.children.size(); ++i)
                {
                    ExpectedGuard context(expected_); expected_ = input && i == count - 1 ? element.get() : nullptr;
                    process(*call.children[i], module, locals, owner, depth + 1);
                }
                NodePtr resultType;
                if (callee.value == "clear" || callee.value == "truncate") resultType = node("Type", "unit", call.range);
                else if (callee.value == "pop")
                {
                    std::vector<NodePtr> values; values.push_back(std::move(element));
                    resultType = specializeBuiltin("Optional", std::move(values), module, call, depth + 1);
                }
                else
                {
                    std::vector<NodePtr> errors;
                    if (input) errors.push_back(clone(*element));
                    auto errorType = specializeBuiltin(input ? "InputError" : callee.value == "reserve" ? "CollectionError" : "BoundsError", std::move(errors), module, call, depth + 1);
                    if (!errorType) return true;
                    std::vector<NodePtr> arguments;
                    arguments.push_back(callee.value == "remove" || callee.value == "swap_remove" ? std::move(element) : node("Type", "unit", call.range));
                    arguments.push_back(std::move(errorType));
                    resultType = specializeBuiltin("Result", std::move(arguments), module, call, depth + 1);
                }
                if (!resultType) return true;
                auto marker = node("CompilerCollectionResult", "", call.range); marker->children.push_back(std::move(resultType)); call.children.push_back(std::move(marker));
                return true;
            }
            bool containsDynamic(const Node& type)
            {
                if (type.kind == "Type" && type.value == "Vector") return true;
                for (const auto& nested : type.children) if (containsDynamic(*nested)) return true;
                return false;
            }
            std::size_t rankConstant(const Node& expression, std::size_t depth = 0)
            {
                if (depth > 32) return 0;
                TypeContext constants;
                const std::unordered_map<const Node*, TypeId> nodeTypes;
                const std::unordered_map<SymbolId, TypeId> symbolTypes;
                const auto constantNames = constantNamesFor(expression);
                const auto evaluated = ConstEvaluator(constantNames, constants, nodeTypes, symbolTypes).evaluate(expression, constants.builtin("usize"));
                if (evaluated.status == ConstStatus::Constant && evaluated.value.kind == ConstantKind::Integer
                    && !evaluated.value.negative && !evaluated.value.highMagnitude && evaluated.value.magnitude > 0 && evaluated.value.magnitude <= 64) return evaluated.value.magnitude;
                if (expression.kind == "Literal" || expression.kind == "ConstantTypeArgument")
                {
                    auto value = expression.value; std::erase(value, '_');
                    try { std::size_t used = 0; const auto result = std::stoull(value, &used, 0); return used == value.size() && result <= 64 ? result : 0; }
                    catch (...) { return 0; }
                }
                if (expression.kind == "Name")
                {
                    const auto* refs = originalReferences(expression);
                    if (refs && refs->size() == 1)
                    {
                        const auto* source = names_.symbols[refs->front()].declaration;
                        if (source && modifier(*source, "const"))
                            for (std::size_t i = 1; i < source->children.size(); ++i)
                                if (source->children[i]->kind != "Modifier" && source->children[i]->kind != "ArraySuffixes") return rankConstant(*source->children[i], depth + 1);
                    }
                }
                if (expression.kind == "Group") return rankConstant(*expression.children[0], depth + 1);
                return 0;
            }
            void canonicalizeDynamic(Node& item)
            {
                auto* suffixes = child(item, "ArraySuffixes");
                if (!suffixes || item.children.empty() || item.kind == "VariadicParameter") return;
                bool dynamic = false;
                for (const auto& suffix : suffixes->children) dynamic = dynamic || suffix->kind == "RankSuffix" || suffix->children.empty();
                if (!dynamic) return;
                auto type = clone(*item.children[0]);
                for (auto suffix = suffixes->children.rbegin(); suffix != suffixes->children.rend(); ++suffix)
                {
                    std::size_t count = 1;
                    if ((*suffix)->kind == "RankSuffix")
                    {
                        count = rankConstant(*(*suffix)->children[0]);
                        if (!count) { error(**suffix, "Array rank must be a positive compile-time integer constant no greater than 64."); return; }
                    }
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        const bool fixed = (*suffix)->kind == "ArraySuffix" && !(*suffix)->children.empty();
                        auto outer = node("Type", fixed ? "Array" : "Vector", (*suffix)->range);
                        outer->children.push_back(std::move(type));
                        if (fixed)
                        {
                            auto extent = clone(*(*suffix)->children[0]);
                            if (extent->kind == "Literal") extent->kind = "ConstantTypeArgument";
                            else if (extent->kind == "Name") extent->kind = "Type";
                            outer->children.push_back(std::move(extent));
                        }
                        type = std::move(outer);
                    }
                }
                item.children[0] = std::move(type);
                std::erase_if(item.children, [](const auto& value) { return value->kind == "ArraySuffixes"; });
            }
            void dynamicElements(Node& literal, const Node& collection, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                if (collection.children.empty()) { error(literal, "Dynamic array literal requires a complete collection context."); return; }
                const auto& element = *collection.children[0];
                for (auto& value : literal.children)
                {
                    if (value->kind == "CompilerArrayResult") continue;
                    if (value->kind == "ArrayLiteral") dynamicElements(*value, element, module, locals, owner, depth + 1);
                    else { ExpectedGuard context(expected_); expected_ = &element; process(*value, module, locals, owner, depth + 1); }
                }
            }
            bool memoryCall(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                if (call.children.empty()) return false;
                const auto& callee = *call.children[0];
                const auto target = declaration(callee.kind == "GenericName" ? *callee.children[0] : callee, module);
                if (!target.node || !child(*target.node, "CompilerMemory") || !child(*target.node, "CompilerIntrinsic")
                    || graph_.modules[target.module]->package != "$memory") return false;
                const bool swap = target.node->value == "swap";
                if (!swap && target.node->value != "replace") return false;
                if (call.children.size() != 3) { error(call, "memory operation requires exactly two arguments."); return true; }
                const std::array<std::string, 2> names = swap ? std::array<std::string, 2>{"left", "right"}
                    : std::array<std::string, 2>{"target", "replacement"};
                std::array<std::size_t, 2> roles{}; std::array<bool, 2> supplied{};
                bool named = false;
                for (std::size_t index = 1; index < call.children.size(); ++index)
                {
                    const auto& argument = *call.children[index];
                    std::size_t role = index - 1;
                    if (!argument.value.empty())
                    {
                        named = true;
                        if (argument.value == names[0]) role = 0;
                        else if (argument.value == names[1]) role = 1;
                        else { error(argument, "Unknown memory operation argument name."); return true; }
                    }
                    else if (named) { error(argument, "Positional arguments must precede named arguments."); return true; }
                    if (argument.children.size() != 1 || supplied[role])
                    { error(argument, "Missing or duplicate memory operation argument."); return true; }
                    supplied[role] = true; roles[index - 1] = role;
                }
                NodePtr element;
                if (callee.kind == "GenericName")
                {
                    const auto arguments = typeArguments(callee);
                    if (arguments.size() != 1) { error(callee, "memory operation accepts exactly one explicit type argument."); return true; }
                    element = clone(*arguments[0]); process(*element, module, locals, owner, depth + 1);
                }
                for (std::size_t index = 1; index < call.children.size(); ++index)
                    process(*call.children[index]->children[0], module, locals, owner, depth + 1);
                if (!element)
                    for (std::size_t index = 1; index < call.children.size(); ++index)
                        if (roles[index - 1] == 0)
                        {
                            auto reference = infer(*call.children[index]->children[0], module, locals, owner);
                            if (reference && reference->kind == "ReferenceType" && reference->value == "mut" && reference->children.size() == 1)
                                element = std::move(reference->children[0]);
                        }
                if (!element) { error(call, "memory operation requires an initialized exclusive &mut value with a known element type."); return true; }
                std::vector<NodePtr> children; children.push_back(std::move(element));
                for (std::size_t index = 1; index < call.children.size(); ++index)
                {
                    auto argument = node("CompilerMemoryArgument", names[roles[index - 1]], call.children[index]->range);
                    argument->children.push_back(std::move(call.children[index]->children[0])); children.push_back(std::move(argument));
                }
                call.kind = "CompilerMemoryExpression"; call.value = target.node->value; call.children = std::move(children);
                return true;
            }
            bool systemCall(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                const Node* callee = call.children[0].get();
                const Node* types = callee->kind == "GenericName" ? child(*callee, "TypeArguments") : nullptr;
                if (callee->kind == "GenericName") callee = callee->children[0].get();
                const auto target = declaration(*callee, module);
                const auto* marker = target.node ? child(*target.node, "CompilerSystem") : nullptr;
                if (!marker) return false;
                const auto method = marker->value;
                const auto count = call.children.size() - 1;
                if ((types && method != "input") || (types && types->children.size() != 1))
                { error(call, "Only system.input accepts one explicit scalar type argument."); return true; }
                const auto library = [&](std::string_view name) -> NodePtr
                {
                    for (const auto& [address, candidate] : declarations_)
                    {
                        (void)address;
                        if (candidate.node->value == name && graph_.modules[candidate.module]->name == "error"
                            && graph_.modules[candidate.module]->package == "$error") return use(candidate, module, "Type", call.range);
                    }
                    error(call, "Missing canonical error library type " + std::string(name) + "."); return {};
                };
                NodePtr valueType = node("Type", "unit", call.range);
                if (method == "input")
                {
                    if (count) { error(call, "system.input accepts no value arguments."); return true; }
                    valueType = types ? portableType(*types->children[0], module, module) : node("Type", "string", call.range);
                    TypeContext scalar;
                    const auto& type = scalar.get(scalar.builtin(valueType->value));
                    if (!(type.kind == TypeKind::String || type.kind == TypeKind::Integer || type.kind == TypeKind::Bool
                        || type.kind == TypeKind::Char || type.kind == TypeKind::Byte
                        || (type.kind == TypeKind::Float && (type.bits == 32 || type.bits == 64))))
                    { error(call, "system.input supports string, integer types, f32/f64, bool, char, and byte."); return true; }
                    std::vector<NodePtr> args; args.push_back(std::move(valueType));
                    valueType = specializeBuiltin("Optional", std::move(args), module, call, depth + 1);
                }
                else if ((method == "flush" && count != 0) || ((method == "print" || method == "println") && count != 1)
                    || (method == "printf" && count == 0) || (method == "log" && (count < 2 || count > 3)))
                { error(call, "Incorrect argument count for system." + method + "."); return true; }
                auto errorType = library(method == "input" ? "ReadError" : "IoError");
                if (!errorType || !valueType) return true;
                std::vector<NodePtr> resultArgs; resultArgs.push_back(std::move(valueType)); resultArgs.push_back(std::move(errorType));
                auto result = specializeBuiltin("Result", std::move(resultArgs), module, call, depth + 1);
                if (!result) return true;
                for (std::size_t i = 1; i < call.children.size(); ++i)
                {
                    const auto& argument = *call.children[i];
                    if (argument.children.size() != 1 || (!argument.value.empty()
                        && method != "log" && !((method == "print" || method == "println") && argument.value == "value")))
                    { error(call, "system operations require positional arguments (print/println also accept value=)."); return true; }
                    ExpectedGuard context(expected_); expected_ = nullptr;
                    process(*call.children[i]->children[0], module, locals, owner, depth + 1);
                }
                std::vector<NodePtr> children; children.push_back(std::move(result));
                const auto text = [&](const Node& expression) -> bool
                {
                    auto type = infer(expression, module, locals, owner);
                    if (type && type->kind == "ReferenceType") type = std::move(type->children[0]);
                    return type && (type->value == "string" || type->value == "string_view");
                };
                const auto composed = [&](const auto& self, const Node& expression) -> bool
                {
                    if (expression.kind == "Group") return self(self, *expression.children[0]);
                    if (expression.kind == "Binary" && expression.value == "+")
                        return self(self, *expression.children[0]) || self(self, *expression.children[1]);
                    return text(expression);
                };
                const auto append = [&](const auto& self, NodePtr expression) -> void
                {
                    if (expression->kind == "Group" && composed(composed, *expression))
                    { self(self, std::move(expression->children[0])); return; }
                    if (expression->kind == "Binary" && expression->value == "+" && composed(composed, *expression))
                    { self(self, std::move(expression->children[0])); self(self, std::move(expression->children[1])); return; }
                    children.push_back(std::move(expression));
                };
                if (method == "print" || method == "println") append(append, std::move(call.children[1]->children[0]));
                else if (method == "log")
                {
                    const std::array<std::string, 3> names = {"level", "message", "code"};
                    std::array<bool, 3> assigned{};
                    std::vector<std::size_t> selectedArguments;
                    std::size_t positional = 0; bool named = false;
                    const auto wrap = [&](std::size_t index, NodePtr expression)
                    {
                        auto part = node("CompilerSystemArgument", names[index], expression->range);
                        part->children.push_back(std::move(expression)); children.push_back(std::move(part));
                    };
                    for (std::size_t i = 1; i < call.children.size(); ++i)
                    {
                        const auto& argument = *call.children[i]; std::size_t selected = positional;
                        if (argument.value.empty())
                        { if (named) { error(call, "Positional log arguments cannot follow named arguments."); return true; } ++positional; }
                        else
                        { named = true; selected = 3; for (std::size_t p = 0; p < names.size(); ++p) if (names[p] == argument.value) selected = p; }
                        if (selected >= 3 || assigned[selected]) { error(call, "Unknown or duplicated system.log argument."); return true; }
                        assigned[selected] = true;
                        selectedArguments.push_back(selected);
                    }
                    if (!assigned[0] || !assigned[1]) { error(call, "system.log requires level and message."); return true; }
                    // 잘못된 호출에도 원래 AST가 완전하게 남도록 검증 후에만 소유 노드를 이동한다.
                    // Move owned syntax only after validation so rejected calls retain a complete AST.
                    for (std::size_t i = 1; i < call.children.size(); ++i)
                    {
                        const auto selected = selectedArguments[i - 1];
                        if (selected == 1)
                        {
                            const auto begin = children.size(); append(append, std::move(call.children[i]->children[0]));
                            for (auto p = begin; p < children.size(); ++p)
                            {
                                auto part = node("CompilerSystemArgument", "message", children[p]->range);
                                part->children.push_back(std::move(children[p])); children[p] = std::move(part);
                            }
                        }
                        else wrap(selected, std::move(call.children[i]->children[0]));
                    }
                    if (!assigned[2]) wrap(2, node("Literal", "\"\"", call.range));
                }
                else if (method == "printf")
                {
                    auto& format = *call.children[1]->children[0];
                    TypeContext scalar; const std::unordered_map<const Node*, TypeId> nodeTypes;
                    const std::unordered_map<SymbolId, TypeId> symbolTypes;
                    const auto constantNames = constantNamesFor(format);
                    const auto evaluated = ConstEvaluator(constantNames, scalar, nodeTypes, symbolTypes).evaluate(format, scalar.builtin("string_view"));
                    if (evaluated.status != ConstStatus::Constant || evaluated.value.kind != ConstantKind::String)
                    { error(format, "system.printf requires a compile-time string format."); return true; }
                    const auto& spelling = evaluated.value.text;
                    std::string bytes;
                    if (const auto raw = rawStringContents(spelling)) bytes.assign(*raw);
                    else for (std::size_t i = 1; i + 1 < spelling.size(); ++i)
                    {
                        auto c = spelling[i];
                        if (c == '\\')
                        {
                            c = spelling[++i];
                            if (c == 'n') c = '\n'; else if (c == 'r') c = '\r'; else if (c == 't') c = '\t'; else if (c == '0') c = '\0';
                            else if (c == 'u')
                            {
                                std::uint32_t value = 0; i += 2;
                                while (spelling[i] != '}') { const auto d = spelling[i++]; value = value * 16 + (d <= '9' ? d - '0' : d <= 'F' ? d - 'A' + 10 : d - 'a' + 10); }
                                if (value < 0x80) bytes += char(value);
                                else if (value < 0x800) { bytes += char(0xc0 | (value >> 6)); bytes += char(0x80 | (value & 63)); }
                                else if (value < 0x10000) { bytes += char(0xe0 | (value >> 12)); bytes += char(0x80 | ((value >> 6) & 63)); bytes += char(0x80 | (value & 63)); }
                                else { bytes += char(0xf0 | (value >> 18)); bytes += char(0x80 | ((value >> 12) & 63)); bytes += char(0x80 | ((value >> 6) & 63)); bytes += char(0x80 | (value & 63)); }
                                continue;
                            }
                        }
                        bytes += c;
                    }
                    std::string pending;
                    std::size_t conversions = 0;
                    for (std::size_t i = 0; i < bytes.size(); ++i)
                    {
                        if (bytes[i] != '%') continue;
                        if (++i == bytes.size()) { error(format, "Incomplete system.printf conversion."); return true; }
                        if (bytes[i] == '%') continue;
                        if (std::string_view("sdufgcb").find(bytes[i]) == std::string_view::npos)
                        { error(format, "Unsupported printf conversion, width, or precision."); return true; }
                        ++conversions;
                    }
                    if (conversions + 2 < call.children.size()) { error(call, "Unexpected system.printf argument."); return true; }
                    if (conversions + 2 > call.children.size()) { error(call, "Missing system.printf argument."); return true; }
                    const auto flush = [&]()
                    {
                        if (pending.empty()) return;
                        std::string literal = "\"";
                        for (const auto c : pending)
                            if (c == '\\' || c == '"') { literal += '\\'; literal += c; }
                            else if (c == '\n') literal += "\\n"; else if (c == '\r') literal += "\\r";
                            else if (c == '\t') literal += "\\t"; else if (c == '\0') literal += "\\0"; else literal += c;
                        literal += '"'; children.push_back(node("Literal", std::move(literal), format.range)); pending.clear();
                    };
                    std::size_t argument = 2;
                    for (std::size_t i = 0; i < bytes.size(); ++i)
                    {
                        if (bytes[i] != '%') { pending += bytes[i]; continue; }
                        if (++i == bytes.size()) { error(format, "Incomplete system.printf conversion."); return true; }
                        if (bytes[i] == '%') { pending += '%'; continue; }
                        if (std::string_view("sdufgcb").find(bytes[i]) == std::string_view::npos)
                        { error(format, "Unsupported printf conversion, width, or precision."); return true; }
                        if (argument >= call.children.size()) { error(call, "Missing system.printf argument."); return true; }
                        flush();
                        auto part = node("CompilerFormatValue", std::string(1, bytes[i]), call.children[argument]->range);
                        part->children.push_back(std::move(call.children[argument++]->children[0])); children.push_back(std::move(part));
                    }
                    flush();
                    if (argument != call.children.size()) { error(call, "Unexpected system.printf argument."); return true; }
                }
                call.kind = "CompilerTextExpression"; call.value = "system." + method; call.children = std::move(children);
                return true;
            }
            bool parseCall(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                const auto& callee = *call.children[0];
                if (callee.kind != "Member" || callee.value != "parse" || callee.children.size() != 1) return false;
                const auto& receiver = *callee.children[0];
                const auto aliased = declaration(receiver, module);
                NodePtr destination;
                if (aliased.node && aliased.node->kind == "TypeAlias")
                    destination = portableType(*aliased.node->children[0], aliased.module, module);
                else
                {
                    TypeContext scalar;
                    if (scalar.builtin(receiver.value) == scalar.error() || !builtinMember(callee, receiver.value, module, locals)) return false;
                    destination = node("Type", receiver.value, receiver.range);
                }
                TypeContext scalars;
                const auto& target = scalars.get(scalars.builtin(destination->value));
                if (aliased.node && target.kind == TypeKind::Error) return false;
                const bool supported = target.kind == TypeKind::Integer || target.kind == TypeKind::Bool
                    || target.kind == TypeKind::Char || target.kind == TypeKind::Byte
                    || (target.kind == TypeKind::Float && (target.bits == 32 || target.bits == 64));
                if (!supported)
                { error(call, "Type.parse supports integer types, f32/f64, bool, char, and byte only."); return true; }
                if (call.children.size() != 2 || call.children[1]->children.size() != 1
                    || (!call.children[1]->value.empty() && call.children[1]->value != "text"))
                { error(call, "Type.parse requires one text argument."); return true; }
                { ExpectedGuard context(expected_); expected_ = nullptr; process(*call.children[1]->children[0], module, locals, owner, depth + 1); }
                auto errors = specializeBuiltin("ParseError", {}, module, call, depth + 1);
                if (!errors) return true;
                std::vector<NodePtr> arguments; arguments.push_back(std::move(destination)); arguments.push_back(std::move(errors));
                auto result = specializeBuiltin("Result", std::move(arguments), module, call, depth + 1);
                if (!result) return true;
                std::vector<NodePtr> children; children.push_back(std::move(result)); children.push_back(std::move(call.children[1]->children[0]));
                call.kind = "CompilerTextExpression"; call.value = "parse"; call.children = std::move(children);
                return true;
            }
            bool charScalarCall(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                if (call.children.empty()) return false;
                const auto* callee = call.children[0].get();
                const auto* arguments = callee->kind == "GenericName" ? child(*callee, "TypeArguments") : nullptr;
                if (arguments && originalReferences(*callee)) return false;
                if (callee->kind == "GenericName" && !callee->children.empty()) callee = callee->children[0].get();
                if (callee->kind != "Member" || callee->children.size() != 1
                    || (callee->value != "from_u32" && callee->value != "to_u32")
                    || !builtinMember(*callee, "Char", module, locals)) return false;
                if (arguments || call.children.size() != 2 || call.children[1]->children.size() != 1)
                { error(call, "Char scalar conversion requires one value and no type arguments."); return true; }
                if (!call.children[1]->value.empty() && call.children[1]->value != "value")
                { error(call, "Unknown Char scalar conversion argument name; use value."); return true; }
                { ExpectedGuard context(expected_); expected_ = nullptr; process(*call.children[1]->children[0], module, locals, owner, depth + 1); }
                const bool from = callee->value == "from_u32";
                NodePtr output;
                if (from)
                {
                    auto errors = specializeBuiltin("CharError", {}, module, call, depth + 1);
                    if (!errors) return true;
                    std::vector<NodePtr> parameters;
                    parameters.push_back(node("Type", "char", call.range)); parameters.push_back(std::move(errors));
                    output = specializeBuiltin("Result", std::move(parameters), module, call, depth + 1);
                    if (!output) return true;
                }
                else output = node("Type", "u32", call.range);
                auto replacement = node("CompilerCharScalarExpression", from ? "Char.from_u32" : "Char.to_u32", call.range);
                replacement->children.push_back(std::move(output));
                replacement->children.push_back(std::move(call.children[1]->children[0]));
                call = std::move(*replacement); rewritten_.insert(&call); return true;
            }
            bool floatBitsCall(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                if (call.children.empty()) return false;
                const auto* callee = call.children[0].get();
                const auto* arguments = callee->kind == "GenericName" ? child(*callee, "TypeArguments") : nullptr;
                // 명시적 타입 인자가 있으면 실제 함수의 해석 결과는 바깥 GenericName에 저장된다.
                // With explicit type arguments, the full resolved function binding lives on the outer GenericName.
                if (arguments && originalReferences(*callee)) return false;
                if (callee->kind == "GenericName" && !callee->children.empty()) callee = callee->children[0].get();
                if (callee->kind != "Member" || callee->children.size() != 1
                    || (callee->value != "to_bits" && callee->value != "from_bits")
                    || !builtinMember(*callee, "Float", module, locals)) return false;
                const bool from = callee->value == "from_bits";
                if (call.children.size() != 2 || call.children[1]->children.size() != 1
                    || (from ? !arguments || arguments->children.size() != 1 : arguments != nullptr))
                { error(call, "Float bit conversion requires one value; only from_bits accepts exactly one explicit float destination type."); return true; }
                if (!call.children[1]->value.empty() && call.children[1]->value != "value")
                { error(call, "Unknown Float bit conversion argument name; use value."); return true; }
                { ExpectedGuard context(expected_); expected_ = nullptr; process(*call.children[1]->children[0], module, locals, owner, depth + 1); }
                auto input = infer(*call.children[1]->children[0], module, locals, owner);
                if (!input) { error(call, "Cannot determine the concrete Float bit input type."); return true; }
                auto destination = from ? clone(*arguments->children[0]) : clone(*input);
                process(*destination, module, locals, owner, depth + 1);
                TypeContext scalars;
                const auto& format = scalars.get(scalars.builtin(key(*destination, module)));
                if (format.kind != TypeKind::Float || (format.bits != 16 && format.bits != 32 && format.bits != 64))
                { error(call, "Float bit conversion supports f16, bf16, f32, and f64 formats only."); return true; }
                const auto word = "u" + std::to_string(format.bits);
                if (from && key(*input, module) != word)
                { error(call, "Float.from_bits requires the exact same-width unsigned word type " + word + "."); return true; }
                if (!from) destination = node("Type", word, call.range);
                auto replacement = node("CompilerFloatBitsExpression", from ? "Float.from_bits" : "Float.to_bits", call.range);
                replacement->children.push_back(std::move(destination));
                replacement->children.push_back(std::move(call.children[1]->children[0]));
                call = std::move(*replacement); rewritten_.insert(&call); return true;
            }
            bool numericCall(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                if (call.children.empty()) return false;
                const Node* callee = call.children[0].get();
                const Node* arguments = callee->kind == "GenericName" ? child(*callee, "TypeArguments") : nullptr;
                if (callee->kind == "GenericName") callee = callee->children[0].get();
                if (callee->kind != "Member" || callee->children.size() != 1 || callee->children[0]->kind != "Name") return false;
                const auto& name = callee->children[0]->value;
                if (name != "Integer" && name != "Float") return false;
                if (!builtinMember(*callee, name, module, locals)) return false;
                const auto method = callee->value;
                if (method != "convert" && !(name == "Float" && (method == "convert_exact" || method == "to_integer_exact"))) return false;
                if (!arguments || arguments->children.size() != 1 || call.children.size() != 2 || call.children[1]->children.size() != 1)
                { error(call, "Numeric conversion requires one destination type argument and one value."); return true; }
                if (!call.children[1]->value.empty() && call.children[1]->value != "value")
                { error(call, "Unknown numeric conversion argument name."); return true; }
                auto destination = clone(*arguments->children[0]);
                { ExpectedGuard context(expected_); expected_ = nullptr; process(*call.children[1]->children[0], module, locals, owner, depth + 1); }
                auto errors = specializeBuiltin(name == "Integer" ? "IntegerError" : "FloatError", {}, module, call, depth + 1);
                if (!errors) return true;
                std::vector<NodePtr> parameters; parameters.push_back(std::move(destination)); parameters.push_back(std::move(errors));
                auto result = specializeBuiltin("Result", std::move(parameters), module, call, depth + 1);
                if (!result) return true;
                auto replacement = node("CompilerNumericExpression", name + "." + method, call.range);
                replacement->children.push_back(std::move(result));
                replacement->children.push_back(std::move(call.children[1]->children[0]));
                call = std::move(*replacement); rewritten_.insert(&call); return true;
            }
            bool syncCall(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                const Node* callee = call.children.empty() ? nullptr : call.children[0].get();
                const Node* explicitTypes = nullptr;
                if (callee && callee->kind == "GenericName") { explicitTypes = child(*callee, "TypeArguments"); callee = callee->children[0].get(); }
                if (!callee || callee->kind != "Member" || callee->children.size() != 1) return false;
                const auto declared = declaration(*callee->children[0], module);
                if (!declared.node) return false;
                std::string prefix;
                for (const auto& [marker, name] : std::vector<std::pair<std::string, std::string>>{{"CompilerMutex","mutex"},{"CompilerRwLock","rwlock"},{"CompilerMutexGuard","mutex_guard"},{"CompilerReadGuard","read_guard"},{"CompilerWriteGuard","write_guard"},{"CompilerPoisoned","poisoned"}})
                    if (child(*declared.node, marker)) { prefix = name; break; }
                if (prefix.empty()) return false;
                const auto operationName = prefix + "." + callee->value;
                const auto* operation = syncOperation(operationName);
                if (!operation) return false;
                const bool constructing = operation->result == SyncResult::Owner;
                if (call.children.size() != operation->operands + 1 || (explicitTypes && (!constructing || explicitTypes->children.size() != 1)))
                { error(call, "Synchronization operation has an invalid argument count or explicit payload type."); return true; }
                NodePtr payload;
                const auto receiverTypes = typeArguments(*callee->children[0]);
                if (explicitTypes) payload = clone(*explicitTypes->children[0]);
                else if (receiverTypes.size() == 1) payload = clone(*receiverTypes[0]);
                else if (!generic(*declared.node))
                    for (const auto& field : declared.node->children)
                        if (field->kind == "Variable" && !field->children.empty()) payload = portableType(*field->children[0], declared.module, module);
                if (!payload && constructing && expected_)
                {
                    const auto context = declaration(*expected_, module);
                    if (context.node && (child(*context.node, "CompilerMutex") || child(*context.node, "CompilerRwLock")))
                        for (const auto& field : context.node->children)
                            if (field->kind == "Variable" && !field->children.empty()) payload = portableType(*field->children[0], context.module, module);
                }
                for (std::size_t i = 1; i < call.children.size(); ++i)
                {
                    if (call.children[i]->children.size() != 1 || !call.children[i]->value.empty())
                    { error(call, "Synchronization operations currently require positional arguments."); return true; }
                    ExpectedGuard context(expected_); expected_ = constructing && i == 1 ? payload.get() : nullptr;
                    process(*call.children[i]->children[0], module, locals, owner, depth + 1);
                }
                if (constructing && !payload) payload = infer(*call.children[1]->children[0], module, locals, owner);
                if (!constructing)
                {
                    auto input = infer(*call.children[1]->children[0], module, locals, owner);
                    if (input && input->kind == "ReferenceType") input = std::move(input->children[0]);
                    const auto actual = input ? declaration(*input, module) : Declaration{};
                    if (actual.node)
                        for (const auto& field : actual.node->children)
                            if (field->kind == "Variable" && !field->children.empty())
                            {
                                auto actualPayload = portableType(*field->children[0], actual.module, module);
                                if (payload && key(*payload, module) != key(*actualPayload, module))
                                { error(call, "Synchronization namespace payload does not match the owner or guard."); return true; }
                                payload = std::move(actualPayload);
                            }
                }
                if (!payload) { error(call, "Cannot infer synchronization payload type."); return true; }
                const auto standardType = [&](const std::string& name, std::vector<NodePtr> arguments) -> NodePtr
                {
                    for (const auto& [address, candidate] : declarations_)
                    {
                        (void)address;
                        // 같은 철자의 enum 항목이나 구체화 복제본을 타입 선언으로 선택하지 않는다.
                        // Do not select same-spelled enum variants or specialized clones as type declarations.
                        if (candidate.owner || !typeDeclaration(*candidate.node) || !originalNodes_.contains(candidate.node)
                            || candidate.node->value != name || graph_.modules[candidate.module]->package != "$sync") continue;
                        const auto* parameters = child(*candidate.node, "GenericParameters");
                        if ((parameters ? parameters->children.size() : 0) != arguments.size()) continue;
                        if (arguments.empty()) return use(candidate, module, "Type", call.range);
                        std::vector<const Node*> borrowed; for (const auto& value : arguments) borrowed.push_back(value.get());
                        const auto concrete = specialize(candidate, borrowed, module, call, depth + 1);
                        return concrete.node ? use(concrete, module, "Type", call.range) : NodePtr{};
                    }
                    return {};
                };
                const auto withPayload = [&](const std::string& name) -> NodePtr
                { std::vector<NodePtr> args; args.push_back(clone(*payload)); return standardType(name, std::move(args)); };
                const auto kindName = [](SyncKind kind) -> std::string
                {
                    switch (kind) {
                        case SyncKind::Mutex: return "Mutex";
                        case SyncKind::RwLock: return "RwLock";
                        case SyncKind::MutexGuard: return "MutexGuard";
                        case SyncKind::ReadGuard: return "RwLockReadGuard";
                        case SyncKind::WriteGuard: return "RwLockWriteGuard";
                        case SyncKind::Poisoned: return "Poisoned";
                    }
                    return {};
                };
                NodePtr output, failure;
                switch (operation->result)
                {
                    case SyncResult::Owner: output = withPayload(kindName(operation->kind)); break;
                    case SyncResult::Acquire:
                    case SyncResult::OptionalAcquire:
                    {
                        output = withPayload(kindName(operation->guard)); if (!output) return true;
                        std::vector<NodePtr> args; args.push_back(clone(*output)); failure = standardType("LockError", std::move(args));
                        if (operation->result == SyncResult::OptionalAcquire)
                        { std::vector<NodePtr> values; values.push_back(std::move(output)); output = specializeBuiltin("Optional", std::move(values), module, call, depth + 1); }
                        break;
                    }
                    case SyncResult::Boolean: output = node("Type", "bool", call.range); break;
                    case SyncResult::Policy: output = standardType("PoisonPolicy", {}); break;
                    case SyncResult::UniqueReference:
                    case SyncResult::IntoInner:
                    {
                        output = clone(*payload);
                        if (operation->result == SyncResult::UniqueReference)
                        { auto reference = node("ReferenceType", "mut", call.range); reference->children.push_back(std::move(output)); output = std::move(reference); }
                        std::vector<NodePtr> values; values.push_back(clone(*output));
                        failure = standardType(operation->result == SyncResult::UniqueReference ? "LockError" : "Poisoned", std::move(values)); break;
                    }
                    case SyncResult::Reference:
                    case SyncResult::MutableReference:
                        output = node("ReferenceType", operation->result == SyncResult::MutableReference ? "mut" : "read", call.range); output->children.push_back(clone(*payload)); break;
                    case SyncResult::Recovered: output = clone(*payload); break;
                    case SyncResult::Unit: output = node("Type", "unit", call.range); break;
                }
                if (!output) return true;
                if (failure)
                { std::vector<NodePtr> values; values.push_back(std::move(output)); values.push_back(std::move(failure)); output = specializeBuiltin("Result", std::move(values), module, call, depth + 1); }
                if (!output) return true;
                auto replacement = node("CompilerSyncExpression", operationName, call.range);
                replacement->children.push_back(std::move(output)); replacement->children.push_back(std::move(payload));
                for (std::size_t i = 1; i < call.children.size(); ++i) replacement->children.push_back(std::move(call.children[i]->children[0]));
                call = std::move(*replacement); rewritten_.insert(&call); return true;
            }
            bool taskCall(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                const Node* callee = call.children.empty() ? nullptr : call.children[0].get();
                const Node* explicitTypes = nullptr;
                if (callee && callee->kind == "GenericName")
                { explicitTypes = child(*callee, "TypeArguments"); callee = callee->children[0].get(); }
                if (!callee || callee->kind != "Member" || callee->children.size() != 1) return false;
                const auto& namespaceExpression = *callee->children[0];
                const auto& namespaceName = namespaceExpression.kind == "GenericName" && !namespaceExpression.children.empty()
                    ? *namespaceExpression.children[0] : namespaceExpression;
                const auto declared = declaration(namespaceName, module);
                if (!declared.node || !child(*declared.node, "CompilerTask")
                    || graph_.modules[declared.module]->package != "$task") return false;
                if (namespaceExpression.kind == "GenericName" && typeArguments(namespaceExpression).size() != 1)
                { error(call, "An explicit Task<T> namespace requires exactly one result type."); return true; }
                auto target = declaration(*callee, module);
                if (!target.node && namespaceExpression.kind == "GenericName")
                {
                    // Task<T>의 타입 인자는 이름 해석의 Task 심벌을 바꾸지 않는다.
                    // Task<T> namespace arguments do not change the canonically bound Task declaration.
                    for (const auto& method : declared.node->children)
                        if (method->kind == "Function" && method->value == callee->value)
                        {
                            if (target.node) { error(call, "The closed Task method is ambiguous."); return true; }
                            const auto known = declarations_.find(method.get());
                            target = known == declarations_.end() ? Declaration{method.get(), declared.module, InvalidSymbolId, declared.node} : known->second;
                        }
                }
                if (!target.node || target.node->kind != "Function" || !target.owner
                    || !child(*target.owner, "CompilerTask") || !child(*target.node, "CompilerIntrinsic")
                    || graph_.modules[target.module]->package != "$task") return false;
                const auto standardType = [&](const std::string& name, std::vector<NodePtr> arguments, const std::string& package = "$task") -> NodePtr
                {
                    for (const auto& [address, candidate] : declarations_)
                    {
                        (void)address;
                        if (candidate.owner || !typeDeclaration(*candidate.node) || !originalNodes_.contains(candidate.node)
                            || candidate.node->value != name || graph_.modules[candidate.module]->package != package) continue;
                        const auto* parameters = child(*candidate.node, "GenericParameters");
                        if ((parameters ? parameters->children.size() : 0) != arguments.size()) continue;
                        if (arguments.empty()) return use(candidate, module, "Type", call.range);
                        std::vector<const Node*> borrowed; for (const auto& value : arguments) borrowed.push_back(value.get());
                        const auto concrete = specialize(candidate, borrowed, module, call, depth + 1);
                        return concrete.node ? use(concrete, module, "Type", call.range) : NodePtr{};
                    }
                    error(call, "The trusted Task operation requires its canonical " + name + " declaration."); return {};
                };
                if (const auto* sourceAlias = child(*target.node, "CompilerTaskSourceAlias"))
                {
                    // 이 별칭은 실제 소스 함수로 연결하며 타이머 opcode나 가짜 실행기 계약을 만들지 않는다.
                    // Bind this alias to a real source function without inventing a timer opcode or scheduler contract.
                    if (sourceAlias->value != "deadline_after" || target.node->value != "deadline_after")
                    { error(call,"Unknown closed Task source alias."); return true; }
                    if (explicitTypes || !typeArguments(*callee->children[0]).empty())
                    { error(call,"Task.deadline_after does not accept function or Task result type arguments."); return true; }
                    auto receiver = standardType("Deadline", {}); if (!receiver) return true;
                    receiver->kind = receiver->kind == "Type" ? "Name" : receiver->kind == "QualifiedType" ? "Member" : receiver->kind;
                    auto member = node("Member","after",callee->range); member->children.push_back(std::move(receiver));
                    call.children[0] = std::move(member); rewritten_.insert(call.children[0].get()); return false;
                }
                if (const auto* cancellation = child(*target.node, "CompilerCancellation"))
                {
                    // 취소 별칭은 Task<T>를 구체화하지 않고 실제 비제네릭 역할의 선언으로 연결한다.
                    // Redirect cancellation aliases to their actual nongeneric role declarations without instantiating Task<T>.
                    std::string role, method;
                    if (cancellation->value == "source_new") { role = "CancellationSource"; method = "new"; }
                    else if (cancellation->value == "token_from_source") { role = "CancellationSource"; method = "token"; }
                    else if (cancellation->value == "token_clone") { role = "CancellationToken"; method = "clone"; }
                    else if (cancellation->value == "source_request") { role = "CancellationSource"; method = "request"; }
                    else if (cancellation->value == "token_requested") { role = "CancellationToken"; method = "requested"; }
                    else { error(call, "Unknown closed Task cancellation alias."); return true; }
                    if (explicitTypes) { error(call, "Task cancellation aliases do not accept type arguments."); return true; }
                    auto receiver = standardType(role, {}); if (!receiver) return true;
                    auto member = node("Member", method, callee->range); receiver->kind = receiver->kind == "Type" ? "Name" : receiver->kind == "QualifiedType" ? "Member" : receiver->kind;
                    member->children.push_back(std::move(receiver)); call.children[0] = std::move(member);
                    rewritten_.insert(call.children[0].get()); return false;
                }
                const auto* marker = child(*target.node, "CompilerTaskOperation");
                const auto* operation = marker ? taskOperation(marker->value) : nullptr;
                if (!operation) return false;
                if (call.children.size() != operation->operands + 1 || explicitTypes)
                { error(call, "Task operations infer value and callable types; use Task<T> to specify a result type and provide the exact argument count."); return true; }
                for (std::size_t index = 1; index < call.children.size(); ++index)
                {
                    if (call.children[index]->children.size() != 1 || !call.children[index]->value.empty())
                    { error(call, "Task operations currently require positional arguments."); return true; }
                    ExpectedGuard context(expected_); expected_ = nullptr;
                    process(*call.children[index]->children[0], module, locals, owner, depth + 1);
                }
                NodePtr payload;
                const auto receiverTypes = typeArguments(*callee->children[0]);
                if (receiverTypes.size() == 1) payload = clone(*receiverTypes[0]);
                if ((operation->access == TaskAccess::None || operation->access == TaskAccess::ScopeValue || operation->access == TaskAccess::CancellationShared || (!operation->worker && operation->preparation == TaskPreparation::None && operation->access == TaskAccess::ScopeShared)) && payload)
                { error(call, "Task scope operations do not accept a task result type."); return true; }
                NodePtr callable = node("Type", "unit", call.range), invoke;
                if (operation->worker)
                {
                    callable = infer(*call.children[2]->children[0], module, locals, owner);
                    const auto concrete = callable && callable->kind != "ReferenceType" ? declaration(*callable, module) : Declaration{};
                    const auto* closure = concrete.node ? child(*concrete.node, "CompilerClosure") : nullptr;
                    if (!closure || (closure->value != "Fn" && closure->value != "FnMut" && closure->value != "FnOnce"))
                    { error(call, "Task spawning requires an owned concrete zero-argument closure."); return true; }
                    Declaration implementation;
                    for (const auto& method : concrete.node->children)
                        if (method->kind == "Function" && method->value == "__invoke")
                        {
                            if (implementation.node) { error(call, "Task closure invocation must be unambiguous."); return true; }
                            const auto known = declarations_.find(method.get());
                            implementation = known == declarations_.end() ? Declaration{method.get(), concrete.module, InvalidSymbolId, concrete.node} : known->second;
                        }
                    const auto* parameters = implementation.node ? child(*implementation.node, "Parameters") : nullptr;
                    if (!implementation.node || implementation.node->children.empty() || !parameters
                        || parameters->children.size() != 1 || parameters->children[0]->value != "self")
                    { error(call, "Task workers must have no explicit arguments."); return true; }
                    auto actual = portableType(*implementation.node->children[0], implementation.module, module);
                    // 새 클로저 선언을 방문하기 전에도 결과 메타데이터는 같은 명목 구체 타입이어야 한다.
                    // Materialize fresh closure return metadata before freezing it into the closed Task expression.
                    process(*actual, module, locals, owner, depth + 1);
                    if (actual->value == "void" || actual->value == "never" || actual->kind == "DynamicType" || actual->kind == "InferredType")
                    { error(call, "Task workers require a concrete storable result; use unit for an empty result."); return true; }
                    if (payload && key(*payload, module) != key(*actual, module))
                    { error(call, "Task namespace result type does not match its closure result."); return true; }
                    payload = std::move(actual);
                    invoke = node("CompilerTaskInvoke", "", call.range);
                    auto selected = node("Member", "__invoke", call.range);
                    selected->children.push_back(use(concrete, module, "Name", call.range)); invoke->children.push_back(std::move(selected));
                }
                else if (operation->preparation == TaskPreparation::Ready)
                {
                    if (!payload) payload = infer(*call.children[2]->children[0], module, locals, owner);
                    if (!payload || payload->kind == "InferredType" || payload->kind == "DynamicType"
                        || payload->value == "void" || payload->value == "never")
                    { error(call,"Task.ready requires a concrete storable owned value; use unit for an empty result."); return true; }
                    process(*payload,module,locals,owner,depth + 1);
                }
                else if (operation->access == TaskAccess::TaskValue || operation->access == TaskAccess::TaskShared || operation->access == TaskAccess::TaskExclusive)
                {
                    auto input = infer(*call.children[1]->children[0], module, locals, owner);
                    if (input && input->kind == "ReferenceType" && !input->children.empty()) input = std::move(input->children[0]);
                    const auto actual = input ? declaration(*input, module) : Declaration{};
                    if (!actual.node || !child(*actual.node, "CompilerTask") || graph_.modules[actual.module]->package != "$task")
                    { error(call, "Task operation requires its exact concrete Task owner or borrow."); return true; }
                    NodePtr actualPayload;
                    const auto arguments = typeArguments(*input);
                    if (arguments.size() == 1) actualPayload = clone(*arguments[0]);
                    else for (const auto& field : actual.node->children)
                        if (field->kind == "Variable" && !field->children.empty()) actualPayload = portableType(*field->children[0], actual.module, module);
                    if (!actualPayload) { error(call, "Cannot determine the concrete Task result type."); return true; }
                    if (payload && key(*payload, module) != key(*actualPayload, module))
                    { error(call, "Task namespace result type does not match its input owner."); return true; }
                    payload = std::move(actualPayload);
                }
                if (operation->access == TaskAccess::CancellationShared)
                {
                    auto input = infer(*call.children[1]->children[0], module, locals, owner);
                    const bool borrowed = input && input->kind == "ReferenceType" && !input->children.empty();
                    const auto token = borrowed ? declaration(*input->children[0], module) : Declaration{};
                    if (!token.node || !child(*token.node, "CompilerCancellationToken") || graph_.modules[token.module]->package != "$task")
                    { error(call, "Task.check_cancel requires an explicit CancellationToken borrow."); return true; }
                }
                if (!payload) payload = node("Type", "unit", call.range);
                NodePtr output, failure;
                switch (operation->result)
                {
                    case TaskResult::Scope: output = standardType("TaskScope", {}); failure = standardType("SpawnError", {}); break;
                    case TaskResult::Spawn:
                    { std::vector<NodePtr> values; values.push_back(clone(*payload)); output = standardType("Task", std::move(values)); failure = standardType("SpawnError", {}); break; }
                    case TaskResult::Join: output = clone(*payload); failure = standardType("TaskError", {}, "$error"); break;
                    case TaskResult::TryTake:
                    { std::vector<NodePtr> values; values.push_back(clone(*payload)); output = specializeBuiltin("Optional", std::move(values), module, call, depth + 1); failure = standardType("TaskError", {}, "$error"); break; }
                    case TaskResult::Discard: output = node("Type", "unit", call.range); failure = standardType("TaskError", {}, "$error"); break;
                    case TaskResult::Status: output = standardType("TaskStatus", {}); break;
                    case TaskResult::Boolean: output = node("Type", "bool", call.range); break;
                    case TaskResult::Token: output = standardType("CancellationToken", {}); failure = standardType("TaskError", {}, "$error"); break;
                    case TaskResult::Executor: output = standardType("ExecutorId", {}); break;
                    case TaskResult::Identity: output = standardType("TaskId", {}); break;
                    case TaskResult::CloseAdmission: output = node("Type", "unit", call.range); failure = standardType("ScopeError", {}); break;
                    case TaskResult::ActiveCount: output = node("Type", "usize", call.range); break;
                    case TaskResult::ScopeState: output = standardType("ScopeStatus", {}); break;
                    case TaskResult::Completion: output = node("Type", "unit", call.range); failure = standardType("TaskError", {}, "$error"); break;
                    case TaskResult::CheckCancel: output = node("Type", "unit", call.range); failure = standardType("Cancelled", {}); break;
                    case TaskResult::Wait: output = standardType("WaitStatus", {}); failure = standardType("TaskError", {}, "$error"); break;
                    case TaskResult::Request: output = standardType("TaskCancelRequest", {}); failure = standardType("TaskError", {}, "$error"); break;
                    case TaskResult::Close: output = standardType("ScopeReport", {}); failure = standardType("ScopeError", {}); break;
                }
                if (!output) return true;
                if (taskFallible(operation->result))
                {
                    if (!failure) return true;
                    std::vector<NodePtr> values; values.push_back(std::move(output)); values.push_back(std::move(failure));
                    output = specializeBuiltin("Result", std::move(values), module, call, depth + 1);
                }
                if (!output) return true;
                auto replacement = node("CompilerTaskExpression", std::string(operation->name), call.range);
                replacement->children.push_back(std::move(output)); replacement->children.push_back(std::move(payload)); replacement->children.push_back(std::move(callable));
                for (std::size_t index = 1; index < call.children.size(); ++index) replacement->children.push_back(std::move(call.children[index]->children[0]));
                if (invoke) replacement->children.push_back(std::move(invoke));
                call = std::move(*replacement); rewritten_.insert(&call); return true;
            }
            bool atomicCall(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                const Node* callee = call.children.empty() ? nullptr : call.children[0].get();
                const Node* explicitTypes = nullptr;
                if (callee && callee->kind == "GenericName") { explicitTypes = child(*callee, "TypeArguments"); callee = callee->children[0].get(); }
                if (!callee || callee->kind != "Member" || callee->children.size() != 1) return false;
                const auto declared = declaration(*callee->children[0], module);
                if (!declared.node || !child(*declared.node, "CompilerAtomic")) return false;
                const auto* operation = atomicOperation(callee->value);
                if (!operation) return false;
                const bool query = operation->result == AtomicResult::Boolean || operation->result == AtomicResult::Alignment;
                if (call.children.size() != operation->operands + 1 || (explicitTypes && ((callee->value != "new" && !query) || explicitTypes->children.size() != 1)))
                { error(call, "Atomic operation has an invalid argument count or explicit scalar type."); return true; }
                NodePtr payload;
                const auto receiverTypes = typeArguments(*callee->children[0]);
                if (explicitTypes) payload = clone(*explicitTypes->children[0]);
                else if (receiverTypes.size() == 1) payload = clone(*receiverTypes[0]);
                else if (!generic(*declared.node))
                    for (const auto& field : declared.node->children)
                        if (field->kind == "Variable" && !field->children.empty()) payload = portableType(*field->children[0], declared.module, module);
                if (!payload && callee->value == "new" && expected_)
                {
                    const auto context = declaration(*expected_, module);
                    if (context.node && child(*context.node, "CompilerAtomic"))
                        for (const auto& field : context.node->children)
                            if (field->kind == "Variable" && !field->children.empty()) payload = portableType(*field->children[0], context.module, module);
                }
                for (std::size_t i = 1; i < call.children.size(); ++i)
                {
                    if (call.children[i]->children.size() != 1 || !call.children[i]->value.empty())
                    { error(call, "Atomic operations currently require positional arguments."); return true; }
                    ExpectedGuard context(expected_); expected_ = callee->value == "new" ? payload.get() : nullptr;
                    process(*call.children[i]->children[0], module, locals, owner, depth + 1);
                }
                if (!payload && callee->value == "new") payload = infer(*call.children[1]->children[0], module, locals, owner);
                if (operation->cell)
                {
                    auto input = infer(*call.children[1]->children[0], module, locals, owner);
                    if (input && input->kind == "ReferenceType") input = std::move(input->children[0]);
                    const auto actual = input ? declaration(*input, module) : Declaration{};
                    if (actual.node && child(*actual.node, "CompilerAtomic"))
                        for (const auto& field : actual.node->children)
                            if (field->kind == "Variable" && !field->children.empty())
                            {
                                auto actualPayload = portableType(*field->children[0], actual.module, module);
                                if (payload && key(*payload, module) != key(*actualPayload, module))
                                { error(call, "Atomic namespace scalar does not match the borrowed cell."); return true; }
                                payload = std::move(actualPayload);
                            }
                }
                if (!payload && (callee->value == "fence" || callee->value == "compiler_fence")) payload = node("Type", "unit", call.range);
                if (!payload) { error(call, "Cannot infer Atomic scalar type; use an explicit <T> query."); return true; }
                const auto standardType = [&](const std::string& name, std::vector<NodePtr> arguments) -> NodePtr
                {
                    for (const auto& [address, candidate] : declarations_)
                    {
                        (void)address;
                        // 같은 철자의 enum 항목이나 구체화 복제본을 타입 선언으로 선택하지 않는다.
                        // Do not select same-spelled enum variants or specialized clones as type declarations.
                        if (candidate.owner || !typeDeclaration(*candidate.node) || !originalNodes_.contains(candidate.node)
                            || candidate.node->value != name || graph_.modules[candidate.module]->package != "$atomic") continue;
                        const auto* parameters = child(*candidate.node, "GenericParameters");
                        if ((parameters ? parameters->children.size() : 0) != arguments.size()) continue;
                        if (arguments.empty()) return use(candidate, module, "Type", call.range);
                        std::vector<const Node*> borrowed; for (const auto& value : arguments) borrowed.push_back(value.get());
                        const auto concrete = specialize(candidate, borrowed, module, call, depth + 1);
                        return concrete.node ? use(concrete, module, "Type", call.range) : NodePtr{};
                    }
                    return {};
                };
                NodePtr output;
                if (operation->result == AtomicResult::Cell || operation->result == AtomicResult::Cas || operation->result == AtomicResult::Wait)
                {
                    std::vector<NodePtr> args; args.push_back(clone(*payload));
                    output = standardType(operation->result == AtomicResult::Cell ? "Atomic" : operation->result == AtomicResult::Cas ? "CasOutcome" : "WaitOutcome", std::move(args));
                }
                else if (operation->result == AtomicResult::Scalar) output = clone(*payload);
                else if (operation->result == AtomicResult::MutableReference)
                { output = node("ReferenceType", "mut", call.range); output->children.push_back(clone(*payload)); }
                else output = node("Type", operation->result == AtomicResult::Boolean ? "bool" : operation->result == AtomicResult::Alignment ? "usize" : "unit", call.range);
                if (!output) return true;
                if (operation->fallible)
                {
                    auto failure = standardType("AtomicError", {}); if (!failure) return true;
                    std::vector<NodePtr> args; args.push_back(std::move(output)); args.push_back(std::move(failure));
                    output = specializeBuiltin("Result", std::move(args), module, call, depth + 1);
                }
                if (!output) return true;
                auto replacement = node("CompilerAtomicExpression", callee->value, call.range);
                replacement->children.push_back(std::move(output)); replacement->children.push_back(std::move(payload));
                for (std::size_t i = 1; i < call.children.size(); ++i) replacement->children.push_back(std::move(call.children[i]->children[0]));
                call = std::move(*replacement); rewritten_.insert(&call); return true;
            }
            bool sharedCall(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                const Node* callee = call.children.empty() ? nullptr : call.children[0].get();
                const Node* explicitTypes = nullptr;
                if (callee && callee->kind == "GenericName") { explicitTypes = child(*callee, "TypeArguments"); callee = callee->children[0].get(); }
                if (!callee || callee->kind != "Member" || callee->children.size() != 1) return false;
                const auto declared = declaration(*callee->children[0], module);
                if (!declared.node || (!child(*declared.node, "CompilerShared") && !child(*declared.node, "CompilerWeak"))) return false;
                const bool weak = child(*declared.node, "CompilerWeak") != nullptr;
                const auto method = callee->value;
                static const std::set<std::string> strongMethods = {"new","clone","as_ref","downgrade","get_mut","try_unwrap","strong_count","weak_count","ptr_equal"};
                static const std::set<std::string> weakMethods = {"new","clone","upgrade","strong_count","weak_count","ptr_equal","is_empty","is_expired","clear"};
                if (!(weak ? weakMethods : strongMethods).contains(method)) return false;
                const auto count = weak && method == "new" ? 0u : method == "ptr_equal" ? 2u : 1u;
                if (call.children.size() != count + 1 || (explicitTypes && (method != "new" || explicitTypes->children.size() != 1)))
                { error(call, "Invalid Shared/Weak argument count or explicit payload type."); return true; }
                for (std::size_t i = 1; i < call.children.size(); ++i)
                    if (call.children[i]->children.size() != 1 || !call.children[i]->value.empty())
                    { error(call, "Shared/Weak currently require positional arguments."); return true; }
                NodePtr payload;
                const auto receiverTypes = typeArguments(*callee->children[0]);
                if (explicitTypes) payload = clone(*explicitTypes->children[0]);
                else if (receiverTypes.size() == 1) payload = clone(*receiverTypes[0]);
                else if (!generic(*declared.node))
                    for (const auto& field : declared.node->children)
                        if (field->kind == "Variable" && !field->children.empty()) payload = portableType(*field->children[0], declared.module, module);
                for (std::size_t i = 1; i < call.children.size(); ++i)
                {
                    ExpectedGuard context(expected_); expected_ = method == "new" ? payload.get() : nullptr;
                    process(*call.children[i]->children[0], module, locals, owner, depth + 1);
                }
                if (method == "new" && !weak && !payload) payload = infer(*call.children[1]->children[0], module, locals, owner);
                else if (method != "new")
                {
                    auto input = infer(*call.children[1]->children[0], module, locals, owner);
                    if (input && input->kind == "ReferenceType") input = std::move(input->children[0]);
                    const auto actual = input ? declaration(*input, module) : Declaration{};
                    if (actual.node && child(*actual.node, weak ? "CompilerWeak" : "CompilerShared"))
                        for (const auto& field : actual.node->children)
                            if (field->kind == "Variable" && !field->children.empty())
                            {
                                auto actualPayload = portableType(*field->children[0], actual.module, module);
                                if (payload && key(*payload, module) != key(*actualPayload, module))
                                { error(call, "Shared/Weak namespace payload does not match the borrowed owner."); return true; }
                                payload = std::move(actualPayload);
                            }
                }
                else if (!payload && expected_)
                {
                    const auto expected = declaration(*expected_, module);
                    if (expected.node && child(*expected.node, "CompilerWeak"))
                        for (const auto& field : expected.node->children)
                            if (field->kind == "Variable" && !field->children.empty()) payload = portableType(*field->children[0], expected.module, module);
                }
                if (!payload) { error(call, "Cannot infer Shared/Weak payload type; specify <T> for empty Weak.new."); return true; }
                const auto ownerType = [&](bool makeWeak) -> NodePtr
                {
                    for (const auto& [address, candidate] : declarations_)
                    {
                        (void)address;
                        if (generic(*candidate.node) && child(*candidate.node, makeWeak ? "CompilerWeak" : "CompilerShared")
                            && graph_.modules[candidate.module]->package == "$memory")
                        {
                            const auto concrete = specialize(candidate, {payload.get()}, module, call, depth + 1);
                            return concrete.node ? use(concrete, module, "Type", call.range) : NodePtr{};
                        }
                    }
                    return {};
                };
                NodePtr output;
                if (weak && method == "new") output = ownerType(true);
                else if (method == "strong_count" || method == "weak_count") output = node("Type", "usize", call.range);
                else if (method == "is_empty" || method == "is_expired" || method == "ptr_equal") output = node("Type", "bool", call.range);
                else if (method == "clear") output = node("Type", "unit", call.range);
                else if (method == "as_ref" || method == "get_mut")
                {
                    output = node("ReferenceType", method == "get_mut" ? "mut" : "read", call.range); output->children.push_back(clone(*payload));
                    if (method == "get_mut") { std::vector<NodePtr> args; args.push_back(std::move(output)); output = specializeBuiltin("Optional", std::move(args), module, call, depth + 1); }
                }
                else
                {
                    NodePtr value, failure;
                    if (method == "new")
                    {
                        value = ownerType(false); std::vector<NodePtr> args; args.push_back(clone(*payload));
                        failure = specializeBuiltin("InputError", std::move(args), module, call, depth + 1);
                    }
                    else if (method == "try_unwrap") { value = clone(*payload); failure = ownerType(false); }
                    else
                    {
                        value = ownerType(method == "downgrade" || (weak && method == "clone"));
                        if (method == "upgrade") { std::vector<NodePtr> args; args.push_back(std::move(value)); value = specializeBuiltin("Optional", std::move(args), module, call, depth + 1); }
                        for (const auto& [address, candidate] : declarations_)
                        {
                            (void)address;
                            if (candidate.node->value == "RefCountError" && graph_.modules[candidate.module]->package == "$memory")
                            { failure = use(candidate, module, "Type", call.range); break; }
                        }
                    }
                    if (!value || !failure) return true;
                    std::vector<NodePtr> args; args.push_back(std::move(value)); args.push_back(std::move(failure));
                    output = specializeBuiltin("Result", std::move(args), module, call, depth + 1);
                }
                if (!output) return true;
                auto replacement = node("CompilerSharedExpression", (weak ? "weak." : "shared.") + method, call.range);
                replacement->children.push_back(std::move(output));
                for (std::size_t i = 1; i < call.children.size(); ++i) replacement->children.push_back(std::move(call.children[i]->children[0]));
                call = std::move(*replacement); rewritten_.insert(&call); return true;
            }
            bool boxConstructor(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                if (call.children.empty()) return false;
                const Node* callee = call.children[0].get();
                const Node* explicitTypes = callee->kind == "GenericName" ? child(*callee, "TypeArguments") : nullptr;
                if (callee->kind == "GenericName") callee = callee->children[0].get();
                if (callee->kind != "Member" || callee->children.empty()) return false;
                const auto box = declaration(*callee->children[0], module);
                if (!box.node || !child(*box.node, "CompilerBox")) return false;
                const auto method = callee->value;
                if (method != "new" && method != "into_dyn" && method != "into_inner" && method != "as_ref" && method != "as_mut") return false;
                if (call.children.size() != 2 || call.children[1]->children.size() != 1)
                { error(call, "Box intrinsic requires exactly one value argument."); return true; }
                if (!call.children[1]->value.empty() && call.children[1]->value != (method == "new" ? "value" : "owner"))
                { error(call, "Unknown Box intrinsic argument name."); return true; }
                if (explicitTypes && (method != "new" || explicitTypes->children.size() != 1))
                { error(call, "Only Box.new accepts one explicit payload type argument."); return true; }
                { ExpectedGuard context(expected_); expected_ = nullptr; process(*call.children[1]->children[0], module, locals, owner, depth + 1); }
                auto input = infer(*call.children[1]->children[0], module, locals, owner);
                if (method == "new")
                {
                    const auto ownerTypes = typeArguments(*callee->children[0]);
                    if (explicitTypes) input = clone(*explicitTypes->children[0]);
                    else if (ownerTypes.size() == 1) input = clone(*ownerTypes[0]);
                }
                if (!input) { error(call, "Cannot infer the Box intrinsic input type."); return true; }
                NodePtr output;
                if (method == "new")
                {
                    Declaration boxTemplate, errorTemplate, resultTemplate;
                    for (const auto& [address, candidate] : declarations_)
                    {
                        (void)address;
                        if (!generic(*candidate.node)) continue;
                        if (child(*candidate.node, "CompilerBox")) boxTemplate = candidate;
                        if (candidate.node->value == "InputError") errorTemplate = candidate;
                        if (carrierName(candidate) == "Result") resultTemplate = candidate;
                    }
                    if (!boxTemplate.node || !errorTemplate.node || !resultTemplate.node)
                    { error(call, "Box.new requires the core Result and memory InputError declarations."); return true; }
                    const auto allocated = specialize(boxTemplate, {input.get()}, module, call, depth);
                    const auto failure = specialize(errorTemplate, {input.get()}, module, call, depth);
                    if (!allocated.node || !failure.node) return true;
                    auto allocatedType = use(allocated, module, "Type", call.range);
                    auto failureType = use(failure, module, "Type", call.range);
                    const auto result = specialize(resultTemplate, {allocatedType.get(), failureType.get()}, module, call, depth);
                    if (!result.node) return true;
                    output = use(result, module, "Type", call.range);
                }
                else if (method == "into_inner")
                {
                    // 대여가 아닌 구체 Box 값 전체에서만 결과 타입을 가져온다.
                    // Infer the result only from one complete concrete Box value, never from an owner borrow.
                    // 다른 모듈의 반환 서명에서 추론한 Box<T>도 먼저 실제 구체 소유자로 연결한다.
                    // Materialize Box<T> inferred from another module's return signature before selecting its concrete payload.
                    if (input->kind != "ReferenceType") process(*input, module, locals, owner, depth + 1);
                    const auto inputBox = declaration(*input, module);
                    if (input->kind == "ReferenceType" || !inputBox.node || !child(*inputBox.node, "CompilerBox"))
                    { error(call, "Box.into_inner requires an owned Box value."); return true; }
                    const Node* payload = nullptr;
                    for (const auto& field : inputBox.node->children) if (field->kind == "Variable" && !field->children.empty()) payload = field->children[0].get();
                    if (!payload) { error(call, "Box.into_inner has no canonical payload type."); return true; }
                    output = portableType(*payload, inputBox.module, module);
                }
                else if (method == "into_dyn")
                {
                    if (!expected_) { error(call, "Box.into_dyn requires a destination Box<dyn Interface> type."); return true; }
                    output = clone(*expected_); process(*output, module, locals, owner, depth + 1);
                }
                else
                {
                    if (input->kind != "ReferenceType") { error(call, "Box.as_ref/as_mut requires an explicit owner borrow."); return true; }
                    if (method == "as_mut" && input->value != "mut") { error(call, "Box.as_mut requires &mut owner."); return true; }
                    const auto inputBox = declaration(*input->children[0], module);
                    if (!inputBox.node || !child(*inputBox.node, "CompilerBox")) { error(call, "Expected a Box owner borrow."); return true; }
                    const Node* payload = nullptr;
                    for (const auto& field : inputBox.node->children) if (field->kind == "Variable") payload = field->children[0].get();
                    if (!payload) { error(call, "Box has no canonical payload type."); return true; }
                    output = node("ReferenceType", method == "as_mut" ? "mut" : "read", call.range);
                    output->children.push_back(portableType(*payload, inputBox.module, module));
                }
                auto replacement = node("CompilerBoxExpression", method, call.range);
                replacement->children.push_back(std::move(output));
                replacement->children.push_back(std::move(call.children[1]->children[0]));
                call = std::move(*replacement); rewritten_.insert(&call); return true;
            }

            void process(Node& item, ModuleId module, Locals locals, Node* owner, std::size_t depth)
            {
                if (depth > 256) { error(item, "Generic traversal nesting limit exceeded."); return; }
                if (item.kind == "Variable" || item.kind == "Parameter") canonicalizeDynamic(item);
                ExpectedGuard expectedGuard(expected_);
                ExpectedGuard resultGuard(functionResult_);
                if (item.kind == "Function" && !item.children.empty()) { functionResult_ = item.children[0].get(); expected_ = nullptr; }
                else if ((item.kind == "Variable" || item.kind == "Parameter") && !item.children.empty()) expected_ = item.children[0].get();
                else if (item.kind == "Return") expected_ = functionResult_;
                if (child(item, "CompilerTemplate") || item.kind == "Interface") return;
                if (item.kind == "Closure") { lowerClosure(item, module, locals, owner, depth); return; }
                if (item.kind.starts_with("Compiler")) return;
                if (item.kind == "Name" || item.kind == "Member")
                {
                    const auto constant = declaration(item, module);
                    if (const auto* level = constant.node ? child(*constant.node, "CompilerSystemLevel") : nullptr)
                    {
                        for (const auto& [address, candidate] : declarations_)
                        {
                            (void)address;
                            if (candidate.module == constant.module && candidate.node->kind == "Enum" && candidate.node->value == "LogLevel")
                            {
                                auto value = node("Member", level->value, item.range);
                                value->children.push_back(use(candidate, module, "Name", item.range));
                                item = std::move(*value); rewritten_.insert(&item); break;
                            }
                        }
                    }
                }
                if (item.kind == "Struct" || item.kind == "Enum") owner = &item;
                if (item.kind == "Type" || item.kind == "QualifiedType")
                {
                    const auto alias = declaration(item, module);
                    if (alias.node && alias.node->kind == "TypeAlias" && !alias.node->children.empty())
                    {
                        auto expanded = portableType(*alias.node->children[0], alias.module, module);
                        const auto range = item.range;
                        item = std::move(*expanded);
                        item.range = range;
                        rewritten_.insert(&item);
                    }
                }
                if ((item.kind == "Type" || item.kind == "QualifiedType") && item.value != "Self")
                {
                    const auto target = declaration(item, module);
                    if (target.node && generic(*target.node) && typeDeclaration(*target.node))
                    {
                        const auto concrete = specialize(target, typeArguments(item), module, item, depth);
                        if (concrete.node)
                        {
                            auto replacement = use(concrete, module, "Type", item.range);
                            item = std::move(*replacement);
                            rewritten_.insert(&item);
                        }
                        return;
                    }
                }
                if (item.kind == "GenericName" && !item.children.empty())
                {
                    const auto target = declaration(*item.children[0], module);
                    if (target.node && generic(*target.node) && typeDeclaration(*target.node))
                    {
                        const auto concrete = specialize(target, typeArguments(item), module, item, depth);
                        if (concrete.node)
                        {
                            auto replacement = use(concrete, module, "Name", item.range);
                            item = std::move(*replacement);
                            rewritten_.insert(&item);
                        }
                        return;
                    }
                }
                if (item.kind == "NamedInitializer" && !item.children.empty())
                {
                    const auto alias = declaration(*item.children[0], module);
                    if (alias.node && alias.node->kind == "TypeAlias" && !alias.node->children.empty())
                    {
                        auto expanded = portableType(*alias.node->children[0], alias.module, module);
                        process(*expanded, module, locals, owner, depth + 1);
                        const auto asValue = [&](const auto& self, NodePtr syntax) -> NodePtr
                        {
                            if (syntax->kind != "Type" && syntax->kind != "QualifiedType") return syntax;
                            const bool qualified = syntax->kind == "QualifiedType";
                            auto base = node(qualified ? "Member" : "Name", syntax->value, syntax->range);
                            const auto first = qualified ? 1u : 0u;
                            if (qualified) base->children.push_back(self(self, std::move(syntax->children[0])));
                            if (syntax->children.size() == first) return base;
                            auto generic = node("GenericName", "", syntax->range);
                            auto arguments = node("TypeArguments", "", syntax->range);
                            for (std::size_t i = first; i < syntax->children.size(); ++i)
                                arguments->children.push_back(std::move(syntax->children[i]));
                            generic->children.push_back(std::move(base));
                            generic->children.push_back(std::move(arguments));
                            return generic;
                        };
                        item.children[0] = asValue(asValue, std::move(expanded));
                        rewritten_.insert(item.children[0].get());
                    }
                    auto& constructor = *item.children[0];
                    if (constructor.kind == "Name" || constructor.kind == "Member")
                    {
                        const auto target = declaration(constructor, module);
                        if (target.node && generic(*target.node) && typeDeclaration(*target.node))
                        {
                            const auto concrete = specialize(target, {}, module, item, depth);
                            if (concrete.node)
                            {
                                item.children[0] = use(concrete, module, "Name", constructor.range);
                                rewritten_.insert(item.children[0].get());
                            }
                        }
                    }
                    else if (constructor.kind == "GenericName")
                    {
                        const auto target = declaration(*constructor.children[0], module);
                        if (target.node && generic(*target.node) && typeDeclaration(*target.node))
                        {
                            const auto concrete = specialize(target, typeArguments(constructor), module, item, depth);
                            if (concrete.node) item.children[0] = use(concrete, module, "Name", constructor.range);
                        }
                    }
                }
                if (item.kind == "ArrayLiteral" && expected_)
                {
                    auto collection = clone(*expected_);
                    const auto arguments = carrierArguments(*expected_, module, "Result");
                    if (!arguments.empty()) collection = clone(*arguments[0]);
                    if (containsDynamic(*collection))
                    {
                        auto resultType = node("Type", "Result", item.range);
                        resultType->children.push_back(clone(*collection));
                        Declaration collectionError;
                        for (const auto& [address, candidate] : declarations_)
                        {
                            (void)address;
                            if (graph_.modules[candidate.module]->name == "$core" && candidate.node->value == "CollectionError") { collectionError = candidate; break; }
                        }
                        if (!collectionError.node) { error(item, "Missing builtin CollectionError."); return; }
                        resultType->children.push_back(use(collectionError, module, "Type", item.range));
                        Declaration resultTemplate;
                        for (const auto& [address, candidate] : declarations_)
                        {
                            (void)address;
                            if (generic(*candidate.node) && carrierName(candidate) == "Result") { resultTemplate = candidate; break; }
                        }
                        if (!resultTemplate.node) { error(item, "Dynamic literal requires the builtin Result type."); return; }
                        std::vector<const Node*> arguments;
                        for (const auto& argument : resultType->children) arguments.push_back(argument.get());
                        const auto concrete = specialize(resultTemplate, arguments, module, item, depth + 1);
                        if (!concrete.node) return;
                        resultType = use(concrete, module, "Type", item.range);
                        auto marker = node("CompilerArrayResult", "", item.range);
                        marker->children.push_back(std::move(resultType));
                        item.children.push_back(std::move(marker));
                        dynamicElements(item, *collection, module, locals, owner, depth + 1);
                        return;
                    }
                }
                if (item.kind == "Call" && !item.children.empty())
                {
                    if (taskCall(item, module, locals, owner, depth)) return;
                    if (memoryCall(item, module, locals, owner, depth)) return;
                    if (systemCall(item, module, locals, owner, depth)) return;
                    if (parseCall(item, module, locals, owner, depth)) return;
                    if (sliceResultCall(item, module, locals, owner, depth)) return;
                    if (textResultCall(item, module, locals, owner, depth)) return;
                    if (charScalarCall(item, module, locals, owner, depth)) return;
                    if (floatBitsCall(item, module, locals, owner, depth)) return;
                    if (numericCall(item, module, locals, owner, depth)) return;
                    if (rangeInspectCall(item, module, locals, owner, depth)) return;
                    if (rangeResultCall(item, module, locals, owner, depth)) return;
                    if (collectionCall(item, module, locals, owner, depth)) return;
                    if (syncCall(item, module, locals, owner, depth)) return;
                    if (atomicCall(item, module, locals, owner, depth)) return;
                    if (sharedCall(item, module, locals, owner, depth)) return;
                    if (boxConstructor(item, module, locals, owner, depth)) return;
                    if (coreConstructor(item, module, locals, owner, depth)) return;
                    // 인수의 중첩 호출을 먼저 구체화하여 반환 타입 추론에 사용한다.
                    // Specialize nested argument calls first so inference can use their return types.
                    const auto argumentTargets = references(*item.children[0], module);
                    for (std::size_t index = 1; index < item.children.size(); ++index)
                    {
                        ExpectedGuard argumentContext(expected_); expected_ = nullptr;
                        NodePtr argumentExpected;
                        if (argumentTargets.size() == 1)
                            if (const auto* parameters = child(*argumentTargets[0].node, "Parameters"))
                            {
                                const Node* parameter = index - 1 < parameters->children.size() ? parameters->children[index - 1].get() : nullptr;
                                if (!item.children[index]->value.empty())
                                    for (const auto& candidate : parameters->children) if (candidate->value == item.children[index]->value) parameter = candidate.get();
                                if (parameter && !parameter->children.empty())
                                { argumentExpected = portableType(*parameter->children[0], argumentTargets[0].module, module); expected_ = argumentExpected.get(); }
                            }
                        process(*item.children[index], module, locals, owner, depth + 1);
                    }
                    process(*item.children[0], module, locals, owner, depth + 1);
                    rewriteClosureCall(item, module, locals, owner);
                    auto& callee = *item.children[0];
                    const bool explicitArguments = callee.kind == "GenericName";
                    const Node* receiver = nullptr;
                    const auto candidates = callCandidates(callee, module, locals, owner, receiver);
                    bool failedRequirements = false;
                    auto matches = matchCalls(item, candidates, receiver, module, locals, owner, failedRequirements);
                    if (!matches.empty())
                    {
                        const auto best = std::max_element(matches.begin(), matches.end(), worseCall);
                        const auto count = std::count_if(matches.begin(), matches.end(), [&](const CallMatch& value) { return sameCallRank(value, *best); });
                        if (best->generic)
                        {
                            if (count != 1) error(item, "Generic call is ambiguous; specify type arguments or use a distinct overload.");
                            else
                            {
                                const auto& target = best->declaration;
                                std::vector<const Node*> arguments;
                                for (const auto& parameter : child(*target.node, "GenericParameters")->children) arguments.push_back(best->bindings.at(parameter->value).get());
                                const auto concrete = specialize(target, arguments, module, item, depth);
                                if (concrete.node)
                                {
                                    const Node& originalCallee = explicitArguments ? *callee.children[0] : callee;
                                    if (concrete.owner && originalCallee.kind == "Member")
                                    {
                                        auto replacement = clone(originalCallee); replacement->value = concrete.node->value;
                                        item.children[0] = std::move(replacement); rewritten_.insert(item.children[0].get());
                                    }
                                    else item.children[0] = use(concrete, module, "Name", callee.range);
                                }
                            }
                        }
                    }
                    else
                    {
                        const bool hasTemplate = std::any_of(candidates.begin(), candidates.end(), [](const auto& target)
                        { return target.node->kind == "Function" && (generic(*target.node) || child(*target.node, "CompilerUnavailable")); });
                        if (hasTemplate) error(item, failedRequirements ? "Concrete type does not satisfy the generic implements requirements." : "Cannot infer a complete, consistent generic type argument list.");
                    }
                    // 비제네릭 후보를 고른 호출은 기존 오버로드 검사에 맡긴다.
                    // Leave calls that selected ordinary candidates to the existing overload checker.
                    return;
                }
                if (item.kind == "Function")
                {
                    if (auto* parameters = child(item, "Parameters"))
                        for (auto& parameter : parameters->children)
                        {
                            process(*parameter, module, locals, owner, depth + 1);
                            if (const auto* type = localParameterType(*parameter)) locals[parameter->value] = type;
                        }
                }
                for (std::size_t index = 0; index < item.children.size(); ++index)
                {
                    auto& nested = *item.children[index];
                    if (item.kind == "Function" && nested.kind == "Parameters") continue;
                    // 전체 경로의 모듈 접두사를 같은 이름의 타입 별칭으로 다시 해석하지 않는다.
                    // Do not reinterpret a qualified module prefix as a same-named type alias.
                    if (item.kind == "QualifiedType" && index == 0) continue;
                    ExpectedGuard fieldContext(expected_);
                    NodePtr fieldExpected;
                    if (item.kind == "NamedInitializer" && nested.kind == "FieldInitializer" && !item.children.empty())
                    {
                        expected_ = nullptr;
                        const auto target = declaration(*item.children[0], module);
                        if (target.node)
                            for (const auto& field : target.node->children)
                                if (field->kind == "Variable" && field->value == nested.value && !field->children.empty())
                                { fieldExpected = portableType(*localParameterType(*field), target.module, module); expected_ = fieldExpected.get(); }
                    }
                    process(nested, module, locals, owner, depth + 1);
                    if (item.kind == "Case" && nested.kind == "PatternBindings")
                        for (const auto& binding : nested.children)
                            if (const auto* boundType = localParameterType(*binding)) locals[binding->value] = boundType;
                    if ((item.kind == "Block" || item.kind == "CaseScope" || item.kind == "For") && nested.kind == "Variable" && !nested.children.empty())
                    {
                        if (nested.children[0]->kind != "InferredType") locals[nested.value] = localParameterType(nested);
                        else
                            for (std::size_t value = 1; value < nested.children.size(); ++value)
                                if (nested.children[value]->kind != "ArraySuffixes" && nested.children[value]->kind != "Modifier")
                                {
                                    auto inferred = infer(*nested.children[value], module, locals, owner);
                                    if (inferred) { locals[nested.value] = inferred.get(); inferred_.push_back(std::move(inferred)); }
                                    break;
                                }
                    }
                }
            }
            bool moduleDeclaration(const Declaration& target)
            {
                if (!target.node || target.module >= graph_.modules.size()) return false;
                for (const auto& item : graph_.modules[target.module]->ast->children) if (item.get() == target.node) return true;
                return false;
            }
            void relocate(Node& item, ModuleId from, ModuleId into)
            {
                if (from == into) return;
                if ((item.kind == "Type" || item.kind == "QualifiedType") && item.value != "Self")
                {
                    const auto target = declaration(item, from);
                    if (target.node && typeDeclaration(*target.node))
                    {
                        auto replacement = portableType(item, from, into);
                        item = std::move(*replacement); rewritten_.insert(&item);
                        return;
                    }
                }
                if (item.kind == "Name" || item.kind == "Member")
                {
                    const auto target = declaration(item, from);
                    if (moduleDeclaration(target))
                    {
                        auto replacement = use(target, into, "Name", item.range);
                        item = std::move(*replacement); rewritten_.insert(&item);
                        return;
                    }
                }
                for (auto& nested : item.children) relocate(*nested, from, into);
            }
            void validateDefaultAccess(const Node& item, const Node& contract)
            {
                if (item.kind == "Member" && !item.children.empty())
                {
                    const Node* receiver = item.children[0].get();
                    while ((receiver->kind == "Group" || (receiver->kind == "Prefix" && receiver->value == "*")) && !receiver->children.empty()) receiver = receiver->children[0].get();
                    if (receiver->kind == "Name" && receiver->value == "self")
                    {
                        bool found = false;
                        for (const auto& member : contract.children)
                            if (member->kind == "Function" && member->value == item.value) found = true;
                        if (!found) error(item, "An interface default may access only members declared by its interface.");
                    }
                }
                for (const auto& nested : item.children) validateDefaultAccess(*nested, contract);
            }
            std::map<std::string, std::string> genericNames(const Node& function, std::string_view self = {}) const
            {
                std::map<std::string, std::string> result;
                if (const auto* parameters = child(function, "GenericParameters"))
                    for (std::size_t index = 0; index < parameters->children.size(); ++index) result[parameters->children[index]->value] = "$generic" + std::to_string(index);
                if (!self.empty()) result["Self"] = std::string(self);
                return result;
            }
            std::string normalizedSpelling(const Node& item, const std::map<std::string, std::string>& names, ModuleId module)
            {
                if (const auto found = names.find(item.value); found != names.end() && item.children.empty())
                {
                    if (item.value != "Self") return found->second;
                    auto concrete = node("Type", found->second, item.range);
                    return normalizedSpelling(*concrete, {}, module);
                }
                if (item.kind == "ReferenceType") return "&" + item.value + normalizedSpelling(*item.children[0], names, module);
                if (item.kind == "Type" || item.kind == "QualifiedType")
                {
                    const auto selected = declaration(item, module);
                    if (selected.node && selected.node->kind == "TypeAlias" && !selected.node->children.empty())
                    {
                        auto expanded = portableType(*selected.node->children[0], selected.module, module);
                        return normalizedSpelling(*expanded, names, module);
                    }
                    if (selected.node && logicalTypes_.contains(selected.node))
                    {
                        const auto& logical = logicalTypes_.at(selected.node);
                        auto restored = portableType(*logical.first, logical.second, module);
                        return normalizedSpelling(*restored, names, module);
                    }
                    // 소스 타입과 이미 구체화된 타입을 같은 명목 타입·인자 구조로 비교한다.
                    // Compare source types and materialized types through the same nominal identity and arguments.
                    std::string result = selected.node ? graph_.modules[selected.module]->name + "." + selected.node->value : flatPath(item);
                    if (result == "int") result = "i32";
                    if (result == "float") result = "f64";
                    const auto arguments = typeArguments(item);
                    if (!arguments.empty())
                    {
                        result += '<';
                        for (const auto* argument : arguments) result += normalizedSpelling(*argument, names, module) + ',';
                        result += '>';
                    }
                    return result;
                }
                if (item.kind == "Member") return normalizedSpelling(*item.children[0], names, module) + "." + item.value;
                std::string result = item.value == "int" ? "i32" : item.value == "float" ? "f64" : item.value;
                if (!item.children.empty())
                {
                    result += '<';
                    for (const auto& argument : item.children) result += normalizedSpelling(*argument, names, module) + ',';
                    result += '>';
                }
                return result;
            }
            std::string resultKey(const Node& function, ModuleId module, std::string_view self = {})
            {
                return function.children.empty() ? std::string{} : normalizedSpelling(*function.children[0], genericNames(function, self), module);
            }
            std::set<std::string> requirementKeys(const Node& function, ModuleId module, std::string_view self = {})
            {
                std::set<std::string> result;
                const auto names = genericNames(function, self);
                for (const auto& item : function.children)
                    if (item->kind == "Constraint") result.insert(normalizedSpelling(*item, names, module));
                return result;
            }
            std::string methodKey(const Node& function, ModuleId module, std::string_view self = {})
            {
                const auto names = genericNames(function, self);
                const auto* parameters = child(function, "GenericParameters");
                const auto arity = parameters ? parameters->children.size() : 0;
                std::string result = function.value + '<' + std::to_string(arity) + ">(";
                if (const auto* parameters = child(function, "Parameters"))
                    for (const auto& parameter : parameters->children)
                    {
                        result += parameter->kind == "VariadicParameter" ? "variadic:" : "";
                        if (!parameter->children.empty()) result += normalizedSpelling(*parameter->children[0], names, module);
                        if (const auto* arrays = child(*parameter, "ArraySuffixes")) result += normalizedSpelling(*arrays, names, module);
                        result += ';';
                    }
                return result + ')';
            }
            void validateInterfaces(Node& owner, ModuleId module)
            {
                if (validated_.contains(&owner) || child(owner, "CompilerTemplate") || child(owner, "CompilerInterfacesValidated")) return;
                if (!validating_.insert(&owner).second) { error(owner, "Cyclic static interface implementation validation."); return; }
                struct Requirement { NodePtr function; ModuleId module; bool hasDefault; };
                std::map<std::string, std::vector<Requirement>> required;
                Bindings self;
                self["Self"] = node("Type", owner.value, owner.range);
                const auto* interfaces = child(owner, "Implements");
                if (interfaces)
                    for (const auto& implemented : interfaces->children)
                    {
                        if (implemented->value == "Copy" || implemented->value == "Drop") continue;
                        const auto target = declaration(*implemented, module);
                        if (!target.node || target.node->kind != "Interface")
                        { error(*implemented, "An implements entry must identify an interface."); continue; }
                        if (child(*target.node, "CompilerNumericContract"))
                        { error(*implemented, "Sealed numeric format contracts can only be implemented by builtin scalar formats."); continue; }
                        for (const auto& method : target.node->children)
                        {
                            if (method->kind != "Function") continue;
                            if (child(*method, "Block")) validateDefaultAccess(*method, *target.node);
                            auto signature = clone(*method);
                            relocate(*signature, target.module, module);
                            substitute(*signature, self, module, module);
                            const auto key = methodKey(*signature, module);
                            required[key].push_back({std::move(signature), target.module, child(*method, "Block") != nullptr});
                        }
                    }
                std::set<const Node*> acceptedOverrides;
                for (auto& [key, contracts] : required)
                {
                    Node* implementation = nullptr;
                    for (auto& method : owner.children)
                        if (method->kind == "Function" && methodKey(*method, module, owner.value) == key) { implementation = method.get(); break; }
                    const auto defaults = std::count_if(contracts.begin(), contracts.end(), [](const Requirement& item) { return item.hasDefault; });
                    for (const auto& requirement : contracts)
                    {
                        if (!contracts[0].function->children.empty() && !requirement.function->children.empty()
                            && resultKey(*contracts[0].function, module) != resultKey(*requirement.function, module))
                            error(owner, "Static interfaces require incompatible return types for the same method signature.");
                    }
                    if (implementation)
                    {
                        if (implementation->value == "drop" && implementsDrop(owner))
                            error(*implementation, "A Drop cleanup method cannot implement a callable interface method named drop.");
                        if (!implementation->children.empty() && !contracts[0].function->children.empty()
                            && resultKey(*implementation, module, owner.value) != resultKey(*contracts[0].function, module))
                            error(*implementation, "Interface implementation return type does not match its contract.");
                        if (requirementKeys(*implementation, module, owner.value) != requirementKeys(*contracts[0].function, module))
                            error(*implementation, "Generic interface implementation requirements do not match its contract.");
                        if (!modifier(*implementation, "public")) error(*implementation, "An interface implementation method must be public.");
                        if (defaults && !modifier(*implementation, "override")) error(*implementation, "Replacing an interface default implementation requires override.");
                        if (!defaults && modifier(*implementation, "override")) error(*implementation, "override requires an inherited default implementation.");
                        if (defaults) acceptedOverrides.insert(implementation);
                    }
                    else if (defaults > 1) error(owner, "Conflicting interface defaults require an explicit override.");
                    else if (defaults == 1)
                    {
                        auto found = std::find_if(contracts.begin(), contracts.end(), [](const Requirement& item) { return item.hasDefault; });
                        auto method = std::move(found->function);
                        if (auto* flags = child(*method, "Modifier"))
                        {
                            if (!modifier(*method, "public")) flags->value += " public";
                        }
                        else method->children.push_back(node("Modifier", "public", method->range));
                        mark(*method, "CompilerDefault");
                        // 기본 구현은 계약 안의 함수만 사용하며 구체 필드에 직접 접근하지 않는다.
                        // A default implementation uses contract methods, not concrete fields directly.
                        auto* added = method.get();
                        owner.children.push_back(std::move(method));
                        declarations_[added] = {added, module, InvalidSymbolId, &owner};
                        index(*added, module, &owner);
                        if (generic(*added)) mark(*added, "CompilerTemplate");
                        process(*added, module, {}, &owner, 0);
                    }
                    else error(owner, "Missing required interface method '" + contracts[0].function->value + "'.");
                }
                for (const auto& method : owner.children)
                    if (method->kind == "Function" && !child(*method, "CompilerMethodSpecialization") && modifier(*method, "override") && !acceptedOverrides.contains(method.get()))
                    {
                        bool known = false;
                        const auto matching = required.find(methodKey(*method, module, owner.value));
                        if (matching != required.end()) known = std::any_of(matching->second.begin(), matching->second.end(), [](const Requirement& item) { return item.hasDefault; });
                        if (!known) error(*method, "override does not match an inherited default method.");
                    }
                mark(owner, "CompilerInterfacesValidated");
                validating_.erase(&owner); validated_.insert(&owner);
            }
        };
    }
    GenericResolver::GenericResolver(Diagnostics& diagnostics) : diagnostics_(diagnostics) {}
    std::optional<NameResolution> GenericResolver::resolve(ModuleGraph& graph, const NameResolution& names)
    {
        if (!names.succeeded || !diagnostics_.empty()) return std::nullopt;
        return Resolver(graph, names, diagnostics_).run();
    }
}
