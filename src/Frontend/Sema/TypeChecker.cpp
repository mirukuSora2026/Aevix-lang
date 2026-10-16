// ========================
//
// Created By : miruku_sora2026
//
// File name : TypeChecker.cpp
//
// Description :
//     기본형, 참조, 정적 배열, 구조체 및 일반 함수의 타입과 호출 호환성을 검사한다.
//     Check types and call compatibility for scalars, references, fixed arrays, structs, and ordinary functions.
//
// ========================

#include "aevix/Frontend/Sema/TypeChecker.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace aevix
{
    namespace
    {
        const Node* child(const Node& node, std::string_view kind)
        {
            for (const auto& item : node.children)
                if (item->kind == kind) return item.get();
            return nullptr;
        }

        bool flag(const Node& node, std::string_view value)
        {
            const auto* modifiers = child(node, "Modifier");
            if (!modifiers) return false;
            const std::string padded = " " + modifiers->value + " ";
            return padded.find(" " + std::string(value) + " ") != std::string::npos;
        }

        const Node* initializer(const Node& node)
        {
            for (std::size_t index = 1; index < node.children.size(); ++index)
            {
                const auto& item = *node.children[index];
                if (item.kind != "ArraySuffixes" && item.kind != "Modifier") return &item;
            }
            return nullptr;
        }

        struct IntegerValue
        {
            std::uint64_t magnitude = 0;
            bool negative = false;
            bool overflow = false;
        };

        // 부호를 별도로 보관하여 가장 작은 음수도 양수 중간값의 넘침 없이 검사한다.
        // Keep the sign separate so the smallest negative integer needs no overflowing positive intermediate.
        IntegerValue integerValue(std::string_view spelling, bool negative = false)
        {
            IntegerValue value{0, negative, false};
            unsigned base = 10;
            std::size_t start = 0;
            if (!spelling.empty() && (spelling.front() == '-' || spelling.front() == '+'))
            {
                value.negative = spelling.front() == '-' ? !negative : negative;
                ++start;
            }
            if (spelling.size() >= start + 2 && spelling[start] == '0')
            {
                const char prefix = spelling[start + 1];
                if (prefix == 'x' || prefix == 'X') { base = 16; start += 2; }
                else if (prefix == 'b' || prefix == 'B') { base = 2; start += 2; }
                else if (prefix == 'o' || prefix == 'O') { base = 8; start += 2; }
            }
            for (std::size_t index = start; index < spelling.size(); ++index)
            {
                const char digit = spelling[index];
                if (digit == '_') continue;
                const unsigned number = digit >= '0' && digit <= '9' ? unsigned(digit - '0')
                    : digit >= 'a' && digit <= 'f' ? unsigned(digit - 'a' + 10)
                    : digit >= 'A' && digit <= 'F' ? unsigned(digit - 'A' + 10) : base;
                if (number >= base || value.magnitude > (std::numeric_limits<std::uint64_t>::max() - number) / base)
                {
                    value.overflow = true;
                    return value;
                }
                value.magnitude = value.magnitude * base + number;
            }
            if (value.magnitude == 0) value.negative = false;
            return value;
        }

        bool floatingSpelling(std::string_view text)
        {
            if (text.starts_with("0x") || text.starts_with("0X")) return false;
            return text.find_first_of(".eE") != std::string_view::npos;
        }

        const Node* numericLiteral(const Node& node)
        {
            if (node.kind == "Group" && !node.children.empty()) return numericLiteral(*node.children[0]);
            if (node.kind == "Prefix" && (node.value == "+" || node.value == "-") && !node.children.empty())
                return numericLiteral(*node.children[0]);
            if (node.kind != "Literal" || node.value.empty()) return nullptr;
            if (node.value[0] < '0' || node.value[0] > '9') return nullptr;
            return &node;
        }

        struct Signature
        {
            SymbolId symbol = InvalidSymbolId;
            const Node* node = nullptr;
            TypeId result = InvalidTypeId;
            std::vector<TypeId> parameters;
            std::vector<const Node*> declarations;
            bool supported = true;
            bool variadic = false;
        };

        class Checker final
        {
        public:
            Checker(Diagnostics& diagnostics, const ModuleGraph& graph, const NameResolution& names)
                : diagnostics_(diagnostics), graph_(graph), names_(names), initialErrors_(diagnostics.size())
            {
                for (const auto& symbol : names.symbols)
                {
                    if (symbol.declaration)
                        declarations_[symbol.declaration] = symbol.id;
                }
                for (const auto& module : graph_.modules)
                {
                    indexOwners(*module->ast, nullptr);
                    indexErased(*module->ast, false);
                }
            }

            TypeCheckResult run()
            {
                // 선언 타입을 먼저 등록하여 모듈 순환과 함수의 앞선 참조를 허용한다.
                // Register declaration types first so module cycles and forward function references are valid.
                for (const auto& symbol : names_.symbols)
                {
                    if (!symbol.declaration) continue;
                    const auto& declaration = *symbol.declaration;
                    if (declaration.kind == "Interface")
                        result_.symbolTypes[symbol.id] = result_.types.dynamic(symbol.id, symbol.name);
                    else if (!erased_.contains(symbol.declaration) && (declaration.kind == "Struct" || declaration.kind == "Enum"))
                        result_.symbolTypes[symbol.id] = result_.types.nominal(symbol.id, symbol.name);
                }
                for (const auto& symbol : names_.symbols)
                    if (symbol.declaration && !erased_.contains(symbol.declaration) && child(*symbol.declaration, "CompilerBox"))
                        for (const auto& field : symbol.declaration->children)
                            if (field->kind == "Variable" && !field->children.empty())
                                result_.symbolTypes[symbol.id] = result_.types.boxed(symbol.id, symbol.name, resolveType(*field->children[0]));
                for (const auto& symbol : names_.symbols)
                {
                    if (!symbol.declaration || erased_.contains(symbol.declaration)) continue;
                    const auto& declaration = *symbol.declaration;
                    if (declaration.kind == "Struct" || declaration.kind == "Enum") checkStruct(declaration);
                    else if (declaration.kind == "Interface")
                        unsupported(declaration, declaration.kind + " type checking");
                }
                for (const auto& symbol : names_.symbols)
                {
                    if (!symbol.declaration || erased_.contains(symbol.declaration)) continue;
                    const auto& declaration = *symbol.declaration;
                    if (declaration.kind == "Function") signature(symbol.id);
                    else if (declaration.kind == "Variable" || declaration.kind == "Parameter")
                    {
                        if (owners_.contains(&declaration) && owners_.at(&declaration) && child(*owners_.at(&declaration), "CompilerBox")) continue;
                        if (!declaration.children.empty() && declaration.children[0]->kind != "InferredType")
                            result_.symbolTypes[symbol.id] = declaredType(declaration);
                    }
                }
                registerEnums();
                registerCarriers();
                for (const auto& module : graph_.modules)
                    for (const auto& declaration : module->ast->children)
                        checkDeclaration(*declaration);
                if (diagnostics_.size() == initialErrors_)
                {
                    TypeLayout layout(result_.types, names_, result_.symbolTypes);
                    std::unordered_set<TypeId> checked;
                    for (const auto& [symbol, type] : result_.symbolTypes)
                    {
                        if (!checked.insert(type).second || result_.types.get(type).kind == TypeKind::Void
                            || result_.types.get(type).kind == TypeKind::Dynamic) continue;
                        const auto storage = layout.layout(type);
                        if (storage.succeeded) result_.layouts[type] = storage.layout;
                        else
                        {
                            const auto* declaration = names_.symbols[symbol].declaration;
                            diagnostics_.push_back({storage.range.isWellFormed() ? storage.range : declaration ? declaration->range : SourceRange{}, storage.message});
                        }
                    }
                }
                for (auto& [call, information] : result_.dynCalls)
                {
                    (void)call;
                    for (const auto& [view, table] : result_.dynViews)
                    {
                        (void)view;
                        if (table.interfaceType == information.interfaceType && information.slot < table.methods.size())
                            information.targets.push_back(table.methods[information.slot]);
                    }
                    for (const auto& [view, table] : result_.boxDynViews)
                    {
                        (void)view;
                        if (table.interfaceType == information.interfaceType && information.slot < table.methods.size())
                            information.targets.push_back(table.methods[information.slot]);
                    }
                    std::sort(information.targets.begin(), information.targets.end());
                    information.targets.erase(std::unique(information.targets.begin(), information.targets.end()), information.targets.end());
                }
                result_.succeeded = diagnostics_.size() == initialErrors_;
                return std::move(result_);
            }

        private:
            Diagnostics& diagnostics_;
            const ModuleGraph& graph_;
            const NameResolution& names_;
            std::size_t initialErrors_;
            TypeCheckResult result_;
            std::unordered_map<const Node*, SymbolId> declarations_;
            std::unordered_map<const Node*, const Node*> owners_;
            std::unordered_set<const Node*> erased_;
            std::unordered_map<SymbolId, Signature> signatures_;
            std::unordered_set<const Node*> reported_;
            std::unordered_set<const Node*> checkedVariables_;
            std::unordered_set<const Node*> checkingVariables_;
            TypeId returnType_ = InvalidTypeId;

            void error(const Node& node, std::string message)
            {
                diagnostics_.push_back({node.range, std::move(message)});
            }

            TypeId unsupported(const Node& node, std::string feature)
            {
                if (reported_.insert(&node).second)
                    error(node, "Unsupported in the initial type checker: " + feature + ".");
                return remember(node, result_.types.error());
            }

            TypeId remember(const Node& node, TypeId type)
            {
                result_.nodeTypes[&node] = type;
                return type;
            }

            bool valid(TypeId type) const { return result_.types.get(type).kind != TypeKind::Error; }
            bool numeric(TypeId type) const
            {
                const auto kind = result_.types.get(type).kind;
                return kind == TypeKind::Integer || kind == TypeKind::Float;
            }
            bool integer(TypeId type) const { return result_.types.get(type).kind == TypeKind::Integer; }

            void indexOwners(const Node& node, const Node* owner)
            {
                owners_[&node] = owner;
                const Node* nested = node.kind == "Struct" || node.kind == "Enum" || node.kind == "Interface" || node.kind == "Variant" ? &node : owner;
                for (const auto& item : node.children) indexOwners(*item, nested);
            }

            void indexErased(const Node& node, bool erased)
            {
                erased = erased || child(node, "CompilerTemplate") || child(node, "CompilerContract") || child(node, "CompilerIntrinsic");
                if (erased) erased_.insert(&node);
                for (const auto& item : node.children) indexErased(*item, erased);
            }

            void registerEnums()
            {
                for (const auto& symbol : names_.symbols)
                {
                    if (!symbol.declaration || symbol.declaration->kind != "Enum"
                        || erased_.contains(symbol.declaration)) continue;
                    std::size_t tag = 0;
                    for (const auto& variant : symbol.declaration->children)
                    {
                        if (variant->kind != "Variant") continue;
                        const auto id = declarationId(*variant);
                        EnumVariantInfo info{id, result_.symbolTypes[symbol.id], tag++, {}, {}};
                        for (const auto& field : variant->children)
                            if (field->kind == "Variable")
                            {
                                const auto fieldId = declarationId(*field);
                                info.fields.push_back(fieldId);
                                info.fieldTypes.push_back(symbolType(fieldId));
                            }
                        result_.enumVariants[id] = std::move(info);
                        result_.symbolTypes[id] = result_.symbolTypes[symbol.id];
                    }
                    if (tag == 0) error(*symbol.declaration, "An enum must declare at least one variant.");
                }
            }

            SymbolId declarationId(const Node& node) const
            {
                const auto found = declarations_.find(&node);
                return found == declarations_.end() ? InvalidSymbolId : found->second;
            }

            std::vector<SymbolId> references(const Node& node) const
            {
                const auto found = names_.references.find(&node);
                if (found != names_.references.end()) return found->second;
                if (node.kind == "Group" || node.kind == "GenericName")
                    if (!node.children.empty()) return references(*node.children[0]);
                return {};
            }

            ConstEvaluation evaluate(const Node& node, TypeId expected = InvalidTypeId)
            {
                return ConstEvaluator(names_, result_.types, result_.nodeTypes, result_.symbolTypes).evaluate(node, expected);
            }

            bool requireConstant(const Node& node, TypeId expected, std::string_view purpose)
            {
                const auto constant = evaluate(node, expected);
                if (constant.status == ConstStatus::Constant)
                {
                    result_.constants[&node] = constant.value;
                    return true;
                }
                if (constant.status == ConstStatus::Invalid)
                    diagnostics_.push_back({constant.range, constant.message});
                else error(node, std::string(purpose));
                return false;
            }

            std::optional<std::size_t> extent(const Node& node)
            {
                const auto constant = evaluate(node, result_.types.builtin("usize"));
                if (constant.status == ConstStatus::Invalid)
                {
                    diagnostics_.push_back({constant.range, constant.message});
                    return std::nullopt;
                }
                if (constant.status == ConstStatus::Constant && constant.value.kind == ConstantKind::Integer
                    && !constant.value.negative && constant.value.magnitude <= std::numeric_limits<std::size_t>::max())
                {
                    result_.constants[&node] = constant.value;
                    return static_cast<std::size_t>(constant.value.magnitude);
                }
                error(node, "A fixed array extent must be a nonnegative compile-time integer constant.");
                return std::nullopt;
            }

            TypeId resolveType(const Node& node)
            {
                if (node.kind == "ReferenceType")
                {
                    const auto element = resolveType(*node.children[0]);
                    if (result_.types.get(element).kind == TypeKind::Void)
                    {
                        error(node, "void cannot be borrowed as a value.");
                        return remember(node, result_.types.error());
                    }
                    return remember(node, result_.types.reference(element, node.value == "mut"));
                }
                if (node.kind == "Type" && (node.value == "Vector" || node.value == "Slice" || node.value == "MutSlice"))
                {
                    if (node.children.size() != 1) { error(node, "Collection type requires one element type."); return result_.types.error(); }
                    const auto element = resolveType(*node.children[0]);
                    if (result_.types.get(element).kind == TypeKind::Void) { error(node, "void cannot be a collection element."); return result_.types.error(); }
                    return remember(node, node.value == "Vector" ? result_.types.vector(element) : result_.types.slice(element, node.value == "MutSlice"));
                }
                if (node.kind == "DynamicType")
                {
                    if (node.children.size() != 1) return unsupported(node, "malformed dynamic interface");
                    const auto ids = references(*node.children[0]);
                    if (ids.size() == 1 && ids[0] < names_.symbols.size() && names_.symbols[ids[0]].declaration
                        && names_.symbols[ids[0]].declaration->kind == "Interface")
                        return remember(node, result_.types.dynamic(ids[0], names_.symbols[ids[0]].name));
                    error(node, "dyn requires one resolved interface type.");
                    return remember(node, result_.types.error());
                }
                if (node.kind == "InferredType") return result_.types.error();
                if (node.kind == "Type" && (node.value == "Range" || node.value == "Slice"))
                {
                    if (node.children.size() != 1) { error(node, node.value + " requires one element type."); return result_.types.error(); }
                    const auto element = resolveType(*node.children[0]);
                    const auto type = node.value == "Range" ? result_.types.range(element) : result_.types.slice(element);
                    if (!valid(type)) error(node, "Invalid element type for " + node.value + ".");
                    return remember(node, type);
                }
                const auto ids = references(node);
                if (node.value == "Self")
                {
                    const auto found = owners_.find(&node);
                    if (found != owners_.end() && found->second)
                    {
                        const auto ownerId = declarationId(*found->second);
                        const auto type = result_.symbolTypes.find(ownerId);
                        if (type != result_.symbolTypes.end()) return remember(node, type->second);
                    }
                }
                if (node.kind == "Type" && node.value == "Array")
                {
                    if (node.children.size() != 2)
                    {
                        error(node, "Array requires an element type and a constant extent.");
                        return remember(node, result_.types.error());
                    }
                    const auto element = resolveType(*node.children[0]);
                    const auto size = extent(*node.children[1]);
                    if (result_.types.get(element).kind == TypeKind::Void)
                    {
                        error(node, "void cannot be an array element.");
                        return remember(node, result_.types.error());
                    }
                    return remember(node, size ? result_.types.array(element, *size) : result_.types.error());
                }
                if (ids.size() == 1 && ids[0] < names_.symbols.size())
                {
                    const auto& symbol = names_.symbols[ids[0]];
                    if (!symbol.declaration)
                    {
                        const auto builtin = result_.types.builtin(symbol.name);
                        if (valid(builtin) && node.children.empty()) return remember(node, builtin);
                        return unsupported(node, "type '" + symbol.name + "'");
                    }
                    if (symbol.declaration->kind == "Struct" || symbol.declaration->kind == "Enum")
                    {
                        const auto* generics = child(*symbol.declaration, "GenericParameters");
                        if ((generics && !generics->children.empty()) || (node.kind == "Type" && !node.children.empty())
                            || (node.kind == "QualifiedType" && node.children.size() > 1))
                            return unsupported(node, "generic struct types");
                        return remember(node, result_.types.nominal(symbol.id, symbol.name));
                    }
                    return unsupported(node, "type '" + symbol.name + "'");
                }
                error(node, "Cannot determine the declared type '" + node.value + "'.");
                return remember(node, result_.types.error());
            }

            TypeId declaredType(const Node& declaration)
            {
                if (declaration.children.empty()) return result_.types.error();
                TypeId type = resolveType(*declaration.children[0]);
                if (result_.types.get(type).kind == TypeKind::Dynamic)
                {
                    error(declaration, "dyn is unsized; use &dyn, &mut dyn, or Box<dyn>.");
                    return remember(declaration, result_.types.error());
                }
                if (declaration.kind == "VariadicParameter") return remember(declaration, result_.types.slice(type));
                if (result_.types.get(type).kind == TypeKind::Void)
                {
                    error(declaration, "void is a return marker and cannot be used for a variable, parameter, or array element.");
                    return remember(declaration, result_.types.error());
                }
                if (const auto* arrays = child(declaration, "ArraySuffixes"))
                {
                    for (auto item = arrays->children.rbegin(); item != arrays->children.rend(); ++item)
                    {
                        if ((*item)->kind != "ArraySuffix" || (*item)->children.empty())
                            return unsupported(**item, "dynamic array declarators");
                        const auto size = extent(*(*item)->children[0]);
                        type = size ? result_.types.array(type, *size) : result_.types.error();
                    }
                }
                if (result_.types.get(type).kind == TypeKind::Void)
                {
                    error(declaration, "void is a return marker and cannot be used for a variable or parameter.");
                    type = result_.types.error();
                }
                return remember(declaration, type);
            }

            void checkStruct(const Node& node)
            {
                if (const auto* parameters = child(node, "GenericParameters"); parameters && !parameters->children.empty())
                    unsupported(node, "generic struct declarations");
                if (const auto* interfaces = child(node, "Implements"))
                    for (const auto& type : interfaces->children)
                        if ((type->value != "Copy" || !type->children.empty())
                            && !child(node, "CompilerInterfacesValidated")) unsupported(*type, "unvalidated interface implementation");
                if (child(node, "Constraint")) unsupported(node, "generic constraints");
                for (const auto& member : node.children)
                    if (member->kind == "Variable" && flag(*member, "persist")) unsupported(*member, "persist struct fields");
            }

            Signature& signature(SymbolId id)
            {
                const auto existing = signatures_.find(id);
                if (existing != signatures_.end()) return existing->second;
                Signature value;
                value.symbol = id;
                if (id >= names_.symbols.size() || !names_.symbols[id].declaration)
                {
                    signatures_[id] = value;
                    return signatures_.at(id);
                }
                value.node = names_.symbols[id].declaration;
                const auto& declaration = *value.node;
                if (const auto* generics = child(declaration, "GenericParameters"); generics && !generics->children.empty())
                {
                    unsupported(declaration, "generic functions");
                    value.supported = false;
                }
                if (child(declaration, "Constraint"))
                {
                    unsupported(declaration, "generic constraints");
                    value.supported = false;
                }
                if (value.supported)
                {
                    value.result = resolveType(*declaration.children[0]);
                    if (const auto* parameters = child(declaration, "Parameters"))
                        for (const auto& parameter : parameters->children)
                        {
                            if (parameter->kind == "VariadicParameter") value.variadic = true;
                            const auto type = declaredType(*parameter);
                            value.parameters.push_back(type);
                            value.declarations.push_back(parameter.get());
                            result_.symbolTypes[declarationId(*parameter)] = type;
                            if (!valid(type)) value.supported = false;
                        }
                }
                result_.symbolTypes[id] = value.supported ? result_.types.function(value.result, value.parameters) : result_.types.error();
                result_.nodeTypes[&declaration] = result_.symbolTypes[id];
                signatures_[id] = std::move(value);
                return signatures_.at(id);
            }

            bool fitsInteger(const Node& node, TypeId type, bool negative = false) const
            {
                const auto* literal = numericLiteral(node);
                if (!literal || floatingSpelling(literal->value)) return false;
                if (node.kind == "Group") return fitsInteger(*node.children[0], type, negative);
                if (node.kind == "Prefix")
                    return fitsInteger(*node.children[0], type, node.value == "-" ? !negative : negative);
                const auto& target = result_.types.get(type);
                if (target.kind != TypeKind::Integer && target.kind != TypeKind::Byte) return false;
                const auto value = integerValue(literal->value, negative);
                if (value.overflow || (value.negative && !target.isSigned && value.magnitude != 0)) return false;
                const auto bits = target.kind == TypeKind::Byte ? 8 : target.bits;
                if (target.isSigned)
                {
                    const auto bound = std::uint64_t{1} << (bits - 1);
                    return value.magnitude <= (value.negative ? bound : bound - 1);
                }
                return bits == 64 || value.magnitude < (std::uint64_t{1} << bits);
            }

            bool fitsFloat(const Node& node, TypeId type) const
            {
                const auto* literal = numericLiteral(node);
                if (!literal || !floatingSpelling(literal->value) || result_.types.get(type).kind != TypeKind::Float) return false;
                std::string spelling;
                for (char character : literal->value) if (character != '_') spelling += character;
                char* end = nullptr;
                const long double value = std::strtold(spelling.c_str(), &end);
                if (!end || *end || !std::isfinite(value)) return false;
                const auto& target = result_.types.get(type);
                const long double maximum = target.name == "f16" ? 65504.0L
                    : target.name == "bf16" ? 3.3895313892515354759e38L
                    : target.bits == 32 ? static_cast<long double>(std::numeric_limits<float>::max())
                    : static_cast<long double>(std::numeric_limits<double>::max());
                return std::fabs(value) <= maximum;
            }

            TypeId literal(const Node& node, TypeId expected, bool negative = false)
            {
                if (node.value == "true" || node.value == "false") return remember(node, result_.types.builtin("bool"));
                if (node.value.starts_with('"')) return remember(node, result_.types.builtin("string_view"));
                if (node.value.starts_with('\'')) return remember(node, result_.types.builtin("char"));
                const bool floating = floatingSpelling(node.value);
                TypeId type = floating ? result_.types.builtin("f64") : result_.types.builtin("i32");
                const auto kind = result_.types.get(expected).kind;
                if ((floating && kind == TypeKind::Float) || (!floating && kind == TypeKind::Integer)) type = expected;
                const bool fits = floating ? fitsFloat(node, type) : fitsInteger(node, type, negative);
                if (!fits)
                {
                    error(node, "Numeric literal is out of range for " + result_.types.display(type) + ".");
                    return remember(node, result_.types.error());
                }
                return remember(node, type);
            }

            bool dynamicReference(TypeId type) const
            {
                const auto& reference = result_.types.get(type);
                return reference.kind == TypeKind::Reference && result_.types.get(reference.element).kind == TypeKind::Dynamic;
            }
            bool implements(TypeId concrete, TypeId contract) const
            {
                const auto* declaration = nominalDeclaration(concrete);
                const auto* interfaces = declaration ? child(*declaration, "Implements") : nullptr;
                if (!interfaces) return false;
                const auto identity = result_.types.get(contract).declaration;
                for (const auto& interface : interfaces->children)
                {
                    const auto ids = references(*interface);
                    if (ids.size() == 1 && ids[0] == identity) return true;
                }
                return false;
            }
            bool dynamicEligible(const Node& method) const
            {
                if (const auto* parameters = child(method, "GenericParameters"); parameters && !parameters->children.empty()) return false;
                const auto* parameters = child(method, "Parameters");
                if (!parameters || parameters->children.empty() || parameters->children[0]->value != "self"
                    || parameters->children[0]->children.empty() || parameters->children[0]->children[0]->kind != "ReferenceType") return false;
                const auto containsSelf = [&](const auto& recur, const Node& item) -> bool
                {
                    if (item.value == "Self") return true;
                    for (const auto& nested : item.children) if (recur(recur, *nested)) return true;
                    return false;
                };
                if (containsSelf(containsSelf, *method.children[0])) return false;
                for (std::size_t index = 1; index < parameters->children.size(); ++index)
                    if (containsSelf(containsSelf, *parameters->children[index])) return false;
                return true;
            }
            std::vector<SymbolId> dynamicMethods(TypeId contract) const
            {
                std::vector<SymbolId> result;
                const auto* declaration = nominalDeclaration(contract);
                if (declaration) for (const auto& member : declaration->children)
                    if (member->kind == "Function" && dynamicEligible(*member)) result.push_back(declarationId(*member));
                return result;
            }
            TypeId adaptDynamic(const Node& node, TypeId actual, TypeId wanted)
            {
                if (!dynamicReference(wanted) || actual == wanted || !compatible(actual, wanted)) return remember(node, actual);
                const auto& source = result_.types.get(actual);
                if (source.kind != TypeKind::Reference) return remember(node, actual);
                DynViewInfo information{actual, result_.types.get(wanted).element, source.element, {}};
                if (result_.types.get(source.element).kind == TypeKind::Dynamic)
                {
                    result_.dynViews[&node] = std::move(information);
                    return remember(node, wanted);
                }
                for (const auto contractMethod : dynamicMethods(information.interfaceType))
                {
                    const auto required = signature(contractMethod);
                    SymbolId selected = InvalidSymbolId;
                    for (const auto candidate : members(source.element, names_.symbols[contractMethod].name))
                    {
                        const auto implementation = signature(candidate);
                        if (!implementation.supported || implementation.parameters.size() != required.parameters.size() || implementation.result != required.result) continue;
                        const auto& requiredSelf = result_.types.get(required.parameters[0]);
                        const auto& concreteSelf = result_.types.get(implementation.parameters[0]);
                        if (concreteSelf.kind != TypeKind::Reference || concreteSelf.element != source.element || concreteSelf.isMutable != requiredSelf.isMutable) continue;
                        bool matches = true;
                        for (std::size_t index = 1; index < required.parameters.size(); ++index)
                            if (required.parameters[index] != implementation.parameters[index]) matches = false;
                        if (matches) selected = candidate;
                    }
                    if (selected == InvalidSymbolId) error(node, "No concrete implementation exists for a dynamic interface slot.");
                    information.methods.push_back(selected);
                }
                result_.dynViews[&node] = std::move(information);
                return remember(node, wanted);
            }
            bool compatible(TypeId actual, TypeId wanted) const
            {
                if (!valid(actual) || !valid(wanted)) return true;
                if (actual == wanted || result_.types.get(actual).kind == TypeKind::Never) return true;
                const auto& source = result_.types.get(actual);
                const auto& target = result_.types.get(wanted);
                return dynamicReference(wanted) && source.kind == TypeKind::Reference
                    && (!target.isMutable || source.isMutable)
                    && (source.element == target.element || implements(source.element, target.element));
            }

            void require(TypeId actual, TypeId wanted, const Node& node, std::string_view purpose)
            {
                if (!compatible(actual, wanted))
                    error(node, std::string(purpose) + " requires " + result_.types.display(wanted)
                        + ", but received " + result_.types.display(actual) + ".");
            }

            TypeId symbolType(SymbolId id)
            {
                if (id >= names_.symbols.size()) return result_.types.error();
                const auto found = result_.symbolTypes.find(id);
                if (found != result_.symbolTypes.end()) return found->second;
                const auto* declaration = names_.symbols[id].declaration;
                if (!declaration) return result_.types.error();
                if (declaration->kind == "Function") return result_.symbolTypes[id] = signature(id).supported ? result_.types.function(signature(id).result, signature(id).parameters) : result_.types.error();
                if (declaration->kind == "Variable") return checkVariable(*declaration);
                return result_.types.error();
            }

            bool lvalue(const Node& node)
            {
                if (node.kind == "Group") return lvalue(*node.children[0]);
                if (node.kind == "Prefix" && node.value == "*") return true;
                if (node.kind == "Index") return lvalue(*node.children[0]);
                if (node.kind == "Member")
                {
                    const auto ids = references(node);
                    if (ids.size() == 1 && ids[0] < names_.symbols.size() && names_.symbols[ids[0]].kind == SymbolKind::Value) return true;
                    return !node.children.empty() && lvalue(*node.children[0]);
                }
                if (node.kind == "Name")
                {
                    const auto ids = references(node);
                    return ids.size() == 1 && ids[0] < names_.symbols.size() && names_.symbols[ids[0]].kind == SymbolKind::Value;
                }
                return false;
            }

            bool writable(const Node& node)
            {
                if (node.kind == "Group") return writable(*node.children[0]);
                if (node.kind == "Prefix" && node.value == "*")
                {
                    const auto type = expression(*node.children[0]);
                    const auto& reference = result_.types.get(type);
                    return reference.kind == TypeKind::Reference && reference.isMutable;
                }
                if (node.kind == "Member" || node.kind == "Index")
                {
                    const auto ids = references(node);
                    if (node.kind == "Member" && ids.size() == 1 && ids[0] < names_.symbols.size())
                    {
                        const auto* declaration = names_.symbols[ids[0]].declaration;
                        if (declaration && !owners_.at(declaration)) return flag(*declaration, "mut");
                    }
                    if (node.children.empty()) return false;
                    const auto type = expression(*node.children[0]);
                    const auto& reference = result_.types.get(type);
                    if (reference.kind == TypeKind::Slice) return reference.isMutable;
                    if (reference.kind == TypeKind::Reference) return reference.isMutable;
                    return writable(*node.children[0]);
                }
                const auto ids = references(node);
                if (ids.size() != 1 || ids[0] >= names_.symbols.size()) return false;
                const auto* declaration = names_.symbols[ids[0]].declaration;
                return declaration && flag(*declaration, "mut");
            }

            void requireWritable(const Node& node)
            {
                if (!lvalue(node)) error(node, "Modification requires an assignable place.");
                else if (!writable(node)) error(node, "Cannot modify an immutable binding or a read-only borrow; use mut or &mut.");
            }

            const Node* nominalDeclaration(TypeId type) const
            {
                const auto& item = result_.types.get(type);
                if (item.kind == TypeKind::Reference || item.kind == TypeKind::Box) return nominalDeclaration(item.element);
                if ((item.kind != TypeKind::Nominal && item.kind != TypeKind::Dynamic) || item.declaration >= names_.symbols.size()) return nullptr;
                return names_.symbols[item.declaration].declaration;
            }

            std::vector<SymbolId> members(TypeId type, std::string_view name)
            {
                std::vector<SymbolId> values;
                const auto* declaration = nominalDeclaration(type);
                if (!declaration) return values;
                for (const auto& member : declaration->children)
                    if (member->value == name && (member->kind == "Variable" || member->kind == "Function"))
                    {
                        const auto id = declarationId(*member);
                        if (id != InvalidSymbolId) values.push_back(id);
                    }
                return values;
            }

            bool visibleMember(const Node& use, const Node& declaration)
            {
                if (flag(declaration, "public")) return true;
                const auto owner = owners_.find(&use);
                const auto declaredOwner = owners_.find(&declaration);
                if (owner != owners_.end() && declaredOwner != owners_.end())
                    for (auto enclosing = declaredOwner->second; enclosing;)
                    {
                        if (owner->second == enclosing) return true;
                        const auto parent = owners_.find(enclosing);
                        enclosing = parent == owners_.end() ? nullptr : parent->second;
                    }
                error(use, "Cannot access private member '" + declaration.value + "'.");
                return false;
            }

            TypeId memberExpression(const Node& node)
            {
                const auto ids = references(node);
                if (!ids.empty())
                {
                    if (ids.size() > 1)
                    {
                        error(node, "An overloaded function needs a call to select a signature.");
                        return remember(node, result_.types.error());
                    }
                    const auto* declaration = names_.symbols[ids[0]].declaration;
                    if (declaration && declaration->kind == "Variant")
                        return variantValue(node, ids[0]);
                    if (declaration && declaration->kind == "Variable" && owners_.at(declaration))
                    {
                        error(node, "An instance field requires a value receiver; a type name cannot read the field.");
                        return remember(node, result_.types.error());
                    }
                    return remember(node, symbolType(ids[0]));
                }
                if (node.children.empty()) return result_.types.error();
                const auto receiver = expression(*node.children[0]);
                if (!valid(receiver)) return remember(node, receiver);
                const auto candidates = members(receiver, node.value);
                if (candidates.empty())
                {
                    error(node, "Type " + result_.types.display(receiver) + " has no supported member '" + node.value + "'.");
                    return remember(node, result_.types.error());
                }
                if (candidates.size() != 1 || names_.symbols[candidates[0]].kind != SymbolKind::Value)
                {
                    error(node, "An instance method requires a call.");
                    return remember(node, result_.types.error());
                }
                const auto* declaration = names_.symbols[candidates[0]].declaration;
                visibleMember(node, *declaration);
                return remember(node, symbolType(candidates[0]));
            }

            void checkCollectionLiteral(const Node& node, TypeId expected)
            {
                const auto collection = result_.types.get(expected);
                if (collection.kind != TypeKind::Array && collection.kind != TypeKind::Vector)
                { error(node, "Nested array literal requires an Array or Vector context."); return; }
                result_.collectionElements[&node] = expected;
                std::size_t count = 0;
                for (const auto& item : node.children)
                {
                    if (item->kind == "CompilerArrayResult") continue;
                    ++count;
                    if (item->kind == "ArrayLiteral") checkCollectionLiteral(*item, collection.element);
                    else require(expression(*item, collection.element), collection.element, *item, "Collection element");
                }
                if (collection.kind == TypeKind::Array && count != collection.extent)
                    error(node, "Fixed array initializer length does not match its declared extent.");
                remember(node, expected);
            }

            TypeId arrayLiteral(const Node& node, TypeId expected)
            {
                if (const auto* marker = child(node, "CompilerArrayResult"))
                {
                    const auto result = resolveType(*marker->children[0]);
                    const auto found = result_.carriers.find(result);
                    if (found == result_.carriers.end()) { error(node, "Missing dynamic collection Result specialization."); return result_.types.error(); }
                    const auto collection = found->second.value;
                    checkCollectionLiteral(node, collection);
                    result_.dynamicLiterals[&node] = result;
                    return remember(node, result);
                }
                const auto target = result_.types.get(expected);
                if (target.kind != TypeKind::Array) return unsupported(node, "array literals without a fixed Array context");
                if (node.children.size() != target.extent)
                    error(node, "Fixed array initializer length does not match its declared extent.");
                for (const auto& item : node.children)
                    require(expression(*item, target.element), target.element, *item, "Array element");
                return remember(node, expected);
            }

            bool copyType(TypeId type, std::unordered_set<TypeId>& visiting)
            {
                const auto& item = result_.types.get(type);
                if (item.kind == TypeKind::String || item.kind == TypeKind::Array || item.kind == TypeKind::Vector || item.kind == TypeKind::Box || item.kind == TypeKind::Void) return false;
                if (item.kind == TypeKind::Reference || item.kind == TypeKind::Slice) return !item.isMutable;
                if (item.kind != TypeKind::Nominal) return item.kind != TypeKind::Error;
                const auto* declaration = nominalDeclaration(type);
                const auto* implemented = declaration ? child(*declaration, "Implements") : nullptr;
                if (!implemented || !visiting.insert(type).second) return false;
                bool marker = false;
                for (const auto& interface : implemented->children) marker = marker || interface->value == "Copy";
                if (marker)
                    for (const auto& field : declaration->children)
                    {
                        if (field->kind == "Variable" && !copyType(symbolType(declarationId(*field)), visiting)) marker = false;
                        if (field->kind == "Variant")
                            for (const auto& payload : field->children)
                                if (payload->kind == "Variable" && !copyType(symbolType(declarationId(*payload)), visiting)) marker = false;
                    }
                visiting.erase(type);
                return marker;
            }

            TypeId checkVariable(const Node& declaration)
            {
                const auto id = declarationId(declaration);
                if (checkedVariables_.contains(&declaration))
                {
                    const auto found = result_.nodeTypes.find(&declaration);
                    return found == result_.nodeTypes.end() ? result_.types.error() : found->second;
                }
                if (!checkingVariables_.insert(&declaration).second)
                {
                    error(declaration, "Cyclic variable type inference or initialization dependency.");
                    return result_.types.error();
                }
                const auto* initial = initializer(declaration);
                TypeId type;
                if (!declaration.children.empty() && declaration.children[0]->kind == "InferredType")
                {
                    type = initial ? expression(*initial) : result_.types.error();
                    if (result_.types.get(type).kind == TypeKind::Void)
                    {
                        error(declaration, "rsn cannot infer a variable type from a void expression.");
                        type = result_.types.error();
                    }
                }
                else
                {
                    const auto found = result_.symbolTypes.find(id);
                    type = found != result_.symbolTypes.end() ? found->second : declaredType(declaration);
                    if (initial) require(expression(*initial, type), type, *initial, "Variable initializer");
                }
                if (flag(declaration, "const") && initial)
                {
                    requireConstant(*initial, type, "const requires a compile-time constant expression.");
                }
                checkingVariables_.erase(&declaration);
                checkedVariables_.insert(&declaration);
                if (id != InvalidSymbolId) result_.symbolTypes[id] = type;
                return remember(declaration, type);
            }

            TypeId call(const Node& node)
            {
                const auto& callee = *node.children[0];
                if (callee.kind == "GenericName") return unsupported(node, "explicit generic function calls");
                auto candidates = references(callee);
                const Node* receiver = nullptr;
                TypeId receiverType = result_.types.error();
                if (candidates.empty() && callee.kind == "Member" && !callee.children.empty())
                {
                    receiver = callee.children[0].get();
                    receiverType = expression(*receiver);
                    candidates = members(receiverType, callee.value);
                }
                const auto receiverBase = result_.types.get(receiverType).kind == TypeKind::Reference ? result_.types.get(receiverType).element : receiverType;
                const bool boxReceiver = result_.types.get(receiverBase).kind == TypeKind::Box;
                const auto receiverPayload = boxReceiver ? result_.types.get(receiverBase).element : receiverBase;
                const bool dynamicCall = dynamicReference(receiverType) || (boxReceiver && result_.types.get(receiverPayload).kind == TypeKind::Dynamic);
                if (dynamicCall)
                {
                    std::erase_if(candidates, [&](SymbolId candidate)
                    {
                        return candidate >= names_.symbols.size() || !names_.symbols[candidate].declaration
                            || !dynamicEligible(*names_.symbols[candidate].declaration);
                    });
                    if (candidates.empty())
                    {
                        error(node, "This method requires the concrete Self type and cannot be called through dyn.");
                        return remember(node, result_.types.error());
                    }
                }
                std::vector<TypeId> arguments(node.children.size() - 1, result_.types.error());
                for (std::size_t index = 1; index < node.children.size(); ++index)
                {
                    const auto& value = *node.children[index]->children[0];
                    if (!numericLiteral(value)) arguments[index - 1] = expression(value);
                }
                struct Match { SymbolId symbol; int preference; std::vector<std::size_t> binding; bool receiver; bool variadic; };
                std::vector<Match> matches;
                bool supportedCandidate = false;
                for (const auto candidate : candidates)
                {
                    if (candidate >= names_.symbols.size()) continue;
                    const auto* declaration = names_.symbols[candidate].declaration;
                    if (declaration && erased_.contains(declaration) && !dynamicCall) continue;
                    if (!declaration || declaration->kind != "Function") continue;
                    const auto function = signature(candidate);
                    if (!function.supported) continue;
                    supportedCandidate = true;
                    const bool instance = !function.declarations.empty() && function.declarations[0]->value == "self";
                    if (receiver && !instance) continue;
                    const std::size_t offset = receiver ? 1 : 0;
                    const auto pack = function.variadic ? function.parameters.size() - 1 : function.parameters.size();
                    std::vector<bool> assigned(function.parameters.size(), false);
                    if (receiver)
                    {
                        const auto wanted = function.parameters[0];
                        const auto& target = result_.types.get(wanted);
                        bool accepts = receiverType == wanted;
                        if (target.kind == TypeKind::Reference)
                        {
                            const auto& actual = result_.types.get(receiverType);
                            auto base = actual.kind == TypeKind::Reference ? actual.element : receiverType;
                            if (result_.types.get(base).kind == TypeKind::Box) base = result_.types.get(base).element;
                            accepts = base == target.element && (!target.isMutable
                                || (actual.kind == TypeKind::Reference ? actual.isMutable : writable(*receiver)));
                            if (actual.kind != TypeKind::Reference) accepts = accepts && lvalue(*receiver);
                        }
                        if (!accepts) continue;
                        assigned[0] = true;
                    }
                    std::size_t positional = offset;
                    bool accepts = true;
                    int preference = 0;
                    std::vector<std::size_t> binding;
                    for (std::size_t index = 1; index < node.children.size(); ++index)
                    {
                        const auto& argument = *node.children[index];
                        const auto& value = *argument.children[0];
                        std::size_t parameter = positional++;
                        if (!argument.value.empty())
                        {
                            parameter = function.declarations.size();
                            for (std::size_t slot = offset; slot < function.declarations.size(); ++slot)
                                if (function.declarations[slot]->value == argument.value) parameter = slot;
                        }
                        if (function.variadic && argument.value.empty() && parameter >= pack) parameter = pack;
                        if (parameter >= assigned.size() || (assigned[parameter] && parameter != pack)
                            || (function.variadic && parameter == pack && !argument.value.empty())) { accepts = false; break; }
                        assigned[parameter] = true;
                        binding.push_back(parameter);
                        const auto wanted = function.variadic && parameter == pack ? result_.types.get(function.parameters[parameter]).element : function.parameters[parameter];
                        if (numericLiteral(value))
                        {
                            if (!fitsInteger(value, wanted) && !fitsFloat(value, wanted)) { accepts = false; break; }
                            const bool floating = floatingSpelling(numericLiteral(value)->value);
                            if (wanted == result_.types.builtin(floating ? "f64" : "i32")) ++preference;
                        }
                        else if (!compatible(arguments[index - 1], wanted)) { accepts = false; break; }
                    }
                    for (std::size_t slot = offset; slot < pack; ++slot)
                        if (!assigned[slot] && !child(*function.declarations[slot], "Default")) accepts = false;
                    if (accepts) matches.push_back({candidate, preference, std::move(binding), receiver != nullptr, function.variadic});
                }
                if (matches.empty())
                {
                    if (!candidates.empty() && !supportedCandidate)
                        return unsupported(node, "this callable or library intrinsic");
                    error(node, "No matching overload for this call; check argument types, names, count, and receiver permissions.");
                    return remember(node, result_.types.error());
                }
                const auto best = std::max_element(matches.begin(), matches.end(), [](const Match& left, const Match& right)
                    { return left.preference != right.preference ? left.preference < right.preference : left.variadic && !right.variadic; });
                if (std::count_if(matches.begin(), matches.end(), [&](const Match& value) { return value.preference == best->preference && value.variadic == best->variadic; }) != 1)
                {
                    error(node, "Ambiguous function call: multiple overloads are equally suitable.");
                    return remember(node, result_.types.error());
                }
                const auto function = signature(best->symbol);
                visibleCallMember(callee, *function.node, receiver != nullptr);
                for (std::size_t index = 1; index < node.children.size(); ++index)
                {
                    const auto& value = *node.children[index]->children[0];
                    const auto slot = best->binding[index - 1];
                    const auto wanted = function.variadic && slot + 1 == function.parameters.size()
                        ? result_.types.get(function.parameters[slot]).element : function.parameters[slot];
                    expression(value, wanted);
                    remember(*node.children[index], wanted);
                }
                if (dynamicCall)
                {
                    const auto interfaceType = boxReceiver ? receiverPayload : result_.types.get(receiverType).element;
                    const auto methods = dynamicMethods(interfaceType);
                    const auto found = std::find(methods.begin(), methods.end(), best->symbol);
                    result_.dynCalls[&node] = {interfaceType, static_cast<std::size_t>(found - methods.begin()), {}};
                }
                result_.calls[&node] = best->symbol;
                if (function.variadic)
                {
                    const auto count = std::count(best->binding.begin(), best->binding.end(), function.parameters.size() - 1);
                    result_.variadicPacks[&node] = result_.types.array(result_.types.get(function.parameters.back()).element, static_cast<std::size_t>(count));
                }
                remember(callee, result_.symbolTypes[best->symbol]);
                return remember(node, function.result);
            }

            void visibleCallMember(const Node& callee, const Node& declaration, bool instance)
            {
                if (instance) visibleMember(callee, declaration);
            }

            TypeId variantValue(const Node& node, SymbolId variant)
            {
                const auto found = result_.enumVariants.find(variant);
                if (found == result_.enumVariants.end()) return result_.types.error();
                if (!found->second.fields.empty())
                    error(node, "A data-bearing enum variant requires a named payload initializer.");
                result_.enumConstructors[&node] = variant;
                return remember(node, found->second.type);
            }

            TypeId enumInitializer(const Node& node, SymbolId variant)
            {
                const auto found = result_.enumVariants.find(variant);
                if (found == result_.enumVariants.end()) return result_.types.error();
                const auto& info = found->second;
                std::set<SymbolId> assigned;
                for (std::size_t i = 1; i < node.children.size(); ++i)
                {
                    const auto& field = *node.children[i];
                    auto candidate = std::find_if(info.fields.begin(), info.fields.end(), [&](SymbolId id)
                        { return names_.symbols[id].name == field.value; });
                    if (candidate == info.fields.end())
                    {
                        error(field, "Unknown enum payload field '" + field.value + "'.");
                        continue;
                    }
                    if (!assigned.insert(*candidate).second) error(field, "Duplicate enum payload field.");
                    const auto wanted = symbolType(*candidate);
                    visibleMember(field, *names_.symbols[*candidate].declaration);
                    require(expression(*field.children[0], wanted), wanted, field, "Enum payload");
                    remember(field, wanted);
                }
                for (auto field : info.fields)
                    if (!assigned.contains(field)) error(node, "Missing enum payload field '" + names_.symbols[field].name + "'.");
                result_.enumConstructors[&node] = variant;
                remember(*node.children[0], info.type);
                return remember(node, info.type);
            }

            void enumSwitch(const Node& node, TypeId selector)
            {
                const auto& selected = result_.types.get(selector);
                const bool borrowed = selected.kind == TypeKind::Reference;
                const bool writable = borrowed && selected.isMutable;
                const bool consuming = node.value == "move";
                const auto type = borrowed ? selected.element : selector;
                if (consuming && borrowed) error(node, "A consuming switch requires an owned enum value.");
                std::set<SymbolId> covered;
                bool defaultSeen = false;
                for (std::size_t i = 1; i < node.children.size(); ++i)
                {
                    const auto& branch = *node.children[i];
                    if (branch.kind == "Default") defaultSeen = true;
                    else if (!branch.children.empty())
                    {
                        const auto ids = references(*branch.children[0]);
                        const auto found = ids.size() == 1 ? result_.enumVariants.find(ids[0]) : result_.enumVariants.end();
                        if (found == result_.enumVariants.end() || found->second.type != type)
                            error(branch, "A case must name a variant of the selected enum.");
                        else
                        {
                            const auto& variant = found->second;
                            if (!covered.insert(ids[0]).second) error(branch, "Duplicate enum case.");
                            result_.enumCases[&branch] = ids[0];
                            remember(*branch.children[0], type);
                            if (const auto* bindings = child(branch, "PatternBindings"))
                                for (const auto& binding : bindings->children)
                                {
                                    const auto* fieldNode = child(*binding, "PatternField");
                                    const auto fieldIds = fieldNode ? references(*fieldNode) : std::vector<SymbolId>{};
                                    if (fieldIds.size() != 1 || std::find(variant.fields.begin(), variant.fields.end(), fieldIds[0]) == variant.fields.end())
                                    {
                                        error(*binding, "Cannot resolve the enum payload binding.");
                                        continue;
                                    }
                                    const auto fieldType = symbolType(fieldIds[0]);
                                    const auto bindingType = declaredType(*binding);
                                    const auto& bound = result_.types.get(bindingType);
                                    const bool explicitBorrow = fieldNode->value.starts_with("&");
                                    const bool explicitMutable = fieldNode->value.starts_with("&mut ");
                                    if (bound.kind == TypeKind::Reference)
                                    {
                                        require(fieldType, bound.element, *binding, "Borrowed payload binding");
                                        if (bound.isMutable && !writable) error(*binding, "A mutable payload binding requires switch (&mut value).");
                                        if (explicitMutable && !bound.isMutable) error(*binding, "An explicit mutable payload loan needs a mutable reference binding.");
                                        if (consuming) error(*binding, "A consuming switch cannot return loans into the consumed enum.");
                                    }
                                    else
                                    {
                                        require(fieldType, bindingType, *binding, "Payload binding");
                                        if (explicitBorrow) error(*binding, "An explicit payload loan requires a reference binding.");
                                        std::unordered_set<TypeId> visiting;
                                        if (!consuming && !copyType(fieldType, visiting))
                                            error(*binding, "A non-Copy payload must be borrowed or extracted by a consuming switch.");
                                    }
                                    result_.symbolTypes[declarationId(*binding)] = bindingType;
                                    remember(*binding, bindingType);
                                    result_.enumBindings[binding.get()] = {ids[0], fieldIds[0], consuming, writable};
                                }
                        }
                    }
                    if (const auto* scope = child(branch, "CaseScope")) statement(*scope);
                }
                std::size_t variants = 0;
                for (const auto& [id, info] : result_.enumVariants) if (info.type == type) { (void)id; ++variants; }
                if (!defaultSeen && covered.size() != variants) error(node, "Enum switch must cover every variant or provide default.");
                else result_.exhaustiveSwitches.insert(&node);
            }

            TypeId namedInitializer(const Node& node)
            {
                const auto& name = *node.children[0];
                if (name.kind == "GenericName") return unsupported(node, "generic struct initialization");
                const auto ids = references(name);
                if (ids.size() != 1 || ids[0] >= names_.symbols.size())
                {
                    error(node, "A named initializer requires one concrete struct type.");
                    return remember(node, result_.types.error());
                }
                const auto* declaration = names_.symbols[ids[0]].declaration;
                if (declaration && declaration->kind == "Variant")
                    return enumInitializer(node, ids[0]);
                if (!declaration || declaration->kind != "Struct") return unsupported(node, "non-struct named initializers");
                if (const auto* generics = child(*declaration, "GenericParameters"); generics && !generics->children.empty())
                    return unsupported(node, "generic struct initialization");
                const auto type = result_.types.nominal(ids[0], declaration->value);
                std::set<std::string> assigned;
                for (std::size_t index = 1; index < node.children.size(); ++index)
                {
                    const auto& field = *node.children[index];
                    assigned.insert(field.value);
                    const auto fields = members(type, field.value);
                    if (fields.size() != 1 || names_.symbols[fields[0]].kind != SymbolKind::Value)
                    {
                        error(field, "Unknown struct field '" + field.value + "'.");
                        expression(*field.children[0]);
                        continue;
                    }
                    const auto* fieldDeclaration = names_.symbols[fields[0]].declaration;
                    visibleMember(field, *fieldDeclaration);
                    const auto wanted = symbolType(fields[0]);
                    require(expression(*field.children[0], wanted), wanted, field, "Field initializer");
                    remember(field, wanted);
                }
                for (const auto& field : declaration->children)
                    if (field->kind == "Variable" && !assigned.contains(field->value) && !initializer(*field))
                        error(node, "Missing struct field '" + field->value + "' in initializer.");
                remember(name, type);
                return remember(node, type);
            }

            void registerCarriers()
            {
                for (const auto& [symbol, type] : result_.symbolTypes)
                {
                    if (symbol >= names_.symbols.size() || result_.types.get(type).kind != TypeKind::Nominal) continue;
                    const auto* declaration = names_.symbols[symbol].declaration;
                    const auto* marker = declaration ? child(*declaration, "CompilerCarrier") : nullptr;
                    if (!marker) continue;
                    CoreCarrierInfo info; info.optional = marker->value == "Optional"; info.type = type;
                    for (const auto& [variant, description] : result_.enumVariants)
                    {
                        if (description.type != type) continue;
                        if (names_.symbols[variant].name == (info.optional ? "Some" : "Ok"))
                        { info.success = variant; if (!description.fieldTypes.empty()) info.value = description.fieldTypes[0]; }
                        else if (names_.symbols[variant].name == (info.optional ? "None" : "Err"))
                        { info.failure = variant; if (!description.fieldTypes.empty()) info.error = description.fieldTypes[0]; }
                    }
                    if (info.success != InvalidSymbolId && info.failure != InvalidSymbolId) result_.carriers[type] = info;
                }
            }
            std::optional<TypeId> corePredicate(const Node& node)
            {
                if (node.children.empty()) return std::nullopt;
                const Node* callee = node.children[0].get();
                const bool explicitArguments = callee->kind == "GenericName";
                if (callee->kind == "GenericName" && !callee->children.empty()) callee = callee->children[0].get();
                if (callee->kind != "Member") return std::nullopt;
                const auto candidates = references(*callee);
                if (candidates.size() != 1 || candidates[0] >= names_.symbols.size()) return std::nullopt;
                const auto* declaration = names_.symbols[candidates[0]].declaration;
                if (!declaration || !child(*declaration, "CompilerIntrinsic")) return std::nullopt;
                const auto owner = owners_.find(declaration);
                const auto* marker = owner != owners_.end() && owner->second ? child(*owner->second, "CompilerCarrier") : nullptr;
                if (!marker) return std::nullopt;
                const bool optional = marker->value == "Optional";
                if (explicitArguments)
                { error(node, "Carrier predicates infer their types from the borrowed input and do not accept explicit type arguments."); return result_.types.error(); }
                if (callee->value != (optional ? "is_some" : "is_ok") && callee->value != (optional ? "is_none" : "is_err")) return std::nullopt;
                if (node.children.size() != 2 || node.children[1]->children.empty())
                { error(node, "A carrier predicate requires one borrowed value."); return result_.types.error(); }
                if (!node.children[1]->value.empty() && node.children[1]->value != "value") error(node, "Unknown carrier predicate argument name.");
                const auto actual = expression(*node.children[1]->children[0]);
                const auto& borrowed = result_.types.get(actual);
                const auto carrier = borrowed.kind == TypeKind::Reference ? result_.carriers.find(borrowed.element) : result_.carriers.end();
                if (carrier == result_.carriers.end() || carrier->second.optional != optional)
                { error(node, "A carrier predicate requires an explicit borrow of its Result or Optional value."); return result_.types.error(); }
                result_.predicates[&node] = {borrowed.element, callee->value == (optional ? "is_some" : "is_ok")};
                return result_.types.builtin("bool");
            }
            std::optional<TypeId> collectionIntrinsic(const Node& node, TypeId expected)
            {
                const auto* callee = node.children.empty() ? nullptr : node.children[0].get();
                if (!callee || callee->kind != "Member" || callee->children.size() != 1 || callee->children[0]->kind != "Name") return std::nullopt;
                const auto& owner = *callee->children[0];
                const auto ids = references(owner);
                if (ids.size() != 1 || names_.symbols[ids[0]].declaration) return std::nullopt;
                if (owner.value == "Slice" && callee->value == "len")
                {
                    if (node.children.size() != 2) { error(node, "Slice.len requires one view."); return result_.types.error(); }
                    auto actual = expression(*node.children[1]->children[0]);
                    if (result_.types.get(actual).kind == TypeKind::Reference) actual = result_.types.get(actual).element;
                    if (result_.types.get(actual).kind != TypeKind::Slice) { error(node, "Slice.len requires a Slice view."); return result_.types.error(); }
                    result_.intrinsics[&node] = "slice.len"; return result_.types.builtin("usize");
                }
                if (owner.value != "Range") return std::nullopt;
                const auto& method = callee->value;
                if (const auto* marker = child(node, "CompilerRangeResult"))
                {
                    const auto resolved = resolveType(*marker->children[0]);
                    const auto carrier = result_.carriers.find(resolved);
                    const auto context = method == "with_step" && carrier != result_.carriers.end() ? carrier->second.value : InvalidTypeId;
                    const auto actual = expression(*node.children[1]->children[0], context);
                    const auto& argument = result_.types.get(actual);
                    const auto descriptor = method == "len" && argument.kind == TypeKind::Reference ? argument.element : actual;
                    const auto& range = result_.types.get(descriptor);
                    if (range.kind != TypeKind::Range || (method == "len" && argument.kind != TypeKind::Reference))
                    { error(node, "Range.len requires an explicit Range borrow; with_step requires a Range value."); return result_.types.error(); }
                    if (!node.children[1]->value.empty()) error(node, "Core Range operations require positional arguments.");
                    if (method == "with_step")
                    {
                        const auto& element = result_.types.get(range.element);
                        const auto magnitude = result_.types.builtin(element.name == "isize" || element.name == "usize" ? "usize" : "u" + std::to_string(element.bits));
                        require(expression(*node.children[2]->children[0], magnitude), magnitude, node, "Range step magnitude");
                        if (!node.children[2]->value.empty()) error(node, "Core Range operations require positional arguments.");
                    }
                    if (method == "with_step") require(descriptor, context, node, "Range descriptor");
                    result_.intrinsics[&node] = "range." + method;
                    return resolved;
                }
                if (method != "new" && method != "closed" && method != "descending" && method != "descending_closed") return std::nullopt;
                if (node.children.size() != 3) { error(node, "A Range constructor requires start and end."); return result_.types.error(); }
                const auto& first = *node.children[1]->children[0];
                const auto& second = *node.children[2]->children[0];
                auto target = result_.types.get(expected).kind == TypeKind::Range ? result_.types.get(expected).element : InvalidTypeId;
                if (!numericLiteral(first)) target = expression(first);
                else if (!numericLiteral(second)) target = expression(second);
                const auto start = expression(first, target);
                require(expression(second, start), start, second, "Range end");
                if (!integer(start)) { error(node, "Range requires matching integer bounds."); return result_.types.error(); }
                result_.intrinsics[&node] = "range." + method;
                return result_.types.range(start);
            }

            TypeId propagation(const Node& node)
            {
                const auto operand = expression(*node.children[0]);
                const auto carrier = result_.carriers.find(operand);
                const auto returned = result_.carriers.find(returnType_);
                if (carrier == result_.carriers.end())
                { error(node, "Postfix ? requires an owned Result or Optional; borrowed carriers are not propagated."); return remember(node, result_.types.error()); }
                if (returned == result_.carriers.end() || returned->second.optional != carrier->second.optional)
                { error(node, "Postfix ? requires an enclosing function returning the same carrier kind."); return remember(node, result_.types.error()); }
                if (!carrier->second.optional && carrier->second.error != returned->second.error)
                { error(node, "Result ? requires exactly the same error type in the enclosing return type."); return remember(node, result_.types.error()); }
                result_.propagations[&node] = {operand, returnType_};
                return remember(node, carrier->second.value);
            }
            TypeId expression(const Node& node, TypeId expected = InvalidTypeId)
            {
                // 같은 표현식의 재방문은 호출 선택과 진단을 반복하지 않지만 리터럴 문맥은 먼저 결정한다.
                // Reusing an expression avoids duplicate call selection and diagnostics after its literal context is fixed.
                if (!numericLiteral(node))
                {
                    const auto existing = result_.nodeTypes.find(&node);
                    if (existing != result_.nodeTypes.end()) return adaptDynamic(node, existing->second, expected);
                }
                if (node.kind == "CompilerBoxExpression")
                {
                    const auto result = resolveType(*node.children[0]);
                    TypeId expectedInput = InvalidTypeId;
                    if (node.value == "new")
                        if (const auto carrier = result_.carriers.find(result); carrier != result_.carriers.end())
                            expectedInput = result_.types.get(carrier->second.value).element;
                    const auto input = expression(*node.children[1], expectedInput);
                    result_.boxIntrinsics[&node] = node.value;
                    if (node.value == "new")
                    {
                        const auto carrier = result_.carriers.find(result);
                        if (carrier == result_.carriers.end() || result_.types.get(carrier->second.value).kind != TypeKind::Box)
                            error(node, "Box.new has no valid Result<Box<T>, InputError<T>> result type.");
                        else require(input, result_.types.get(carrier->second.value).element, *node.children[1], "Box input");
                    }
                    else if (node.value == "into_dyn")
                    {
                        const auto& source = result_.types.get(input); const auto& destination = result_.types.get(result);
                        if (source.kind != TypeKind::Box || destination.kind != TypeKind::Box || result_.types.get(destination.element).kind != TypeKind::Dynamic
                            || !implements(source.element, destination.element)) error(node, "Box.into_dyn requires a concrete owner implementing the destination interface.");
                        else
                        {
                            adaptDynamic(node, result_.types.reference(source.element, false), result_.types.reference(destination.element, false));
                            auto information = std::move(result_.dynViews.at(&node)); result_.dynViews.erase(&node);
                            result_.boxDynViews[&node] = std::move(information);
                        }
                    }
                    else
                    {
                        const auto& borrowed = result_.types.get(input);
                        if (borrowed.kind != TypeKind::Reference || result_.types.get(borrowed.element).kind != TypeKind::Box
                            || (node.value == "as_mut" && !borrowed.isMutable)) error(node, "Box view requires a compatible explicit owner borrow.");
                    }
                    return adaptDynamic(node, result, expected);
                }
                if (node.kind == "Literal") return literal(node, expected);
                if (node.kind == "Group") return remember(node, expression(*node.children[0], expected));
                if (node.kind == "Name")
                {
                    const auto ids = references(node);
                    if (ids.size() != 1)
                    {
                        error(node, "A value reference must identify one declaration; overloaded functions require a call.");
                        return remember(node, result_.types.error());
                    }
                    if (ids[0] >= names_.symbols.size()) return remember(node, result_.types.error());
                    if (names_.symbols[ids[0]].kind == SymbolKind::Type)
                    {
                        error(node, "A type name cannot be used as a runtime value.");
                        return remember(node, result_.types.error());
                    }
                    if (!names_.symbols[ids[0]].declaration)
                        return unsupported(node, "intrinsic namespace used as a runtime value");
                    if (names_.symbols[ids[0]].declaration->kind == "Variant")
                        return variantValue(node, ids[0]);
                    return adaptDynamic(node, symbolType(ids[0]), expected);
                }
                if (node.kind == "Member") return adaptDynamic(node, memberExpression(node), expected);
                if (node.kind == "Call")
                {
                    if (const auto intrinsic = collectionIntrinsic(node, expected)) return remember(node, *intrinsic);
                    if (node.children.size() == 1 && node.children[0]->kind == "Member" && node.children[0]->value == "value"
                        && node.children[0]->children.size() == 1 && node.children[0]->children[0]->kind == "Name" && node.children[0]->children[0]->value == "Unit")
                    {
                        const auto namespaceIds = references(*node.children[0]->children[0]);
                        if (namespaceIds.size() == 1 && namespaceIds[0] < names_.symbols.size())
                        {
                            const auto& namespaceSymbol = names_.symbols[namespaceIds[0]];
                            if (!namespaceSymbol.declaration && namespaceSymbol.kind == SymbolKind::Intrinsic && namespaceSymbol.name == "Unit")
                            { result_.unitValues.insert(&node); return remember(node, result_.types.builtin("unit")); }
                        }
                    }
                    if (const auto* marker = child(node, "CompilerCollectionResult"))
                    {
                        const auto& callee = *node.children[0];
                        const auto receiver = expression(*node.children[1]->children[0]);
                        const auto& borrowed = result_.types.get(receiver);
                        const auto& vector = result_.types.get(borrowed.element);
                        if (borrowed.kind != TypeKind::Reference || !borrowed.isMutable || vector.kind != TypeKind::Vector)
                            error(node, "Vector growth requires an explicit exclusive &mut Vector borrow.");
                        const auto wanted = callee.value == "push" ? vector.element : result_.types.builtin("usize");
                        require(expression(*node.children[2]->children[0], wanted), wanted, node, "Vector growth input");
                        if (!node.children[1]->value.empty() || !node.children[2]->value.empty()) error(node, "Core Vector growth currently uses positional arguments.");
                        result_.collectionCalls[&node] = callee.value;
                        return remember(node, resolveType(*marker->children[0]));
                    }
                    const auto* collectionCallee = node.children.empty() ? nullptr : node.children[0].get();
                    if (collectionCallee && collectionCallee->kind == "Member" && (collectionCallee->value == "len" || collectionCallee->value == "capacity") && !collectionCallee->children.empty())
                    {
                        const auto owner = references(*collectionCallee->children[0]);
                        if (owner.size() == 1 && !names_.symbols[owner[0]].declaration
                            && (names_.symbols[owner[0]].name == "Vector" || names_.symbols[owner[0]].name == "Slice"))
                        {
                            if (node.children.size() != 2 || node.children[1]->children.size() != 1) { error(node, "Collection.len requires one argument."); return result_.types.error(); }
                            auto type = result_.types.get(expression(*node.children[1]->children[0]));
                            if (type.kind == TypeKind::Reference) type = result_.types.get(type.element);
                            if (type.kind != TypeKind::Vector && type.kind != TypeKind::Slice) error(node, "Collection.len requires a Vector borrow or Slice.");
                            const auto argumentType = result_.types.get(result_.nodeTypes.at(node.children[1]->children[0].get()));
                            if (type.kind == TypeKind::Vector && argumentType.kind != TypeKind::Reference) error(node, "Vector.len requires an explicit borrow.");
                            result_.collectionCalls[&node] = collectionCallee->value; return remember(node, result_.types.builtin("usize"));
                        }
                    }
                    if (const auto predicate = corePredicate(node)) return remember(node, *predicate);
                    return adaptDynamic(node, call(node), expected);
                }
                if (node.kind == "NamedInitializer") return namedInitializer(node);
                if (node.kind == "ArrayLiteral") return arrayLiteral(node, expected);
                if (node.kind == "Index")
                {
                    const auto receiver = expression(*node.children[0]);
                    auto type = result_.types.get(receiver);
                    if (type.kind == TypeKind::Reference) type = result_.types.get(type.element);
                    const auto index = expression(*node.children[1], result_.types.builtin("usize"));
                    if (valid(index) && !integer(index)) error(*node.children[1], "An array index must have an integer type.");
                    if (type.kind != TypeKind::Array && type.kind != TypeKind::Vector && type.kind != TypeKind::Slice)
                    {
                        if (valid(receiver)) error(node, "Indexing requires an Array, Vector or Slice value or borrow.");
                        return remember(node, result_.types.error());
                    }
                    const auto constant = evaluate(*node.children[1], index);
                    if (constant.status == ConstStatus::Invalid) diagnostics_.push_back({constant.range, constant.message});
                    else if (type.kind == TypeKind::Array && constant.status == ConstStatus::Constant && constant.value.kind == ConstantKind::Integer
                        && (constant.value.negative || constant.value.magnitude >= type.extent))
                        error(*node.children[1], "Constant array index is out of bounds.");
                    return adaptDynamic(node, type.element, expected);
                }
                if (node.kind == "Prefix")
                {
                    const auto& value = *node.children[0];
                    if ((node.value == "-" || node.value == "+") && numericLiteral(node)
                        && !floatingSpelling(numericLiteral(node)->value))
                    {
                        auto type = integer(expected) ? expected : result_.types.builtin("i32");
                        if (!fitsInteger(node, type))
                        {
                            error(node, "Numeric literal is out of range for " + result_.types.display(type) + ".");
                            type = result_.types.error();
                        }
                        std::vector<const Node*> signedNodes{&value};
                        while (!signedNodes.empty())
                        {
                            const auto* signedNode = signedNodes.back();
                            signedNodes.pop_back();
                            remember(*signedNode, type);
                            if (signedNode->kind == "Group" || signedNode->kind == "Prefix")
                                signedNodes.push_back(signedNode->children[0].get());
                        }
                        return remember(node, type);
                    }
                    const auto type = expression(value, numeric(expected) ? expected : InvalidTypeId);
                    const auto& target = result_.types.get(type);
                    if (node.value == "&" || node.value == "&mut")
                    {
                        if (!lvalue(value)) error(value, "Borrowing requires an addressable place.");
                        if (node.value == "&mut") requireWritable(value);
                        return adaptDynamic(node, result_.types.reference(type, node.value == "&mut"), expected);
                    }
                    if (node.value == "*")
                    {
                        if (target.kind == TypeKind::Reference) return adaptDynamic(node, target.element, expected);
                        if (valid(type)) error(node, "Dereference requires a reference value.");
                        return remember(node, result_.types.error());
                    }
                    if (node.value == "!")
                    {
                        require(type, result_.types.builtin("bool"), node, "Logical negation");
                        return remember(node, result_.types.builtin("bool"));
                    }
                    if (node.value == "++" || node.value == "--")
                    {
                        requireWritable(value);
                        if (valid(type) && !integer(type)) error(node, "Increment and decrement require an integer.");
                        return remember(node, type);
                    }
                    if (valid(type) && (node.value == "~" ? !integer(type) : !numeric(type)))
                        error(node, "Unary operator '" + node.value + "' does not support " + result_.types.display(type) + ".");
                    return remember(node, type);
                }
                if (node.kind == "Postfix")
                {
                    if (node.value == "?") return propagation(node);
                    const auto type = expression(*node.children[0]);
                    requireWritable(*node.children[0]);
                    if (valid(type) && !integer(type)) error(node, "Increment and decrement require an integer.");
                    return remember(node, type);
                }
                if (node.kind == "Binary" || node.kind == "Range") return binary(node, expected);
                if (node.kind == "Conditional")
                {
                    require(expression(*node.children[0]), result_.types.builtin("bool"), *node.children[0], "Conditional condition");
                    TypeId target = expected;
                    if (!valid(target))
                    {
                        if (numericLiteral(*node.children[1]) && !numericLiteral(*node.children[2])) target = expression(*node.children[2]);
                        else target = expression(*node.children[1]);
                    }
                    const auto left = expression(*node.children[1], target);
                    const auto right = expression(*node.children[2], target);
                    require(right, left, node, "Conditional alternatives");
                    return remember(node, left);
                }
                return unsupported(node, "expression '" + node.kind + "'");
            }

            TypeId binary(const Node& node, TypeId expected)
            {
                const auto& leftNode = *node.children[0];
                const auto& rightNode = *node.children[1];
                const auto& operation = node.value;
                if (operation == ".." || operation == "..=")
                {
                    TypeId target = result_.types.get(expected).kind == TypeKind::Range ? result_.types.get(expected).element : InvalidTypeId;
                    if (!numericLiteral(leftNode)) target = expression(leftNode);
                    else if (!numericLiteral(rightNode)) target = expression(rightNode);
                    const auto left = expression(leftNode, target);
                    require(expression(rightNode, left), left, rightNode, "Range bound");
                    if (!integer(left)) { error(node, "Range bounds must be integers."); return remember(node, result_.types.error()); }
                    result_.intrinsics[&node] = operation == ".." ? "range.new" : "range.closed";
                    return remember(node, result_.types.range(left));
                }
                if (operation == "&&" || operation == "||")
                {
                    const auto boolean = result_.types.builtin("bool");
                    require(expression(leftNode), boolean, leftNode, "Logical operand");
                    require(expression(rightNode), boolean, rightNode, "Logical operand");
                    return remember(node, boolean);
                }
                TypeId target = numeric(expected) ? expected : InvalidTypeId;
                if (numericLiteral(leftNode) && !numericLiteral(rightNode)) target = expression(rightNode);
                else if (!numericLiteral(leftNode)) target = expression(leftNode);
                const auto left = expression(leftNode, target);
                const bool shift = operation == "<<" || operation == ">>";
                const auto right = expression(rightNode, shift ? InvalidTypeId : left);
                if (shift)
                {
                    if ((valid(left) && !integer(left)) || (valid(right) && !integer(right)))
                        error(node, "Shift operators require integer operands.");
                    return remember(node, left);
                }
                require(right, left, node, "Binary operands");
                const bool equality = operation == "==" || operation == "!=";
                const bool ordering = operation == "<" || operation == "<=" || operation == ">" || operation == ">=";
                const auto kind = result_.types.get(left).kind;
                if (equality || ordering)
                {
                    const bool comparable = numeric(left) || kind == TypeKind::Char || kind == TypeKind::Byte || (equality && (kind == TypeKind::Bool || kind == TypeKind::Unit));
                    if (valid(left) && !comparable) error(node, "This type has no supported comparison operator.");
                    return remember(node, result_.types.builtin("bool"));
                }
                const bool integral = operation == "%" || operation == "&" || operation == "|" || operation == "^";
                if (valid(left) && (integral ? !integer(left) : !numeric(left)))
                    error(node, "Operator '" + operation + "' does not support " + result_.types.display(left) + ".");
                return remember(node, left);
            }

            void assignment(const Node& node)
            {
                const auto& place = *node.children[0];
                const auto type = expression(place);
                bool deferredFirstWrite = false;
                const Node* directPlace = &place;
                while (directPlace->kind == "Group") directPlace = directPlace->children[0].get();
                if (node.value == "=" && (directPlace->kind == "Name" || directPlace->kind == "Member"))
                {
                    const auto ids = references(*directPlace);
                    if (ids.size() == 1 && ids[0] < names_.symbols.size())
                    {
                        const auto* declaration = names_.symbols[ids[0]].declaration;
                        deferredFirstWrite = declaration && declaration->kind == "Variable" && !initializer(*declaration)
                            && (directPlace->kind == "Name" || owners_.at(declaration) == nullptr);
                    }
                }
                // 초기값 없는 불변 변수의 최초 대입 횟수와 경로는 제어 흐름 검사가 판단한다.
                // Control-flow analysis decides the number and paths of first writes to uninitialized immutable locals.
                if (!deferredFirstWrite) requireWritable(place);
                const auto value = expression(*node.children[1], type);
                require(value, type, node, "Assignment");
                if (node.value != "=")
                {
                    const bool integral = node.value == "%=" || node.value == "&=" || node.value == "|=" || node.value == "^=" || node.value == "<<=" || node.value == ">>=";
                    if (valid(type) && (integral ? !integer(type) : !numeric(type)))
                        error(node, "Compound assignment requires compatible numeric operands.");
                }
                remember(node, result_.types.builtin("unit"));
            }

            void statement(const Node& node)
            {
                if (node.kind == "Block" || node.kind == "CaseScope")
                {
                    for (const auto& item : node.children) statement(*item);
                    return;
                }
                if (node.kind == "Variable") { checkVariable(node); return; }
                if (node.kind == "ExpressionStatement") { expression(*node.children[0]); return; }
                if (node.kind == "Assignment") { assignment(node); return; }
                if (node.kind == "Return")
                {
                    const auto& result = result_.types.get(returnType_);
                    if (node.children.empty())
                    {
                        if (valid(returnType_) && result.kind != TypeKind::Void) error(node, "A non-void function must return an explicit value.");
                    }
                    else
                    {
                        const auto value = expression(*node.children[0], returnType_);
                        if (result.kind == TypeKind::Void) error(node, "A void function cannot return a value.");
                        else require(value, returnType_, node, "Return value");
                    }
                    return;
                }
                if (node.kind == "If" || node.kind == "While")
                {
                    require(expression(*node.children[0]), result_.types.builtin("bool"), *node.children[0], "Condition");
                    for (std::size_t index = 1; index < node.children.size(); ++index) statement(*node.children[index]);
                    return;
                }
                if (node.kind == "For")
                {
                    statement(*node.children[0]);
                    if (node.children[1]->kind != "Empty")
                        require(expression(*node.children[1]), result_.types.builtin("bool"), *node.children[1], "For condition");
                    statement(*node.children[2]);
                    statement(*node.children[3]);
                    return;
                }
                if (node.kind == "RangeFor")
                {
                    const auto iterable = expression(*node.children[1]);
                    auto range = result_.types.get(iterable);
                    if (range.kind == TypeKind::Reference) range = result_.types.get(range.element);
                    if (range.kind != TypeKind::Range) { error(node, "Range iteration requires a Range value."); return; }
                    const auto& binding = *node.children[0];
                    const auto type = binding.children[0]->kind == "InferredType" ? range.element : declaredType(binding);
                    require(range.element, type, binding, "Range iteration binding");
                    result_.symbolTypes[declarationId(binding)] = type;
                    remember(binding, type);
                    statement(*node.children[2]); return;
                }
                if (node.kind == "EnhancedFor")
                {
                    const auto iterable = expression(*node.children[1]);
                    auto collection = result_.types.get(iterable);
                    if (collection.kind == TypeKind::Reference) collection = result_.types.get(collection.element);
                    TypeId element = result_.types.error();
                    if (collection.kind == TypeKind::Array) element = collection.element;
                    else if (collection.kind == TypeKind::String || collection.kind == TypeKind::StringView) element = result_.types.builtin("char");
                    else if (valid(iterable)) error(node, "Enhanced for currently requires a fixed Array or text value.");
                    const auto& binding = *node.children[0];
                    TypeId type = binding.children[0]->kind == "InferredType" ? element : declaredType(binding);
                    require(element, type, binding, "Iteration binding");
                    const auto id = declarationId(binding);
                    if (id != InvalidSymbolId) result_.symbolTypes[id] = type;
                    remember(binding, type);
                    statement(*node.children[2]);
                    return;
                }
                if (node.kind == "Switch")
                {
                    const auto type = expression(*node.children[0]);
                    const auto kind = result_.types.get(type).kind;
                    const auto* declaration = nominalDeclaration(type);
                    if (declaration && declaration->kind == "Enum")
                    {
                        enumSwitch(node, type);
                        return;
                    }
                    if (valid(type) && kind != TypeKind::Integer && kind != TypeKind::Bool && kind != TypeKind::Char && kind != TypeKind::Byte)
                        error(node, "This switch requires a supported scalar selector.");
                    std::set<std::string> cases;
                    for (std::size_t index = 1; index < node.children.size(); ++index)
                    {
                        const auto& branch = *node.children[index];
                        if (branch.kind == "Case" && !branch.children.empty())
                        {
                            const auto& label = *branch.children[0];
                            require(expression(label, type), type, label, "Case value");
                            if (requireConstant(label, type, "A switch case label must be a compile-time constant."))
                            {
                                const auto key = result_.constants.at(&label).key();
                                if (!cases.insert(key).second) error(label, "Duplicate switch case value.");
                            }
                        }
                        if (child(branch, "PatternBindings")) unsupported(branch, "enum pattern bindings");
                        if (const auto* scope = child(branch, "CaseScope")) statement(*scope);
                    }
                    return;
                }
                if (node.kind == "Empty" || node.kind == "Break" || node.kind == "Continue") return;
                unsupported(node, "statement '" + node.kind + "'");
            }

            void checkDeclaration(const Node& declaration)
            {
                if (erased_.contains(&declaration) || child(declaration, "CompilerBox")) return;
                if (declaration.kind == "Import" || declaration.kind == "Modifier") return;
                if (declaration.kind == "Variable") { checkVariable(declaration); return; }
                if (declaration.kind == "Struct" || declaration.kind == "Enum")
                {
                    if (const auto* generics = child(declaration, "GenericParameters"); generics && !generics->children.empty()) return;
                    for (const auto& member : declaration.children)
                    {
                        if (member->kind == "Variable" || member->kind == "Function") checkDeclaration(*member);
                        if (member->kind == "Variant")
                            for (const auto& field : member->children)
                                if (field->kind == "Variable" || field->kind == "Function") checkDeclaration(*field);
                    }
                    if (const auto* interfaces = child(declaration, "Implements"))
                        for (const auto& interface : interfaces->children)
                            if (interface->value == "Copy")
                            {
                                std::unordered_set<TypeId> visiting;
                                if (!copyType(symbolType(declarationId(declaration)), visiting))
                                    error(declaration, "A struct implementing Copy must contain only Copy-compatible fields.");
                            }
                    return;
                }
                if (declaration.kind != "Function") return;
                const auto function = signature(declarationId(declaration));
                if (!function.supported) return;
                for (std::size_t index = 0; index < function.declarations.size(); ++index)
                {
                    const auto& parameter = *function.declarations[index];
                    if (const auto* initial = child(parameter, "Default"))
                    {
                        const auto& value = *initial->children[0];
                        require(expression(value, function.parameters[index]), function.parameters[index], value, "Default argument");
                        std::unordered_set<TypeId> copyVisiting;
                        if (!copyType(function.parameters[index], copyVisiting))
                            error(value, "A parameter default must be a Copy-compatible compile-time constant.");
                        else requireConstant(value, function.parameters[index], "A parameter default must be a Copy-compatible compile-time constant.");
                    }
                }
                const auto previous = returnType_;
                returnType_ = function.result;
                if (const auto* body = child(declaration, "Block")) statement(*body);
                returnType_ = previous;
            }
        };
    }

    TypeChecker::TypeChecker(Diagnostics& diagnostics)
        : diagnostics_(diagnostics)
    {
    }

    TypeCheckResult TypeChecker::check(const ModuleGraph& graph, const NameResolution& names)
    {
        if (!names.succeeded || !diagnostics_.empty()) return {};
        for (const auto& module : graph.modules)
        {
            if (!module || !module->ast)
            {
                diagnostics_.push_back({{}, "Type checking requires parsed modules and successful name resolution."});
                return {};
            }
        }
        return Checker(diagnostics_, graph, names).run();
    }
}
