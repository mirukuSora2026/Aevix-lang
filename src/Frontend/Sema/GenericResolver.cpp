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
#include "aevix/Frontend/Sema/ConstEvaluator.hpp"

#include <algorithm>
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
                for (ModuleId module = 0; module < graph.modules.size(); ++module)
                    index(*graph.modules[module]->ast, module, nullptr);
                for (const auto& [expression, ids] : names.references)
                {
                    (void)ids;
                    referencePaths_[expression] = flatPath(*expression);
                }
            }
            std::optional<NameResolution> run()
            {
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
            std::unordered_set<const Node*> rewritten_;
            std::unordered_set<const Node*> validated_;
            std::unordered_set<const Node*> validating_;
            std::size_t generated_ = 0;
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
                origins_.erase(result.get());
                return result;
            }
            void error(const Node& location, std::string message)
            {
                diagnostics_.push_back({location.range, std::move(message)});
            }
            void mark(Node& declaration, std::string kind)
            {
                if (!child(declaration, kind)) declaration.children.push_back(node(std::move(kind), "", declaration.range));
            }
            void index(Node& declaration, ModuleId module, Node* owner)
            {
                if (typeDeclaration(declaration) || declaration.kind == "Function")
                {
                    auto found = declarations_.find(&declaration);
                    if (found == declarations_.end()) declarations_[&declaration] = {&declaration, module, InvalidSymbolId, owner};
                    else found->second.owner = owner;
                }
                Node* nested = typeDeclaration(declaration) || declaration.kind == "Variant" ? &declaration : owner;
                for (auto& item : declaration.children) index(*item, module, nested);
            }
            NodePtr clone(const Node& original)
            {
                auto result = node(original.kind, original.value, original.range);
                result->comments = original.comments; result->height = original.height;
                const auto origin = origins_.find(&original);
                origins_[result.get()] = origin == origins_.end() ? &original : origin->second;
                for (const auto& item : original.children) result->children.push_back(clone(*item));
                return result;
            }
            std::vector<Declaration> references(const Node& expression, ModuleId module)
            {
                const Node* origin = &expression;
                const auto cloned = origins_.find(origin);
                if (cloned != origins_.end()) origin = cloned->second;
                const auto found = names_.references.find(origin);
                std::vector<Declaration> result;
                const auto originalPath = referencePaths_.find(origin);
                const bool originalSpelling = originalPath != referencePaths_.end() && flatPath(expression) == originalPath->second;
                if (!rewritten_.contains(&expression) && originalSpelling && found != names_.references.end())
                    for (const auto id : found->second)
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
                return result;
            }
            void substitute(Node& item, const Bindings& bindings, ModuleId argumentModule, ModuleId into)
            {
                if (item.kind == "Type")
                {
                    const auto found = bindings.find(item.value);
                    if (found != bindings.end() && item.children.empty())
                    {
                        auto replacement = portableType(*found->second, argumentModule, into);
                        const auto range = item.range;
                        item = std::move(*replacement); item.range = range;
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
                else
                    for (std::size_t index = expression.kind == "QualifiedType" ? 1 : 0; index < expression.children.size(); ++index)
                        result.push_back(expression.children[index].get());
                return result;
            }
            std::string key(const Node& type, ModuleId module)
            {
                if (type.kind == "ReferenceType") return "&" + type.value + key(*type.children[0], module);
                auto declared = declaration(type, module);
                if (declared.node && canonicalKeys_.contains(declared.node)) return canonicalKeys_.at(declared.node);
                std::string result = declared.node ? graph_.modules[declared.module]->name + "." + declared.node->value : flatPath(type);
                if (result == "int") result = "i32";
                if (result == "float") result = "f64";
                const auto arguments = typeArguments(type);
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
                if (type.kind == "ReferenceType") return type.value != "mut";
                const auto name = flatPath(type);
                if (name == "string") return false;
                if (name == "Range" && type.children.size() == 1) return copy(*type.children[0], module, visiting);
                if (name == "Slice" && type.children.size() == 1) return true;
                static const std::set<std::string> scalars = {"i8","i16","i32","i64","u8","u16","u32","u64","int","isize","usize","f16","bf16","f32","f64","float","bool","char","byte","unit","never","string_view"};
                if (scalars.contains(name)) return true;
                const auto declared = declaration(type, module);
                if (!declared.node) return false;
                const auto identity = key(type, module);
                if (!visiting.insert(identity).second) return false;
                const auto* implemented = child(*declared.node, "Implements");
                bool answer = false;
                if (implemented)
                    for (const auto& interface : implemented->children) if (interface->value == "Copy") answer = true;
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
            bool constraint(const Node& concrete, ModuleId module, const Node& required)
            {
                if (required.value == "Copy") { std::set<std::string> visiting; return copy(concrete, module, visiting); }
                const auto type = declaration(concrete, module);
                const auto* interfaces = type.node ? child(*type.node, "Implements") : nullptr;
                if (!interfaces) return false;
                for (const auto& interface : interfaces->children)
                    if (key(*interface, type.module) == key(required, module)) return true;
                return false;
            }
            Declaration specialize(const Declaration& original, const std::vector<const Node*>& arguments, ModuleId argumentModule, const Node& site, std::size_t depth)
            {
                if (!original.node) return {};
                const auto* parameters = child(*original.node, "GenericParameters");
                if (!parameters || parameters->children.size() != arguments.size())
                {
                    error(site, "Generic type argument count does not match its declaration."); return {};
                }
                // 구체 소유 타입과 원본 위치를 포함해 복제된 메서드끼리 충돌하지 않게 한다.
                // Include the concrete owner and source location to distinguish cloned methods.
                std::string identity = graph_.modules[original.module]->name + ':'
                    + (original.owner ? key(*use(declarations_.at(original.owner), original.module, "Type", original.node->range), original.module) : std::string{})
                    + ':' + std::to_string(original.node->range.begin.source) + ':' + std::to_string(original.node->range.begin.offset)
                    + ':' + original.node->value + ':';
                for (const auto* argument : arguments) identity += key(*argument, argumentModule) + ';';
                const auto existing = specializations_.find(identity);
                if (existing != specializations_.end()) return existing->second;
                if (generated_ >= 512 || depth > 64)
                { error(site, "Generic specialization limit exceeded."); return {}; }
                Bindings bindings;
                for (std::size_t index = 0; index < arguments.size(); ++index) bindings[parameters->children[index]->value] = clone(*arguments[index]);
                for (const auto& requirement : original.node->children)
                    if (requirement->kind == "Constraint" && requirement->children.size() >= 2)
                    {
                        const auto concrete = bindings.find(requirement->children[0]->value);
                        if (concrete == bindings.end()) { error(*requirement, "A generic constraint must name a type parameter."); return {}; }
                        for (std::size_t index = 1; index < requirement->children.size(); ++index)
                        {
                            auto required = portableType(*requirement->children[index], original.module, argumentModule);
                            substitute(*required, bindings, argumentModule, argumentModule);
                            if (!constraint(*concrete->second, argumentModule, *required))
                            { error(site, "Concrete type '" + key(*concrete->second, argumentModule) + "' does not satisfy implements " + flatPath(*requirement->children[index]) + "."); return {}; }
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
                    for (const auto* argument : arguments) canonical += key(*argument, argumentModule) + ',';
                    canonicalKeys_[concrete.node] = canonical + '>';
                    auto logical = use(original, argumentModule, "Type", site.range);
                    for (const auto* argument : arguments) logical->children.push_back(clone(*argument));
                    logicalTypes_[concrete.node] = {std::move(logical), argumentModule};
                }
                if (original.owner) original.owner->children.push_back(std::move(result));
                else graph_.modules[original.module]->ast->children.push_back(std::move(result));
                index(*concrete.node, original.module, original.owner);
                process(*concrete.node, original.module, {}, nullptr, depth + 1);
                return concrete;
            }
            using Locals = std::map<std::string, const Node*>;
            std::vector<NodePtr> inferred_;
            const Node* localParameterType(const Node& parameter)
            {
                if (parameter.children.empty()) return nullptr;
                if (parameter.kind != "VariadicParameter") return parameter.children[0].get();
                // 가변 인수의 지역 이름은 원소 T가 아니라 대여된 Slice<T>를 나타낸다.
                // A variadic local denotes a borrowed Slice<T>, not one owned element T.
                auto view = node("Type", "Slice", parameter.range);
                view->children.push_back(clone(*parameter.children[0]));
                const auto* result = view.get(); inferred_.push_back(std::move(view)); return result;
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
                for (const auto* kind : {"CompilerRangeResult", "CompilerCollectionResult", "CompilerArrayResult"})
                    if (const auto* marker = child(expression, kind); marker && !marker->children.empty()) return clone(*marker->children[0]);
                if (expression.kind == "CompilerBoxExpression" && !expression.children.empty()) return clone(*expression.children[0]);
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
                    return node("Type", text == "true" || text == "false" ? "bool" : text.starts_with('"') ? "string_view" : text.starts_with('\'') ? "char"
                        : !text.starts_with("0x") && !text.starts_with("0X") && text.find_first_of(".eE") != std::string::npos ? "f64" : "i32", expression.range);
                }
                if (expression.kind == "Name")
                {
                    const auto local = locals.find(expression.value);
                    if (local != locals.end()) return clone(*local->second);
                    const auto origin = origins_.find(&expression);
                    const auto references = names_.references.find(origin == origins_.end() ? &expression : origin->second);
                    if (references != names_.references.end() && references->second.size() == 1)
                    {
                        const auto id = references->second[0];
                        if (id < names_.symbols.size())
                        {
                            const auto* declaration = names_.symbols[id].declaration;
                            if (declaration && (declaration->kind == "Variable" || declaration->kind == "Parameter" || declaration->kind == "PatternBinding") && !declaration->children.empty())
                            {
                                if (declaration->children[0]->kind != "InferredType") return portableType(*declaration->children[0], names_.symbols[id].module, module);
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
                    if (target.node && typeDeclaration(*target.node)) return use(target, module, "Type", expression.range);
                    if (target.node && target.node->kind == "Variant" && target.owner) return use(declarations_.at(target.owner), module, "Type", expression.range);
                    return {};
                }
                if (expression.kind == "Call")
                {
                    if (!expression.children.empty() && expression.children[0]->kind == "Member" && !expression.children[0]->children.empty()
                        && expression.children[0]->children[0]->value == "Range" && expression.children.size() == 3)
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
                    if (expression.children.size() == 1 && flatPath(*expression.children[0]) == "Unit.value") return node("Type", "unit", expression.range);
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
                                if (parameters && arguments.size() == parameters->children.size())
                                    for (std::size_t index = 0; index < arguments.size(); ++index) bindings[parameters->children[index]->value] = clone(*arguments[index]);
                            }
                            if (inferArguments(target, expression, module, locals, owner, bindings)) substitute(*result, bindings, module, module);
                        }
                        return result;
                    }
                    if (expression.children[0]->kind == "Member")
                    {
                        auto receiver = infer(*expression.children[0]->children[0], module, locals, owner, depth + 1);
                        bool dynamic = false;
                        const auto type = receiverDeclaration(std::move(receiver), module, owner, &dynamic);
                        if (type.node)
                            for (const auto& method : type.node->children)
                                if (method->kind == "Function" && method->value == expression.children[0]->value && (!dynamic || !generic(*method)))
                                    return portableType(*method->children[0], type.module, module);
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
                    if (known.node && known.node->kind == "Variant" && known.owner) return use(declarations_.at(known.owner), module, "Type", expression.range);
                    auto receiver = infer(*expression.children[0], module, locals, owner, depth + 1);
                    if (receiver && receiver->kind == "ReferenceType") receiver = std::move(receiver->children[0]);
                    const auto type = receiver && receiver->value == "Self" && owner ? declarations_.at(owner)
                        : receiver ? declaration(*receiver, module) : Declaration{};
                    if (type.node)
                        for (const auto& field : type.node->children)
                            if (field->kind == "Variable" && field->value == expression.value && !field->children.empty())
                                return portableType(*field->children[0], type.module, module);
                }
                return {};
            }
            bool unify(const Node& parameter, const Node& actual, const std::set<std::string>& variables, Bindings& bindings, ModuleId module)
            {
                if (parameter.kind == "Type" && variables.contains(parameter.value) && parameter.children.empty())
                {
                    const auto existing = bindings.find(parameter.value);
                    if (existing == bindings.end()) { bindings[parameter.value] = clone(actual); return true; }
                    return key(*existing->second, module) == key(actual, module);
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
            bool declaresMethod(const Node& type, std::string_view name) const
            {
                return std::any_of(type.children.begin(), type.children.end(), [&](const auto& item) { return item->kind == "Function" && item->value == name; });
            }
            void validateTemplateContract(const Declaration& declaration)
            {
                if (!contractsChecked_.insert(declaration.node).second) return;
                std::set<std::string> variables, copyVariables;
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
                                    auto result = portableType(*field->children[0], target.module, declaration.module);
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
                                : target == "i8" || target == "i16" || target == "i32" || target == "i64" || target == "u8" || target == "u16" || target == "u32" || target == "u64" || target == "isize" || target == "usize";
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
                auto walk = [&](auto&& visit, const Node& item, Locals scope, const Node* parent, std::size_t depth) -> void
                {
                    if (depth > 256) { error(item, "Generic contract traversal limit exceeded."); return; }
                    if (item.kind == "Member" && !item.children.empty())
                    {
                        const auto variable = abstractVariable(*item.children[0], scope);
                        if (!variable.empty() && std::none_of(methods[variable].begin(), methods[variable].end(), [&](const auto& method) { return method->value == item.value; }))
                            error(item, "Generic contract does not declare member '" + item.value + "'; add an interface requirement.");
                    }
                    if (item.kind == "Return" && !item.children.empty() && !declaration.node->children.empty())
                    {
                        const auto& wanted = *declaration.node->children[0];
                        if (wanted.kind == "Type" && variables.contains(wanted.value))
                        {
                            auto actual = abstractType(*item.children[0], scope, 0);
                            if (actual && key(wanted, declaration.module) != key(*actual, declaration.module))
                                error(item, "Generic contract cannot produce an arbitrary type parameter from a concrete return value.");
                        }
                    }
                    if (item.kind == "Binary" && item.children.size() >= 2)
                    {
                        if (!abstractVariable(*item.children[0], scope).empty() || !abstractVariable(*item.children[1], scope).empty())
                            error(item, "Generic contract does not guarantee operator '" + item.value + "' for every permitted type.");
                    }
                    if (item.kind == "Prefix" && !item.children.empty() && item.value != "&" && item.value != "&mut" && item.value != "*")
                        if (!abstractVariable(*item.children[0], scope).empty()) error(item, "Generic contract does not guarantee this unary operator.");
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
                    for (const auto& nested : item.children)
                    {
                        visit(visit, *nested, scope, &item, depth + 1);
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
                if (const auto* body = child(*declaration.node, "Block")) walk(walk, *body, locals, nullptr, 0);
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
                            for (const auto& nested : item.children[index]->children)
                                if (nested->kind == "CaseScope") statements(*nested, branch, scope, depth + 1);
                            merge(state, state, branch);
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
                        const auto concrete = bindings.find(requirement->children[0]->value);
                        if (concrete == bindings.end()) return false;
                        for (std::size_t index = 1; index < requirement->children.size(); ++index)
                        {
                            auto required = portableType(*requirement->children[index], function.module, module);
                            substitute(*required, bindings, module, module);
                            if (!constraint(*concrete->second, module, *required)) return false;
                        }
                    }
                return true;
            }
            void fillExpected(const Node& pattern, const Node& actual, const std::set<std::string>& variables, Bindings& bindings, ModuleId module)
            {
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
                    arguments.emplace_back(parameters->children[parameter]->children[0].get(), argument.children[0].get());
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
                        const bool integral = numeric == "i8" || numeric == "i16" || numeric == "i32" || numeric == "i64" || numeric == "isize" || numeric == "u8" || numeric == "u16" || numeric == "u32" || numeric == "u64" || numeric == "usize";
                        const bool floating = numeric == "f32" || numeric == "f64";
                        if (literalType->value == "f64" ? !floating : !integral) return false;
                        if (literalPreference && numeric == literalType->value) ++*literalPreference;
                    }
                if (expected_ && expected_->kind != "InferredType" && !function.node->children.empty())
                {
                    auto resultType = portableType(*function.node->children[0], function.module, module);
                    fillExpected(*resultType, *expected_, variables, bindings, module);
                }
                if (generics)
                    for (const auto& parameter : generics->children) if (!bindings.contains(parameter->value)) return false;
                return true;
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
            bool rangeResultCall(Node& call, ModuleId module, const Locals& locals, Node* owner, std::size_t depth)
            {
                const auto& callee = *call.children[0];
                if (callee.kind != "Member" || callee.children.empty() || callee.children[0]->kind != "Name" || callee.children[0]->value != "Range"
                    || (callee.value != "with_step" && callee.value != "len")) return false;
                const auto refs = names_.references.find(callee.children[0].get());
                if (refs != names_.references.end() && (refs->second.size() != 1 || names_.symbols[refs->second[0]].declaration)) return false;
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
                if (callee.kind != "Member" || callee.children.empty() || callee.children[0]->kind != "Name"
                    || callee.children[0]->value != "Vector" || (callee.value != "push" && callee.value != "reserve")) return false;
                const auto refs = names_.references.find(callee.children[0].get());
                if (refs != names_.references.end() && (refs->second.size() != 1 || names_.symbols[refs->second[0]].declaration)) return false;
                if (call.children.size() != 3 || call.children[1]->children.empty()) return false;
                auto receiver = infer(*call.children[1]->children[0], module, locals, owner);
                if (!receiver || receiver->kind != "ReferenceType" || receiver->children[0]->value != "Vector" || receiver->children[0]->children.size() != 1) return false;
                auto element = clone(*receiver->children[0]->children[0]);
                for (std::size_t i = 1; i < call.children.size(); ++i)
                {
                    ExpectedGuard context(expected_); expected_ = i == 2 && callee.value == "push" ? element.get() : nullptr;
                    process(*call.children[i], module, locals, owner, depth + 1);
                }
                std::vector<NodePtr> errors;
                if (callee.value == "push") errors.push_back(std::move(element));
                auto errorType = specializeBuiltin(callee.value == "push" ? "InputError" : "CollectionError", std::move(errors), module, call, depth + 1);
                if (!errorType) return true;
                std::vector<NodePtr> arguments; arguments.push_back(node("Type", "unit", call.range)); arguments.push_back(std::move(errorType));
                auto resultType = specializeBuiltin("Result", std::move(arguments), module, call, depth + 1);
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
                const auto origin = origins_.find(&expression);
                const auto evaluated = ConstEvaluator(names_, constants, nodeTypes, symbolTypes).evaluate(origin == origins_.end() ? expression : *origin->second, constants.builtin("usize"));
                if (evaluated.status == ConstStatus::Constant && evaluated.value.kind == ConstantKind::Integer
                    && !evaluated.value.negative && evaluated.value.magnitude > 0 && evaluated.value.magnitude <= 64) return evaluated.value.magnitude;
                if (expression.kind == "Literal" || expression.kind == "ConstantTypeArgument")
                {
                    auto value = expression.value; std::erase(value, '_');
                    try { std::size_t used = 0; const auto result = std::stoull(value, &used, 0); return used == value.size() && result <= 64 ? result : 0; }
                    catch (...) { return 0; }
                }
                if (expression.kind == "Name")
                {
                    const auto origin = origins_.find(&expression);
                    const auto refs = names_.references.find(origin == origins_.end() ? &expression : origin->second);
                    if (refs != names_.references.end() && refs->second.size() == 1)
                    {
                        const auto* source = names_.symbols[refs->second[0]].declaration;
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
                if (method != "new" && method != "into_dyn" && method != "as_ref" && method != "as_mut") return false;
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
                if (item.kind.starts_with("Compiler")) return;
                if (item.kind == "Struct" || item.kind == "Enum") owner = &item;
                if ((item.kind == "Type" || item.kind == "QualifiedType") && !typeArguments(item).empty())
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
                    auto& constructor = *item.children[0];
                    if (constructor.kind == "GenericName")
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
                    if (rangeResultCall(item, module, locals, owner, depth)) return;
                    if (collectionCall(item, module, locals, owner, depth)) return;
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
                    auto& callee = *item.children[0];
                    const bool explicitArguments = callee.kind == "GenericName";
                    const Node* receiver = nullptr;
                    const auto candidates = callCandidates(callee, module, locals, owner, receiver);
                    struct Match { Declaration declaration; Bindings bindings; int literals; bool generic; bool variadic; };
                    std::vector<Match> matches;
                    bool failedRequirements = false;
                    for (const auto& candidate : candidates)
                    {
                        if (candidate.node->kind != "Function") continue;
                        const bool isGeneric = generic(*candidate.node);
                        if (explicitArguments && !isGeneric) continue;
                        Bindings bindings;
                        if (explicitArguments)
                        {
                            const auto arguments = typeArguments(callee);
                            const auto* parameters = child(*candidate.node, "GenericParameters");
                            if (arguments.size() != parameters->children.size()) continue;
                            for (std::size_t index = 0; index < arguments.size(); ++index) bindings[parameters->children[index]->value] = clone(*arguments[index]);
                        }
                        int literals = 0;
                        if (!inferArguments(candidate, item, module, locals, owner, bindings, receiver, &literals)) continue;
                        if (!requirementsHold(candidate, bindings, module)) { failedRequirements = true; continue; }
                        const auto* parameters = child(*candidate.node, "Parameters");
                        const bool variadic = parameters && !parameters->children.empty() && parameters->children.back()->kind == "VariadicParameter";
                        matches.push_back({candidate, std::move(bindings), literals, isGeneric, variadic});
                    }
                    if (!matches.empty())
                    {
                        const auto best = std::max_element(matches.begin(), matches.end(), [](const Match& left, const Match& right)
                        { return left.literals != right.literals ? left.literals < right.literals : left.generic != right.generic ? left.generic && !right.generic : left.variadic && !right.variadic; });
                        const auto count = std::count_if(matches.begin(), matches.end(), [&](const Match& value) { return value.literals == best->literals && value.generic == best->generic && value.variadic == best->variadic; });
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
                        const bool hasTemplate = std::any_of(candidates.begin(), candidates.end(), [](const auto& target) { return target.node->kind == "Function" && generic(*target.node); });
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
                    ExpectedGuard fieldContext(expected_);
                    NodePtr fieldExpected;
                    if (item.kind == "NamedInitializer" && nested.kind == "FieldInitializer" && !item.children.empty())
                    {
                        expected_ = nullptr;
                        const auto target = declaration(*item.children[0], module);
                        if (target.node)
                            for (const auto& field : target.node->children)
                                if (field->kind == "Variable" && field->value == nested.value && !field->children.empty())
                                { fieldExpected = portableType(*field->children[0], target.module, module); expected_ = fieldExpected.get(); }
                    }
                    process(nested, module, locals, owner, depth + 1);
                    if ((item.kind == "Block" || item.kind == "CaseScope" || item.kind == "For") && nested.kind == "Variable" && !nested.children.empty())
                    {
                        if (nested.children[0]->kind != "InferredType") locals[nested.value] = nested.children[0].get();
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
            std::map<std::string, std::string> genericNames(const Node& function) const
            {
                std::map<std::string, std::string> result;
                if (const auto* parameters = child(function, "GenericParameters"))
                    for (std::size_t index = 0; index < parameters->children.size(); ++index) result[parameters->children[index]->value] = "$generic" + std::to_string(index);
                return result;
            }
            std::string normalizedSpelling(const Node& item, const std::map<std::string, std::string>& names) const
            {
                if (const auto found = names.find(item.value); found != names.end() && item.children.empty()) return found->second;
                if (item.kind == "ReferenceType") return "&" + item.value + normalizedSpelling(*item.children[0], names);
                if (item.kind == "QualifiedType" || item.kind == "Member") return normalizedSpelling(*item.children[0], names) + "." + item.value;
                std::string result = item.value == "int" ? "i32" : item.value == "float" ? "f64" : item.value;
                if (!item.children.empty())
                {
                    result += '<';
                    for (const auto& argument : item.children) result += normalizedSpelling(*argument, names) + ',';
                    result += '>';
                }
                return result;
            }
            std::string resultKey(const Node& function) const
            {
                return function.children.empty() ? std::string{} : normalizedSpelling(*function.children[0], genericNames(function));
            }
            std::set<std::string> requirementKeys(const Node& function) const
            {
                std::set<std::string> result;
                const auto names = genericNames(function);
                for (const auto& item : function.children)
                    if (item->kind == "Constraint") result.insert(normalizedSpelling(*item, names));
                return result;
            }
            std::string methodKey(const Node& function) const
            {
                const auto names = genericNames(function);
                std::string result = function.value + '<' + std::to_string(names.size()) + ">(";
                if (const auto* parameters = child(function, "Parameters"))
                    for (const auto& parameter : parameters->children)
                    {
                        result += parameter->kind == "VariadicParameter" ? "variadic:" : "";
                        if (!parameter->children.empty()) result += normalizedSpelling(*parameter->children[0], names);
                        if (const auto* arrays = child(*parameter, "ArraySuffixes")) result += normalizedSpelling(*arrays, names);
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
                        if (implemented->value == "Copy") continue;
                        const auto target = declaration(*implemented, module);
                        if (!target.node || target.node->kind != "Interface")
                        { error(*implemented, "An implements entry must identify an interface."); continue; }
                        for (const auto& method : target.node->children)
                        {
                            if (method->kind != "Function") continue;
                            if (child(*method, "Block")) validateDefaultAccess(*method, *target.node);
                            auto signature = clone(*method);
                            relocate(*signature, target.module, module);
                            substitute(*signature, self, module, module);
                            const auto key = methodKey(*signature);
                            required[key].push_back({std::move(signature), target.module, child(*method, "Block") != nullptr});
                        }
                    }
                std::set<const Node*> acceptedOverrides;
                for (auto& [key, contracts] : required)
                {
                    Node* implementation = nullptr;
                    for (auto& method : owner.children)
                        if (method->kind == "Function" && methodKey(*method) == key) { implementation = method.get(); break; }
                    const auto defaults = std::count_if(contracts.begin(), contracts.end(), [](const Requirement& item) { return item.hasDefault; });
                    for (const auto& requirement : contracts)
                    {
                        if (!contracts[0].function->children.empty() && !requirement.function->children.empty()
                            && resultKey(*contracts[0].function) != resultKey(*requirement.function))
                            error(owner, "Static interfaces require incompatible return types for the same method signature.");
                    }
                    if (implementation)
                    {
                        if (!implementation->children.empty() && !contracts[0].function->children.empty()
                            && resultKey(*implementation) != resultKey(*contracts[0].function))
                            error(*implementation, "Interface implementation return type does not match its contract.");
                        if (requirementKeys(*implementation) != requirementKeys(*contracts[0].function))
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
                        const auto matching = required.find(methodKey(*method));
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
