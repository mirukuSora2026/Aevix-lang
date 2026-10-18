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
#include "aevix/Frontend/Lexer/LiteralText.hpp"
#include "aevix/Frontend/Sema/NumericLiteral.hpp"
#include "aevix/Native/OsOperation.hpp"
#include "aevix/Native/AtomicOperation.hpp"
#include "aevix/Native/TaskOperation.hpp"
#include "aevix/Native/RangeOperation.hpp"
#include "aevix/Native/SyncOperation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
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

        TypeKind syncMarker(const Node& node)
        {
            return child(node, "CompilerMutex") ? TypeKind::Mutex : child(node, "CompilerRwLock") ? TypeKind::RwLock
                : child(node, "CompilerMutexGuard") ? TypeKind::MutexGuard : child(node, "CompilerReadGuard") ? TypeKind::RwLockReadGuard
                : child(node, "CompilerWriteGuard") ? TypeKind::RwLockWriteGuard : TypeKind::Error;
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
            UInt128 magnitude;
            bool negative = false;
            bool overflow = false;
        };

        // 부호를 별도로 보관하여 가장 작은 음수도 양수 중간값의 넘침 없이 검사한다.
        // Keep the sign separate so the smallest negative integer needs no overflowing positive intermediate.
        IntegerValue integerValue(std::string_view spelling, bool negative = false)
        {
            spelling = numericSpelling(spelling);
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
            value.overflow = !UInt128::parse(spelling.substr(start), value.magnitude, base);
            if (!value.magnitude) value.negative = false;
            return value;
        }

        bool floatingSpelling(std::string_view text)
        {
            return numericFloating(text);
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
                    if (symbol.declaration && !erased_.contains(symbol.declaration))
                    {
                        if (child(*symbol.declaration,"CompilerTaskScope")) result_.symbolTypes[symbol.id] = result_.types.taskOwner(TypeKind::TaskScope,symbol.id,symbol.name);
                        else if (child(*symbol.declaration,"CompilerTask"))
                        {
                            const Node* payload = nullptr;
                            bool valid = symbol.module < graph_.modules.size() && graph_.modules[symbol.module]->package == "$task"
                                && graph_.modules[symbol.module]->name == "task";
                            for (const auto& field : symbol.declaration->children) if (field->kind == "Variable")
                            {
                                valid = valid && !payload && field->value == "result_type" && field->children.size() == 2
                                    && field->children[1]->kind == "ArraySuffixes" && field->children[1]->children.empty()
                                    && !flag(*field,"public") && !flag(*field,"persist") && !flag(*field,"mut");
                                if (!field->children.empty()) payload = field->children[0].get();
                            }
                            if (!valid || !payload) error(*symbol.declaration,"The sealed Task declaration requires exactly one private application-result metadata field.");
                            else result_.symbolTypes[symbol.id] = result_.types.taskOwner(TypeKind::Task,symbol.id,symbol.name,resolveType(*payload));
                        }
                    }
                for (const auto& symbol : names_.symbols)
                    if (symbol.declaration && !erased_.contains(symbol.declaration)
                        && (child(*symbol.declaration, "CompilerCancellationSource") || child(*symbol.declaration, "CompilerCancellationToken")))
                        result_.symbolTypes[symbol.id] = result_.types.cancellation(symbol.id, symbol.name, child(*symbol.declaration, "CompilerCancellationToken") != nullptr);
                for (const auto& symbol : names_.symbols)
                    if (symbol.declaration && !erased_.contains(symbol.declaration) && (child(*symbol.declaration, "CompilerBox") || child(*symbol.declaration, "CompilerShared") || child(*symbol.declaration, "CompilerWeak") || child(*symbol.declaration, "CompilerAtomic")))
                        for (const auto& field : symbol.declaration->children)
                            if (field->kind == "Variable" && !field->children.empty())
                                result_.symbolTypes[symbol.id] = child(*symbol.declaration, "CompilerBox")
                                    ? result_.types.boxed(symbol.id, symbol.name, resolveType(*field->children[0]))
                                    : child(*symbol.declaration, "CompilerAtomic") ? result_.types.atomic(symbol.id, symbol.name, resolveType(*field->children[0]))
                                    : result_.types.shared(symbol.id, symbol.name, resolveType(*field->children[0]), child(*symbol.declaration, "CompilerWeak") != nullptr);
                for (const auto& symbol : names_.symbols)
                    if (symbol.declaration && !erased_.contains(symbol.declaration) && syncMarker(*symbol.declaration) != TypeKind::Error)
                        for (const auto& field : symbol.declaration->children)
                            if (field->kind == "Variable" && !field->children.empty())
                                result_.symbolTypes[symbol.id] = result_.types.sync(syncMarker(*symbol.declaration), symbol.id, symbol.name, resolveType(*field->children[0]));
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
                        if (owners_.contains(&declaration) && owners_.at(&declaration) && (syncMarker(*owners_.at(&declaration)) != TypeKind::Error || child(*owners_.at(&declaration), "CompilerTask") || child(*owners_.at(&declaration), "CompilerTaskScope") || child(*owners_.at(&declaration), "CompilerCancellationSource") || child(*owners_.at(&declaration), "CompilerCancellationToken") || child(*owners_.at(&declaration), "CompilerBox") || child(*owners_.at(&declaration), "CompilerShared") || child(*owners_.at(&declaration), "CompilerWeak") || child(*owners_.at(&declaration), "CompilerAtomic"))) continue;
                        if (!declaration.children.empty() && declaration.children[0]->kind != "InferredType")
                            result_.symbolTypes[symbol.id] = declaredType(declaration);
                    }
                }
                registerEnums();
                registerDropMethods();
                registerCarriers();
                for (const auto& module : graph_.modules)
                    for (const auto& declaration : module->ast->children)
                        checkDeclaration(*declaration);
                validateParallelWorkers();
                validateTaskGlobals();
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
                    if (std::any_of(information.targets.begin(), information.targets.end(), [&](SymbolId id) { return dropMethod(id); }))
                        error(*call, "Drop cleanup methods cannot be called explicitly through dyn interfaces.");
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

            bool builtinMember(const Node& callee, std::string_view expected) const
            {
                if (callee.kind != "Member" || callee.children.size() != 1
                    || callee.children[0]->kind != "Name" || callee.children[0]->value != expected) return false;
                // 전체 호출 경로가 사용자 함수면 같은 철자의 내장 소유자를 무시한다.
                // An exact user-function binding takes precedence over an identically spelled builtin owner.
                if (!references(callee).empty()) return false;
                const auto ids = references(*callee.children[0]);
                if (ids.size() != 1 || ids[0] >= names_.symbols.size()) return false;
                const auto& symbol = names_.symbols[ids[0]];
                return !symbol.declaration && symbol.module == InvalidModuleId && symbol.name == expected
                    && (symbol.kind == SymbolKind::Type || symbol.kind == SymbolKind::Intrinsic);
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
                if (node.kind == "ConstantTypeArgument" && node.children.size() == 1) return extent(*node.children[0]);
                const auto constant = evaluate(node, result_.types.builtin("usize"));
                if (constant.status == ConstStatus::Invalid)
                {
                    diagnostics_.push_back({constant.range, constant.message});
                    return std::nullopt;
                }
                if (constant.status == ConstStatus::Constant && constant.value.kind == ConstantKind::Integer
                    && !constant.value.negative && !constant.value.highMagnitude && constant.value.magnitude <= std::numeric_limits<std::size_t>::max())
                {
                    result_.constants[&node] = constant.value;
                    return static_cast<std::size_t>(constant.value.magnitude);
                }
                error(node, "A fixed array extent must be a nonnegative compile-time integer constant.");
                return std::nullopt;
            }

            TypeId resolveType(const Node& node)
            {
                const auto simdIds = references(node);
                if ((node.kind == "Type" || node.kind == "QualifiedType") && simdIds.size() == 1
                    && simdIds[0] < names_.symbols.size() && names_.symbols[simdIds[0]].declaration
                    && child(*names_.symbols[simdIds[0]].declaration, "CompilerSimd"))
                {
                    const bool mask = names_.symbols[simdIds[0]].name == "SimdMask";
                    const std::size_t first = node.kind == "QualifiedType" ? 1 : 0;
                    if (node.children.size() != first + (mask ? 1 : 2))
                    { error(node, mask ? "SimdMask requires a constant lane count." : "Simd requires an element type and constant lane count."); return remember(node, result_.types.error()); }
                    const auto lanes = extent(*node.children[first + (mask ? 0 : 1)]);
                    const auto element = mask ? InvalidTypeId : resolveType(*node.children[first]);
                    const auto resolved = lanes ? (mask ? result_.types.simdMask(*lanes) : result_.types.simd(element, *lanes)) : result_.types.error();
                    if (!valid(resolved)) error(node, "SIMD requires 2, 4, 8, or 16 numeric lanes totaling at most 128 bits; f16 and bf16 lanes are not supported.");
                    return remember(node, resolved);
                }
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
                    if (ids.size() == 1 && ids[0] < names_.symbols.size()
                        && !names_.symbols[ids[0]].declaration && names_.symbols[ids[0]].name == "Drop")
                    {
                        error(node, "Drop is a lifecycle marker and cannot form dyn Drop or Box<dyn Drop>.");
                        return remember(node, result_.types.error());
                    }
                    if (ids.size() == 1 && ids[0] < names_.symbols.size() && names_.symbols[ids[0]].declaration
                        && names_.symbols[ids[0]].declaration->kind == "Interface")
                        return remember(node, result_.types.dynamic(ids[0], names_.symbols[ids[0]].name));
                    error(node, "dyn requires one resolved interface type.");
                    return remember(node, result_.types.error());
                }
                if (node.kind == "InferredType") return result_.types.error();
                if (node.kind == "Type" && node.value == "Tuple")
                {
                    std::vector<TypeId> fields;
                    for (const auto& field : node.children) fields.push_back(resolveType(*field));
                    const auto type = result_.types.tuple(fields);
                    if (!valid(type)) error(node, "Tuple fields must have storable value types.");
                    return remember(node, type);
                }
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
                    if (symbol.declaration && symbol.declaration->kind == "TypeAlias" && !symbol.declaration->children.empty())
                    {
                        const auto type = resolveType(*symbol.declaration->children[0]);
                        result_.symbolTypes[symbol.id] = type;
                        return remember(node, type);
                    }
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
                if (result_.types.get(type).kind == TypeKind::Never)
                {
                    error(declaration, "never has no values and cannot be stored in a variable or parameter.");
                    return remember(declaration, result_.types.error());
                }
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
                        if (((type->value != "Copy" && type->value != "Drop") || !type->children.empty())
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

            bool implementsDrop(const Node& owner) const
            {
                const auto* implemented = child(owner, "Implements");
                return implemented && std::any_of(implemented->children.begin(), implemented->children.end(),
                    [](const auto& marker) { return marker->value == "Drop"; });
            }

            bool dropMethod(SymbolId id) const
            {
                if (id >= names_.symbols.size()) return false;
                const auto* method = names_.symbols[id].declaration;
                if (!method || method->kind != "Function" || method->value != "drop") return false;
                const auto owner = owners_.find(method);
                return owner != owners_.end() && owner->second && implementsDrop(*owner->second);
            }

            void registerDropMethods()
            {
                // 구체 타입의 정리 훅을 본문 검사 전에 고정하여 직접 호출도 차단한다.
                // Register concrete cleanup hooks before checking bodies, including direct-call rejection.
                for (const auto& owner : names_.symbols)
                {
                    if (!owner.declaration || erased_.contains(owner.declaration)
                        || (owner.declaration->kind != "Struct" && owner.declaration->kind != "Enum")
                        || !implementsDrop(*owner.declaration)) continue;
                    const auto& declaration = *owner.declaration;
                    const auto ownerType = symbolType(owner.id);
                    const auto* implemented = child(declaration, "Implements");
                    std::size_t markers = 0;
                    for (const auto& marker : implemented->children)
                    {
                        if (marker->value == "Copy") error(declaration, "A type cannot implement both Copy and Drop.");
                        if (marker->value == "Drop")
                        {
                            ++markers;
                            if (!marker->children.empty()) error(*marker, "Drop is a lifecycle marker without type arguments.");
                        }
                    }
                    if (markers != 1) error(declaration, "A type must list the Drop lifecycle marker exactly once.");
                    std::vector<SymbolId> methods;
                    for (const auto& member : declaration.children)
                        if (member->kind == "Function" && member->value == "drop") methods.push_back(declarationId(*member));
                    if (methods.size() != 1)
                    {
                        error(declaration, "Drop requires exactly one public void drop(&mut Self self) method.");
                        continue;
                    }
                    const auto id = methods[0];
                    const auto function = signature(id);
                    const auto& method = *function.node;
                    bool valid = function.supported && flag(method, "public") && !flag(method, "private")
                        && !flag(method, "override") && !flag(method, "mut") && !flag(method, "const")
                        && !flag(method, "persist") && !flag(method, "extern") && !child(method, "Constraint")
                        && child(method, "Block") && function.result == result_.types.builtin("void")
                        && !function.variadic && function.parameters.size() == 1
                        && function.declarations.size() == 1;
                    if (const auto* generics = child(method, "GenericParameters"); generics && !generics->children.empty()) valid = false;
                    if (valid)
                    {
                        const auto& self = *function.declarations[0];
                        valid = self.kind == "Parameter" && self.value == "self"
                            && !child(self, "Default") && !child(self, "ArraySuffixes") && !child(self, "Modifier")
                            && function.parameters[0] == result_.types.reference(ownerType, true);
                    }
                    if (!valid) error(method, "Drop requires the exact public void drop(&mut Self self) contract without generic, variadic, default, or extra parameters.");
                    else result_.dropMethods[ownerType] = id;
                }
            }

            bool fitsInteger(const Node& node, TypeId type, bool negative = false) const
            {
                const auto* literal = numericLiteral(node);
                if (!literal || floatingSpelling(literal->value)) return false;
                if (node.kind == "Group") return fitsInteger(*node.children[0], type, negative);
                if (node.kind == "Prefix")
                    return fitsInteger(*node.children[0], type, node.value == "-" ? !negative : negative);
                const auto suffix = numericSuffix(literal->value);
                if (!suffix.empty() && type != result_.types.builtin(suffix)) return false;
                const auto& target = result_.types.get(type);
                if (target.kind != TypeKind::Integer && target.kind != TypeKind::Byte) return false;
                const auto value = integerValue(literal->value, negative);
                if (value.overflow || (value.negative && !target.isSigned && static_cast<bool>(value.magnitude))) return false;
                const auto bits = target.kind == TypeKind::Byte ? 8 : target.bits;
                if (target.isSigned)
                {
                    const auto bound = UInt128{1} << (bits - 1);
                    return value.magnitude <= (value.negative ? bound : bound - UInt128{1});
                }
                return bits == 128 || value.magnitude < (UInt128{1} << bits);
            }

            bool fitsFloat(const Node& node, TypeId type) const
            {
                const auto* literal = numericLiteral(node);
                if (!literal || !floatingSpelling(literal->value) || result_.types.get(type).kind != TypeKind::Float) return false;
                const auto suffix = numericSuffix(literal->value);
                if (!suffix.empty() && type != result_.types.builtin(suffix)) return false;
                std::string spelling;
                for (char character : numericSpelling(literal->value)) if (character != '_') spelling += character;
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
                if (isStringLiteralSpelling(node.value)) return remember(node, result_.types.builtin("string_view"));
                if (node.value.starts_with('\'')) return remember(node, result_.types.builtin("char"));
                const bool floating = floatingSpelling(node.value);
                TypeId type = floating ? result_.types.builtin("f64") : result_.types.builtin("i32");
                const auto kind = result_.types.get(expected).kind;
                const auto suffix = numericSuffix(node.value);
                if (!suffix.empty()) type = result_.types.builtin(suffix);
                else if ((floating && kind == TypeKind::Float) || (!floating && kind == TypeKind::Integer)) type = expected;
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

            bool readOnlyProjection(const Node& node)
            {
                // 읽기 대여를 거쳐 얻은 수정 참조는 다시 쓰기 권한을 부여하지 않는다.
                // A mutable reference reached through a shared loan cannot restore write authority.
                if (node.kind == "Group" && !node.children.empty()) return readOnlyProjection(*node.children[0]);
                if ((node.kind == "Prefix" && node.value == "*") || node.kind == "Member" || node.kind == "Index")
                {
                    if (node.children.empty()) return false;
                    const auto& base = *node.children[0];
                    const auto& type = result_.types.get(expression(base));
                    if ((type.kind == TypeKind::Reference || type.kind == TypeKind::Slice) && !type.isMutable) return true;
                    return readOnlyProjection(base);
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
                    return reference.kind == TypeKind::Reference && reference.isMutable
                        && !readOnlyProjection(*node.children[0]);
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
                    if (reference.kind == TypeKind::Slice) return reference.isMutable && !readOnlyProjection(*node.children[0]);
                    if (reference.kind == TypeKind::Reference) return reference.isMutable && !readOnlyProjection(*node.children[0]);
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
                    if (std::any_of(ids.begin(), ids.end(), [&](SymbolId id) { return dropMethod(id); }))
                    {
                        error(node, "Drop cleanup methods cannot be referenced or called explicitly.");
                        return remember(node, result_.types.error());
                    }
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

            TypeId repeatedArray(const Node& node, TypeId expected)
            {
                const auto count = node.children.size() == 2 ? extent(*node.children[1]) : std::nullopt;
                if (!count) return remember(node, result_.types.error());
                const auto& context = result_.types.get(expected);
                const auto element = expression(*node.children[0], context.kind == TypeKind::Array ? context.element : InvalidTypeId);
                std::unordered_set<TypeId> visiting;
                if (!copyType(element, visiting)) error(node, "Repeated array elements must implement Copy; no implicit clone is performed.");
                const auto type = result_.types.array(element, *count);
                if (context.kind == TypeKind::Array && (context.element != element || context.extent != *count))
                    error(node, "Repeated array length or element type does not match its declared Array.");
                return remember(node, type);
            }

            bool copyType(TypeId type, std::unordered_set<TypeId>& visiting)
            {
                const auto& item = result_.types.get(type);
                if (taskOwner(item.kind) || cancellationOwner(item.kind) || syncOwner(item.kind) || syncGuard(item.kind) || item.kind == TypeKind::String || item.kind == TypeKind::Tuple || item.kind == TypeKind::Array || item.kind == TypeKind::Vector || item.kind == TypeKind::Box || item.kind == TypeKind::Shared || item.kind == TypeKind::Weak || item.kind == TypeKind::Atomic || item.kind == TypeKind::Void) return false;
                if (item.kind == TypeKind::Reference || item.kind == TypeKind::Slice) return !item.isMutable;
                if (item.kind != TypeKind::Nominal) return item.kind != TypeKind::Error;
                const auto* declaration = nominalDeclaration(type);
                const auto* implemented = declaration ? child(*declaration, "Implements") : nullptr;
                if (!implemented || !visiting.insert(type).second) return false;
                bool marker = false;
                for (const auto& interface : implemented->children) marker = marker || interface->value == "Copy";
                if (implementsDrop(*declaration)) marker = false;
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
                    if (result_.types.get(type).kind == TypeKind::Void || result_.types.get(type).kind == TypeKind::Never)
                    {
                        error(declaration, "rsn cannot infer a storable variable type from a void or never expression.");
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

            const CancellationOperationInfo* cancellationBinding(SymbolId id) const
            {
                if (id >= names_.symbols.size()) return nullptr;
                const auto& symbol = names_.symbols[id];
                const auto* marker = symbol.declaration ? child(*symbol.declaration, "CompilerCancellation") : nullptr;
                if (!marker || symbol.module >= graph_.modules.size() || graph_.modules[symbol.module]->package != "$task") return nullptr;
                return cancellationOperation(marker->value);
            }
            bool cancellationSignature(const Signature& signature, const CancellationOperationInfo& operation)
            {
                if (signature.variadic || signature.parameters.size() != operation.operands) return false;
                if (operation.operands)
                {
                    const auto& input = result_.types.get(signature.parameters[0]);
                    if (input.kind != TypeKind::Reference || input.isMutable || result_.types.get(input.element).kind != operation.input) return false;
                }
                auto output = signature.result;
                if (operation.fallible)
                {
                    const auto carrier = result_.carriers.find(output);
                    if (carrier == result_.carriers.end() || carrier->second.optional || !standardNominal(carrier->second.error,"CancellationError","$task")) return false;
                    output = carrier->second.value;
                }
                const auto kind = result_.types.get(output).kind;
                return operation.result == CancellationResult::Source ? kind == TypeKind::CancellationSource
                    : operation.result == CancellationResult::Token ? kind == TypeKind::CancellationToken
                    : operation.result == CancellationResult::Boolean ? kind == TypeKind::Bool
                    : standardNominal(output,"CancelRequest","$task");
            }

            const OsOperation* osBinding(SymbolId id) const
            {
                if (id >= names_.symbols.size()) return nullptr;
                const auto& symbol = names_.symbols[id];
                const auto* marker = symbol.declaration ? child(*symbol.declaration, "CompilerOs") : nullptr;
                if (!marker || symbol.module >= graph_.modules.size()) return nullptr;
                const auto* operation = osOperation(marker->value);
                return operation && graph_.modules[symbol.module]->package == "$" + std::string(operation->module)
                    ? operation : nullptr;
            }
            TypeId osPayloadType(const OsOperation& operation)
            {
                if (operation.tokenResult) return result_.types.builtin("u64");
                switch (operation.payload)
                {
                    case OsResultKind::Progress: return result_.types.builtin("usize");
                    case OsResultKind::Text: return result_.types.builtin("string");
                    case OsResultKind::SignedTime: return result_.types.builtin("i64");
                    case OsResultKind::Exit: return result_.types.tuple({result_.types.builtin("i32"), result_.types.builtin("i32")});
                    case OsResultKind::Metadata: return result_.types.tuple({result_.types.builtin("usize"),
                        result_.types.builtin("i64"), result_.types.builtin("u32"), result_.types.builtin("u32")});
                }
                return result_.types.error();
            }
            bool osValueType(TypeId id, OsValueKind expected) const
            {
                const auto& type = result_.types.get(id);
                switch (expected)
                {
                    case OsValueKind::U64: return id == result_.types.builtin("u64");
                    case OsValueKind::U32: return id == result_.types.builtin("u32");
                    case OsValueKind::I64: return id == result_.types.builtin("i64");
                    case OsValueKind::Usize: return id == result_.types.builtin("usize");
                    case OsValueKind::Text: return type.kind == TypeKind::StringView;
                    case OsValueKind::Arguments: return type.kind == TypeKind::Slice && !type.isMutable
                        && type.element == result_.types.builtin("string_view");
                    case OsValueKind::Bytes: return type.kind == TypeKind::Slice && !type.isMutable
                        && type.element == result_.types.builtin("byte");
                    case OsValueKind::MutableBytesLoan:
                    {
                        const auto& view = result_.types.get(type.element);
                        return type.kind == TypeKind::Reference && type.isMutable && view.kind == TypeKind::Slice
                            && view.isMutable && view.element == result_.types.builtin("byte");
                    }
                }
                return false;
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
                if (std::any_of(candidates.begin(), candidates.end(), [&](SymbolId id) { return dropMethod(id); }))
                {
                    error(node, "Drop cleanup methods cannot be called explicitly.");
                    return remember(node, result_.types.error());
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
                    if (declaration && erased_.contains(declaration) && !dynamicCall && !osBinding(candidate) && !cancellationBinding(candidate)) continue;
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
                                || (actual.kind == TypeKind::Reference ? actual.isMutable && !readOnlyProjection(*receiver) : writable(*receiver)));
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
                    const auto& parameterType = result_.types.get(wanted);
                    if (parameterType.kind == TypeKind::Reference && parameterType.isMutable && readOnlyProjection(value))
                        error(value, "Cannot reborrow a mutable reference through a read-only projection.");
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
                if (const auto* operation = cancellationBinding(best->symbol))
                {
                    if (!cancellationSignature(function, *operation)) error(node, "Trusted cancellation operation differs from its closed owner/result signature.");
                    else result_.valueIntrinsics[&node] = "cancel." + std::string(operation->name);
                }
                if (const auto* operation = osBinding(best->symbol))
                {
                    bool validSignature = !function.variadic && function.parameters.size() == operation->parameterCount;
                    for (std::size_t index = 0; validSignature && index < function.parameters.size(); ++index)
                        validSignature = osValueType(function.parameters[index], operation->parameters[index]);
                    const auto& returned = result_.types.get(function.result);
                    if (operation->unitResult) validSignature = validSignature && returned.kind == TypeKind::Unit;
                    else validSignature = validSignature && returned.kind == TypeKind::Tuple
                        && returned.parameters == std::vector<TypeId>{result_.types.builtin("i32"),
                            osPayloadType(*operation),
                            result_.types.builtin("i32"), result_.types.builtin("i32")};
                    if (!validSignature) error(node, "Trusted OS operation differs from its closed native signature.");
                    else result_.valueIntrinsics[&node] = "os." + std::string(operation->name);
                }
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
                                    const bool borrowedBinding = bound.kind == TypeKind::Reference && (explicitBorrow || bindingType != fieldType);
                                    if (borrowedBinding)
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
                                    result_.enumBindings[binding.get()] = {ids[0], fieldIds[0], consuming, writable, borrowedBinding};
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
                if (child(*declaration,"CompilerTask") || child(*declaration,"CompilerTaskScope") || child(*declaration,"CompilerTaskId") || child(*declaration, "CompilerCancellationSource") || child(*declaration, "CompilerCancellationToken"))
                { error(node, "Compiler-owned Task, Scope, diagnostic, and cancellation identities cannot be constructed with field initializers."); return remember(node, result_.types.error()); }
                if (syncMarker(*declaration) != TypeKind::Error)
                { error(node, "Compiler-owned Sync state and guards cannot be constructed with field initializers."); return remember(node, result_.types.error()); }
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
            bool parallelScalar(TypeId type) const
            {
                const auto kind = result_.types.get(type).kind;
                return kind == TypeKind::Bool || kind == TypeKind::Integer || kind == TypeKind::Float
                    || kind == TypeKind::Char || kind == TypeKind::Byte;
            }
            std::optional<TypeId> parallelIntrinsic(const Node& node)
            {
                if (node.children.empty()) return std::nullopt;
                const auto ids = references(*node.children[0]);
                if (ids.size() != 1 || ids[0] >= names_.symbols.size()) return std::nullopt;
                const auto* declaration = names_.symbols[ids[0]].declaration;
                const auto* marker = declaration ? child(*declaration, "CompilerParallel") : nullptr;
                if (!marker) return std::nullopt;
                if (marker->value == "worker_count")
                {
                    if (node.children.size() != 1) error(node, "parallel.worker_count takes no arguments.");
                    result_.parallelCalls[&node] = InvalidSymbolId;
                    return result_.types.builtin("usize");
                }
                if (node.children.size() != 3 || node.children[1]->children.size() != 1 || node.children[2]->children.size() != 1)
                { error(node, "parallel.for_each requires one MutSlice and one named static worker function."); return result_.types.error(); }
                if (!node.children[1]->value.empty() || !node.children[2]->value.empty())
                    error(node, "parallel.for_each uses positional arguments in 0.2.");
                const auto collection = expression(*node.children[1]->children[0]);
                const auto& slice = result_.types.get(collection);
                if (slice.kind != TypeKind::Slice || !slice.isMutable || !parallelScalar(slice.element))
                { error(node, "parallel.for_each requires MutSlice of a scalar numeric, bool, char, or byte type."); return result_.types.error(); }
                const auto& worker = *node.children[2]->children[0];
                const auto targets = references(worker);
                if ((worker.kind != "Name" && worker.kind != "Member") || targets.size() != 1 || targets[0] >= names_.symbols.size())
                { error(worker, "A parallel worker must name one non-overloaded static function."); return result_.types.error(); }
                const auto* target = names_.symbols[targets[0]].declaration;
                if (!target || target->kind != "Function" || child(*target, "CompilerIntrinsic")
                    || (child(*target, "GenericParameters") && !child(*target, "GenericParameters")->children.empty())
                    || (owners_.contains(target) && owners_.at(target)))
                { error(worker, "A parallel worker must be a concrete module-level function."); return result_.types.error(); }
                const auto workerType = signature(targets[0]);
                const auto wanted = result_.types.reference(slice.element, true);
                if (workerType.result != result_.types.builtin("void") || workerType.parameters.size() != 1
                    || workerType.parameters[0] != wanted || workerType.variadic)
                { error(worker, "A parallel worker must have signature void worker(&mut Element item)."); return result_.types.error(); }
                remember(worker, result_.symbolTypes.at(targets[0]));
                result_.parallelCalls[&node] = targets[0];
                return result_.types.builtin("unit");
            }
            bool parallelLocalType(TypeId type) const
            {
                const auto& description = result_.types.get(type);
                if (parallelScalar(type) || description.kind == TypeKind::Unit) return true;
                if (description.kind == TypeKind::Reference) return parallelScalar(description.element);
                return false;
            }
            bool containsDrop(TypeId type, std::unordered_set<TypeId>& visiting)
            {
                if (result_.dropMethods.contains(type)) return true;
                if (!visiting.insert(type).second) return false;
                const auto& value = result_.types.get(type);
                if (value.kind == TypeKind::Array || value.kind == TypeKind::Vector || value.kind == TypeKind::Box
                    || value.kind == TypeKind::Range || value.kind == TypeKind::Reference || value.kind == TypeKind::Slice)
                    return containsDrop(value.element, visiting);
                if (value.kind == TypeKind::Tuple)
                    for (const auto element : value.parameters) if (containsDrop(element, visiting)) return true;
                if (value.kind == TypeKind::Nominal)
                    if (const auto* owner = nominalDeclaration(type))
                        for (const auto& field : owner->children)
                        {
                            if (field->kind == "Variable" && containsDrop(symbolType(declarationId(*field)), visiting)) return true;
                            if (field->kind == "Variant")
                                for (const auto& payload : field->children)
                                    if (payload->kind == "Variable" && containsDrop(symbolType(declarationId(*payload)), visiting)) return true;
                        }
                return false;
            }
            void validateParallelWorkers()
            {
                std::unordered_set<SymbolId> visited;
                std::function<void(SymbolId, const Node&)> validate;
                validate = [&](SymbolId symbol, const Node& origin)
                {
                    if (!visited.insert(symbol).second) return;
                    const auto* declaration = symbol < names_.symbols.size() ? names_.symbols[symbol].declaration : nullptr;
                    if (!declaration || declaration->kind != "Function" || !child(*declaration, "Block")
                        || flag(*declaration, "extern") || child(*declaration, "CompilerIntrinsic"))
                    { error(origin, "Parallel workers cannot call external or unvalidated functions."); return; }
                    const auto function = signature(symbol);
                    const auto returned = result_.types.get(function.result).kind;
                    if (returned != TypeKind::Void && returned != TypeKind::Never && !parallelLocalType(function.result))
                        error(origin, "Parallel worker helpers may return only scalars, scalar borrows, unit, void, or never.");
                    std::unordered_set<SymbolId> locals;
                    std::function<void(const Node&)> collect = [&](const Node& node)
                    {
                        if (node.kind == "Variable" || node.kind == "Parameter")
                        {
                            const auto id = declarationId(node); locals.insert(id);
                            if (flag(node, "persist")) error(node, "Parallel workers cannot declare or access persist storage.");
                            const auto found = result_.symbolTypes.find(id);
                            if (found == result_.symbolTypes.end() || !parallelLocalType(found->second))
                                error(node, "Parallel worker locals and parameters are limited to scalars and scalar borrows in 0.2.");
                        }
                        for (const auto& item : node.children) collect(*item);
                    };
                    collect(*declaration);
                    std::function<void(const Node&)> inspect = [&](const Node& node)
                    {
                        if (const auto type = result_.nodeTypes.find(&node); type != result_.nodeTypes.end())
                        {
                            std::unordered_set<TypeId> visiting;
                            if (containsDrop(type->second, visiting))
                                error(node, "Parallel workers cannot create or use values with custom Drop cleanup in the current CPU parallel profile.");
                        }
                        if (node.kind == "Name" || node.kind == "Member")
                            for (const auto id : references(node))
                                if (id < names_.symbols.size() && names_.symbols[id].kind == SymbolKind::Value
                                    && names_.symbols[id].declaration && !locals.contains(id))
                                    error(node, "Parallel workers cannot access global, persist, or captured storage.");
                        if (node.kind == "Call")
                        {
                            if (const auto call = result_.calls.find(&node); call != result_.calls.end()) validate(call->second, node);
                            else if (const auto intrinsic = result_.intrinsics.find(&node); intrinsic == result_.intrinsics.end() || intrinsic->second != "never.unreachable")
                                error(node, "Parallel workers cannot invoke dynamic calls, nested parallel work, FFI, or unvalidated intrinsics.");
                        }
                        for (const auto& item : node.children) inspect(*item);
                    };
                    inspect(*child(*declaration, "Block"));
                };
                for (const auto& [call, worker] : result_.parallelCalls)
                    if (worker != InvalidSymbolId) validate(worker, *call);
            }
            bool textInput(TypeId type) const
            {
                const auto& input = result_.types.get(type);
                return input.kind == TypeKind::StringView || (input.kind == TypeKind::Reference
                    && (result_.types.get(input.element).kind == TypeKind::String || result_.types.get(input.element).kind == TypeKind::StringView));
            }
            // AEVIX_API_LIFECYCLE_BEGIN:array
            /*
             * 고정 배열을 읽기 대여하여 Copy 원소를 새 인라인 배열에 복사한다.
             * Borrow a fixed-size array and copy its Copy elements into a new inline array; arrays remain move-only.
             * @api Array.from_copy
             * @since 0.3
             * @deprecated_at none
             * @removed_at none
             */
            // AEVIX_API_LIFECYCLE_END:array
            std::optional<TypeId> valueIntrinsic(const Node& node, TypeId expected)
            {
                if (node.children.empty()) return std::nullopt;
                const Node* callee = node.children[0].get();
                const Node* arguments = nullptr;
                if (callee->kind == "GenericName") { arguments = child(*callee, "TypeArguments"); callee = callee->children[0].get(); }
                if (callee->kind != "Member" || callee->children.size() != 1 || callee->children[0]->kind != "Name") return std::nullopt;
                const auto& owner = *callee->children[0];
                if (!builtinMember(*callee, owner.value)) return std::nullopt;
                if (owner.value != "Tuple" && owner.value != "Array" && owner.value != "string" && owner.value != "Slice" && owner.value != "Byte") return std::nullopt;
                const auto& method = callee->value;
                const auto argument = [&](std::size_t index, TypeId context = InvalidTypeId)
                {
                    if (index >= node.children.size() || node.children[index]->children.empty())
                    { error(node, "Missing argument for " + owner.value + "." + method + "."); return result_.types.error(); }
                    return expression(*node.children[index]->children[0], context);
                };
                const auto done = [&](TypeId type, std::string operation)
                { result_.valueIntrinsics[&node] = std::move(operation); return type; };
                if (owner.value == "Array")
                {
                    if (method != "from_copy") return std::nullopt;
                    if (arguments || node.children.size() != 2)
                    { error(node, "Array.from_copy requires one explicit array borrow and no type arguments."); return result_.types.error(); }
                    if (!node.children[1]->value.empty())
                        error(node, "Array.from_copy requires one positional array borrow.");
                    const auto& reference = result_.types.get(argument(1));
                    const auto& array = result_.types.get(reference.element);
                    if (reference.kind != TypeKind::Reference || array.kind != TypeKind::Array)
                    { error(node, "Array.from_copy requires a borrowed fixed array."); return result_.types.error(); }
                    std::unordered_set<TypeId> visiting;
                    if (!copyType(array.element, visiting))
                        error(node, "Array.from_copy requires its element type to implement Copy.");
                    // 배열 자체는 이동 타입으로 유지하고 명시적인 원소 복사만 허용한다.
                    // Keep arrays move-only while permitting this explicit element-copy operation.
                    return done(reference.element, "array.copy");
                }
                /*
                 * Preserve all eight bits while constructing the distinct byte type from u8.
                 * @api Byte.from_u8
                 * @since 0.2
                 * @deprecated_at none
                 * @removed_at none
                 */
                /*
                 * Return the exact unsigned octet without allocation or numeric reinterpretation.
                 * @api Byte.to_u8
                 * @since 0.2
                 * @deprecated_at none
                 * @removed_at none
                 */
                if (owner.value == "Byte")
                {
                    if (method != "from_u8" && method != "to_u8") return std::nullopt;
                    if (arguments || node.children.size() != 2)
                    { error(node, "Byte conversion requires one value and no type arguments."); return result_.types.error(); }
                    if (!node.children[1]->value.empty() && node.children[1]->value != "value")
                    { error(node, "Unknown Byte conversion argument name; use value."); return result_.types.error(); }
                    const auto input = result_.types.builtin(method == "from_u8" ? "u8" : "byte");
                    require(argument(1, input), input, node, "Byte conversion");
                    return done(result_.types.builtin(method == "from_u8" ? "byte" : "u8"), "byte." + method);
                }
                if (owner.value == "Tuple")
                {
                    if (method == "make")
                    {
                        std::vector<TypeId> fields;
                        if (arguments) for (const auto& field : arguments->children) fields.push_back(resolveType(*field));
                        else if (result_.types.get(expected).kind == TypeKind::Tuple) fields = result_.types.get(expected).parameters;
                        if (!fields.empty() && fields.size() + 1 != node.children.size()) error(node, "Tuple.make field count does not match the contextual Tuple.");
                        const auto context = fields; fields.clear();
                        for (std::size_t index = 1; index < node.children.size(); ++index)
                        {
                            const auto actual = argument(index, index <= context.size() ? context[index - 1] : InvalidTypeId);
                            if (index <= context.size()) require(actual, context[index - 1], node, "Tuple field");
                            fields.push_back(actual);
                        }
                        return done(result_.types.tuple(fields), "tuple.make");
                    }
                    if (node.children.size() != 2) { error(node, "Tuple operations require one explicit tuple borrow."); return result_.types.error(); }
                    const auto borrowed = argument(1); const auto& reference = result_.types.get(borrowed);
                    if (reference.kind != TypeKind::Reference || result_.types.get(reference.element).kind != TypeKind::Tuple)
                    { error(node, "Tuple operations require &tuple or &mut tuple."); return result_.types.error(); }
                    const auto& fields = result_.types.get(reference.element).parameters;
                    if (method == "len" || method == "is_empty") return done(result_.types.builtin(method == "len" ? "usize" : "bool"), "tuple." + method);
                    if (method == "copy")
                    {
                        for (const auto field : fields) { std::unordered_set<TypeId> visiting; if (!copyType(field, visiting)) error(node, "Tuple.copy requires every field to implement Copy."); }
                        return done(reference.element, "tuple.copy");
                    }
                    if (method != "get" && method != "get_mut") return std::nullopt;
                    if (!arguments || arguments->children.size() != 1 || arguments->children[0]->kind != "ConstantTypeArgument")
                    { error(node, "Tuple.get requires one compile-time index: Tuple.get<0>(&value)."); return result_.types.error(); }
                    const auto index = extent(*arguments->children[0]);
                    if (!index || *index >= fields.size()) { error(node, "Tuple field index is out of bounds."); return result_.types.error(); }
                    if (method == "get_mut" && !reference.isMutable) error(node, "Tuple.get_mut requires an exclusive &mut tuple borrow.");
                    result_.tupleIndices[&node] = *index;
                    return done(result_.types.reference(fields[*index], method == "get_mut"), "tuple." + method);
                }
                if (owner.value == "Slice" && (method == "from_array" || method == "from_array_mut" || method == "from_vector" || method == "from_vector_mut"))
                {
                    if (node.children.size() != 2) { error(node, "Slice construction requires one collection borrow."); return result_.types.error(); }
                    const auto borrowed = argument(1); const auto& reference = result_.types.get(borrowed);
                    const bool mutableView = method.ends_with("_mut");
                    const auto& collection = result_.types.get(reference.element);
                    if (reference.kind != TypeKind::Reference || (mutableView && !reference.isMutable)
                        || collection.kind != (method.starts_with("from_array") ? TypeKind::Array : TypeKind::Vector))
                    { error(node, "Slice construction requires a compatible explicit collection borrow."); return result_.types.error(); }
                    return done(result_.types.slice(collection.element, mutableView), "slice.view");
                }
                if (owner.value != "string") return std::nullopt;
                const bool twoInputs = method == "equals";
                if (node.children.size() != (twoInputs ? 3u : 2u)) { error(node, "Incorrect argument count for string." + method + "."); return result_.types.error(); }
                const auto input = argument(1);
                if (method == "clear")
                {
                    const auto& reference = result_.types.get(input);
                    if (reference.kind != TypeKind::Reference || !reference.isMutable || result_.types.get(reference.element).kind != TypeKind::String)
                        error(node, "string.clear requires &mut string.");
                    return done(result_.types.builtin("unit"), "text.clear");
                }
                if (!textInput(input)) { error(node, "Text operations require string_view or an explicit borrow of string."); return result_.types.error(); }
                if (twoInputs && !textInput(argument(2))) error(node, "string.equals requires two text views or borrows.");
                if (method == "byte_len" || method == "scalar_count") return done(result_.types.builtin("usize"), "text." + method);
                if (method == "is_empty" || method == "equals") return done(result_.types.builtin("bool"), "text." + method);
                if (method == "view") return done(result_.types.builtin("string_view"), "text.view");
                if (method == "as_bytes") return done(result_.types.slice(result_.types.builtin("byte"), false), "text.as_bytes");
                return std::nullopt;
            }
            TypeId sliceExpression(const Node& node)
            {
                if (node.children.size() != 4) { error(node, "Slice slicing requires a view, start, and end."); return result_.types.error(); }
                const auto returned = resolveType(*node.children[0]);
                const auto carrier = result_.carriers.find(returned);
                if (carrier == result_.carriers.end()) { error(node, "Missing Slice Result specialization."); return result_.types.error(); }
                const auto view = expression(*node.children[1]); const auto& description = result_.types.get(view);
                if (description.kind != TypeKind::Slice || description.isMutable != (node.value == "slice_mut"))
                    error(node, "Slice.slice requires Slice<T>; Slice.slice_mut requires MutSlice<T>.");
                require(view, carrier->second.value, node, "Slice source view");
                for (std::size_t index = 2; index < 4; ++index)
                    require(expression(*node.children[index], result_.types.builtin("usize")), result_.types.builtin("usize"), node, "Slice boundary");
                return remember(node, returned);
            }
            TypeId textExpression(const Node& node)
            {
                const auto result = resolveType(*node.children[0]);
                const auto carrier = result_.carriers.find(result);
                if (carrier == result_.carriers.end()) { error(node, "Missing text Result specialization."); return result_.types.error(); }
                if (node.value.starts_with("system."))
                {
                    const auto method = node.value.substr(7);
                    if (method == "input")
                    {
                        const auto optional = result_.carriers.find(carrier->second.value);
                        if (node.children.size() != 1 || optional == result_.carriers.end() || !optional->second.optional)
                            error(node, "system.input requires a Result<Optional<T>, ReadError> carrier and no arguments.");
                    }
                    else for (std::size_t index = 1; index < node.children.size(); ++index)
                    {
                        const auto& part = *node.children[index];
                        const auto& source = part.kind == "CompilerFormatValue" || part.kind == "CompilerSystemArgument" ? *part.children[0] : part;
                        const auto actual = expression(source);
                        const auto& type = result_.types.get(actual);
                        const bool text = textInput(actual) || type.kind == TypeKind::String;
                        if (method == "log" && part.value == "level")
                        {
                            bool level = false;
                            for (const auto& symbol : names_.symbols)
                                if (symbol.declaration && symbol.declaration->kind == "Enum" && symbol.name == "LogLevel"
                                    && symbol.module < graph_.modules.size() && graph_.modules[symbol.module]->package == "$system"
                                    && symbolType(symbol.id) == actual) level = true;
                            if (!level) error(source, "system.log requires a system.LogLevel value.");
                            continue;
                        }
                        if (method == "log" && part.value == "code" && !text)
                            error(source, "system.log code must be text.");
                        if (!(text || type.kind == TypeKind::Integer || type.kind == TypeKind::Bool || type.kind == TypeKind::Char
                            || type.kind == TypeKind::Byte || (type.kind == TypeKind::Float && (type.bits == 32 || type.bits == 64))))
                            error(source, "system output supports text and scalar integer, f32/f64, bool, char, or byte values.");
                        if (type.kind == TypeKind::String) result_.systemBorrows[&source] = result_.types.reference(actual, false);
                        if (part.kind == "CompilerFormatValue")
                        {
                            const auto spec = part.value;
                            const bool matches = spec == "s" ? text : spec == "d" ? type.kind == TypeKind::Integer && type.isSigned
                                : spec == "u" ? type.kind == TypeKind::Integer && !type.isSigned
                                : spec == "f" || spec == "g" ? type.kind == TypeKind::Float && (type.bits == 32 || type.bits == 64)
                                : spec == "c" ? type.kind == TypeKind::Char : spec == "b" && type.kind == TypeKind::Bool;
                            if (!matches) error(source, "system.printf argument type does not match %" + spec + ".");
                            remember(part, actual);
                        }
                        if (part.kind == "CompilerSystemArgument") remember(part, actual);
                    }
                    result_.valueIntrinsics[&node] = node.value;
                    return remember(node, result);
                }
                if (node.value == "parse")
                {
                    if (node.children.size() != 2) { error(node, "Type.parse requires one text argument."); return result_.types.error(); }
                    const auto input = expression(*node.children[1]);
                    if (!textInput(input)) error(node, "Type.parse requires string_view or an explicit string borrow.");
                    const auto& target = result_.types.get(carrier->second.value);
                    if (!(target.kind == TypeKind::Integer || target.kind == TypeKind::Bool || target.kind == TypeKind::Char
                        || target.kind == TypeKind::Byte || (target.kind == TypeKind::Float && (target.bits == 32 || target.bits == 64))))
                        error(node, "Type.parse has an unsupported scalar destination.");
                    result_.valueIntrinsics[&node] = "text.parse";
                    return remember(node, result);
                }
                const bool mutation = node.value == "append" || node.value == "push_char";
                const auto expectedCount = node.value == "slice_bytes" ? 4u : mutation ? 3u : 2u;
                if (node.children.size() != expectedCount) { error(node, "Incorrect argument count for string." + node.value + "."); return result_.types.error(); }
                const auto input = expression(*node.children[1]);
                if (mutation)
                {
                    const auto& reference = result_.types.get(input);
                    if (reference.kind != TypeKind::Reference || !reference.isMutable || result_.types.get(reference.element).kind != TypeKind::String)
                        error(node, "String modification requires an exclusive &mut string borrow.");
                    const auto value = expression(*node.children[2], node.value == "push_char" ? result_.types.builtin("char") : InvalidTypeId);
                    if (node.value == "push_char") require(value, result_.types.builtin("char"), node, "Appended character");
                    else if (!textInput(value)) error(node, "string.append requires a text view.");
                }
                else if (node.value == "from_utf8")
                {
                    const auto& bytes = result_.types.get(input);
                    if (bytes.kind != TypeKind::Slice || bytes.element != result_.types.builtin("byte")) error(node, "string.from_utf8 requires Slice<byte>.");
                }
                else if (!textInput(input)) error(node, "Text construction and slicing require string_view or a string borrow.");
                if (node.value == "slice_bytes")
                    for (std::size_t index = 2; index < 4; ++index)
                        require(expression(*node.children[index], result_.types.builtin("usize")), result_.types.builtin("usize"), node, "Text byte boundary");
                result_.valueIntrinsics[&node] = "text." + node.value;
                return remember(node, result);
            }
            std::optional<TypeId> collectionIntrinsic(const Node& node, TypeId expected)
            {
                const auto* callee = node.children.empty() ? nullptr : node.children[0].get();
                if (!callee || callee->kind != "Member" || callee->children.size() != 1 || callee->children[0]->kind != "Name") return std::nullopt;
                const auto& owner = *callee->children[0];
                if (!builtinMember(*callee, owner.value)) return std::nullopt;
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

            // AEVIX_API_LIFECYCLE_BEGIN:simd
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.SimdMask.all
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.SimdMask.and
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.SimdMask.any
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.SimdMask.lanes
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.SimdMask.not
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.SimdMask.or
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.SimdMask.xor
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.add
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.div
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.eq
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.extract
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.from_array
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.ge
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.gt
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.lanes
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.le
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.load
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.lt
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.mul
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.ne
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.select
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.splat
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.sqrt
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.store
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.sub
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.sum
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            /*
             * 프로필 제한이 있는 공개 SIMD 내장 연산.
             * Public SIMD intrinsic subject to the implemented element/lane profile.
             * @api simd.Simd.to_array
             * @since 0.2
             * @deprecated_at none
             * @removed_at none
             */
            // AEVIX_API_LIFECYCLE_END:simd
            std::optional<TypeId> simdIntrinsic(const Node& node, TypeId expected)
            {
                if (node.children.empty()) return std::nullopt;
                const Node* callee = node.children[0].get();
                const Node* arguments = nullptr;
                if (callee->kind == "GenericName" && callee->children.size() == 2)
                { arguments = callee->children[1].get(); callee = callee->children[0].get(); }
                if (callee->kind != "Member" || callee->children.empty()) return std::nullopt;
                if (!references(*callee).empty()) return std::nullopt;
                const auto namespaceIds = references(*callee->children[0]);
                if (namespaceIds.size() != 1 || namespaceIds[0] >= names_.symbols.size()) return std::nullopt;
                const auto* declaration = names_.symbols[namespaceIds[0]].declaration;
                if (!declaration || !child(*declaration, "CompilerSimd")) return std::nullopt;
                const bool maskNamespace = declaration->value == "SimdMask";
                const auto& method = callee->value;
                for (std::size_t i = 1; i < node.children.size(); ++i)
                    if (!node.children[i]->value.empty()) error(node, "SIMD operations require positional arguments.");
                const auto count = node.children.size() - 1;
                const auto argument = [&](std::size_t index, TypeId context = InvalidTypeId)
                { return index < count ? expression(*node.children[index + 1]->children[0], context) : result_.types.error(); };
                const auto fail = [&](std::string message) -> std::optional<TypeId>
                { error(node, std::move(message)); return result_.types.error(); };
                const auto commit = [&](TypeId output, TypeId vector) -> std::optional<TypeId>
                {
                    result_.intrinsics[&node] = (maskNamespace ? "simd.mask." : "simd.") + method;
                    result_.simdTypes[&node] = vector;
                    return output;
                };
                const auto explicitVector = [&]() -> TypeId
                {
                    if (!arguments)
                        return result_.types.get(expected).kind == TypeKind::Simd ? expected : result_.types.error();
                    if (arguments->children.size() != 2) return result_.types.error();
                    const auto lanes = extent(*arguments->children[1]);
                    return lanes ? result_.types.simd(resolveType(*arguments->children[0]), *lanes) : result_.types.error();
                };
                if (maskNamespace)
                {
                    const bool unary = method == "any" || method == "all" || method == "not" || method == "lanes";
                    if ((!unary && method != "and" && method != "or" && method != "xor") || arguments)
                        return fail("Unknown SimdMask operation or unexpected type arguments.");
                    if (count != (unary ? 1u : 2u)) return fail("SimdMask argument count does not match the operation.");
                    const auto input = argument(0);
                    if (result_.types.get(input).kind != TypeKind::SimdMask) return fail("SimdMask operation requires a mask value.");
                    if (!unary) require(argument(1, input), input, node, "SIMD masks");
                    return commit(method == "any" || method == "all" ? result_.types.builtin("bool")
                        : method == "lanes" ? result_.types.builtin("usize") : input, input);
                }
                if (method == "splat" || method == "load")
                {
                    const auto vector = explicitVector();
                    if (!valid(vector)) return fail("Specify a valid SIMD destination type or explicit <element, lanes> arguments.");
                    const auto& type = result_.types.get(vector);
                    if (count != (method == "splat" ? 1u : 2u)) return fail("SIMD argument count does not match the operation.");
                    if (method == "splat") require(argument(0, type.element), type.element, node, "SIMD splat element");
                    else
                    {
                        const auto& reference = result_.types.get(argument(0));
                        const auto& array = result_.types.get(reference.element);
                        if (reference.kind != TypeKind::Reference || array.kind != TypeKind::Array || array.element != type.element)
                            return fail("Simd.load requires a borrowed fixed array with matching elements.");
                        require(argument(1, result_.types.builtin("usize")), result_.types.builtin("usize"), node, "SIMD offset");
                    }
                    return commit(vector, vector);
                }
                if (method == "from_array")
                {
                    if (count != 1 || arguments) return fail("Simd.from_array requires one borrowed fixed array.");
                    const auto& reference = result_.types.get(argument(0));
                    const auto& array = result_.types.get(reference.element);
                    if (reference.kind != TypeKind::Reference || array.kind != TypeKind::Array)
                        return fail("Simd.from_array requires a borrowed fixed array.");
                    const auto vector = result_.types.simd(array.element, array.extent);
                    if (!valid(vector)) return fail("The array has an unsupported SIMD element type or lane count.");
                    return commit(vector, vector);
                }
                if (method == "store")
                {
                    if (count != 3 || arguments) return fail("Simd.store requires a mutable array loan, offset, and SIMD value.");
                    const auto& reference = result_.types.get(argument(0));
                    const auto& array = result_.types.get(reference.element);
                    const auto vector = argument(2);
                    if (reference.kind != TypeKind::Reference || !reference.isMutable || array.kind != TypeKind::Array
                        || result_.types.get(vector).kind != TypeKind::Simd || array.element != result_.types.get(vector).element)
                        return fail("Simd.store requires a mutable fixed array with matching SIMD elements.");
                    require(argument(1, result_.types.builtin("usize")), result_.types.builtin("usize"), node, "SIMD offset");
                    return commit(result_.types.builtin("unit"), vector);
                }
                if (method == "select")
                {
                    if (count != 3 || arguments) return fail("Simd.select requires mask, true value, and false value.");
                    const auto mask = argument(0), vector = argument(1);
                    if (result_.types.get(vector).kind != TypeKind::Simd || result_.types.get(mask).kind != TypeKind::SimdMask
                        || result_.types.get(mask).extent != result_.types.get(vector).extent)
                        return fail("SIMD selection requires matching lane counts.");
                    require(argument(2, vector), vector, node, "SIMD selection alternatives");
                    return commit(vector, vector);
                }
                const bool binary = method == "add" || method == "sub" || method == "mul" || method == "div"
                    || method == "eq" || method == "ne" || method == "lt" || method == "le" || method == "gt" || method == "ge";
                const bool unary = method == "to_array" || method == "sum" || method == "sqrt" || method == "lanes";
                if ((!binary && !unary && method != "extract") || arguments) return fail("Unknown Simd operation or unexpected type arguments.");
                if (count != (binary || method == "extract" ? 2u : 1u)) return fail("SIMD argument count does not match the operation.");
                const auto vector = argument(0);
                const auto& type = result_.types.get(vector);
                if (type.kind != TypeKind::Simd) return fail("SIMD operation requires a SIMD value.");
                if (binary) require(argument(1, vector), vector, node, "SIMD operands");
                if (method == "extract") require(argument(1, result_.types.builtin("usize")), result_.types.builtin("usize"), node, "SIMD lane index");
                if (method == "sqrt" && result_.types.get(type.element).kind != TypeKind::Float) return fail("SIMD sqrt requires floating-point lanes.");
                if (method == "eq" || method == "ne" || method == "lt" || method == "le" || method == "gt" || method == "ge")
                    return commit(result_.types.simdMask(type.extent), vector);
                if (method == "sum" || method == "extract") return commit(type.element, vector);
                if (method == "lanes") return commit(result_.types.builtin("usize"), vector);
                if (method == "to_array") return commit(result_.types.array(type.element, type.extent), vector);
                return commit(vector, vector);
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
            bool atomicScalar(TypeId type) const
            {
                const auto& item = result_.types.get(type);
                return item.kind == TypeKind::Bool || (item.kind == TypeKind::Integer
                    && (item.bits == 8 || item.bits == 16 || item.bits == 32 || item.bits == 64));
            }
            bool standardNominal(TypeId type, std::string_view name, std::string_view package) const
            {
                const auto& item = result_.types.get(type);
                if (item.kind != TypeKind::Nominal || item.declaration >= names_.symbols.size()) return false;
                const auto& symbol = names_.symbols[item.declaration];
                return symbol.name == name && symbol.module < graph_.modules.size() && graph_.modules[symbol.module]->package == package;
            }
            std::optional<std::size_t> atomicOrderConstant(const Node& node, std::set<const Node*>& visiting)
            {
                if (!visiting.insert(&node).second) return std::nullopt;
                if (node.kind == "Group" && !node.children.empty()) return atomicOrderConstant(*node.children[0], visiting);
                if (const auto found = result_.enumConstructors.find(&node); found != result_.enumConstructors.end())
                    if (const auto variant = result_.enumVariants.find(found->second); variant != result_.enumVariants.end()) return variant->second.tag;
                const auto symbols = references(node);
                if (symbols.size() == 1 && symbols[0] < names_.symbols.size())
                    if (const auto* declaration = names_.symbols[symbols[0]].declaration;
                        declaration && declaration->kind == "Variable" && !flag(*declaration, "mut") && declaration->children.size() > 1)
                        return atomicOrderConstant(*declaration->children.back(), visiting);
                return std::nullopt;
            }
            TypeId syncExpression(const Node& node)
            {
                const auto* operation = syncOperation(node.value);
                if (!operation || node.children.size() != operation->operands + 2)
                { error(node, "Invalid compiler Sync operation shape."); return remember(node, result_.types.error()); }
                const auto output = resolveType(*node.children[0]);
                const auto payload = resolveType(*node.children[1]); remember(*node.children[1], payload);
                const auto kind = result_.types.get(payload).kind;
                if (kind == TypeKind::Error || kind == TypeKind::Void || kind == TypeKind::Never || kind == TypeKind::Dynamic)
                    error(node, "Inline lock payload requires a concrete storable value type.");
                for (std::size_t index = 0; index < operation->operands; ++index)
                {
                    const auto& argument = *node.children[index + 2];
                    const auto actual = expression(argument, operation->access == SyncAccess::None && index == 0 ? payload : InvalidTypeId);
                    if (index == 0 && operation->access != SyncAccess::None)
                    {
                        auto receiver = actual;
                        if (operation->access != SyncAccess::Value)
                        {
                            const auto& borrow = result_.types.get(actual);
                            if (borrow.kind != TypeKind::Reference || (operation->access == SyncAccess::Exclusive && !borrow.isMutable))
                                error(argument, "Sync operation requires an explicit compatible owner or guard borrow.");
                            if (operation->access == SyncAccess::Exclusive && readOnlyProjection(argument))
                                error(argument, "Cannot mutate a Sync guard or unique owner through a read-only projection.");
                            receiver = borrow.element;
                        }
                        const auto& owning = result_.types.get(receiver);
                        if (operation->kind == SyncKind::Poisoned)
                        {
                            const auto* declaration = nominalDeclaration(receiver);
                            bool valid = owning.kind == TypeKind::Nominal && declaration && child(*declaration, "CompilerPoisoned");
                            std::size_t fields = 0;
                            if (declaration) for (const auto& field : declaration->children) if (field->kind == "Variable")
                            { ++fields; valid = valid && symbolType(declarationId(*field)) == payload; }
                            if (!valid || fields != 1) error(argument, "Poison recovery requires the sealed Poisoned<T> carrier.");
                        }
                        else if (owning.kind != syncTypeKind(operation->kind) || owning.element != payload)
                            error(argument, "Sync operation receiver or payload does not match its sealed capability type.");
                    }
                    else if (operation->timed)
                    { if (!standardNominal(actual, "Duration", "$time")) error(argument, "Sync timeout requires time.Duration."); }
                    else if (index == 1)
                    { if (!standardNominal(actual, "PoisonPolicy", "$sync")) error(argument, "Sync policy requires sync.PoisonPolicy."); }
                    else require(actual, payload, argument, "Sync input");
                }
                return remember(node, output);
            }
            TypeId taskExpression(const Node& node)
            {
                const auto* operation = taskOperation(node.value);
                const auto* invoke = child(node,"CompilerTaskInvoke");
                if (!operation || node.children.size() != 3 + operation->operands + (operation->worker ? 1u : 0u)
                    || static_cast<bool>(invoke) != operation->worker)
                { error(node,"Invalid closed Task operation shape."); return remember(node,result_.types.error()); }
                const auto output = resolveType(*node.children[0]), payload = resolveType(*node.children[1]), callable = resolveType(*node.children[2]);
                const bool scopeAction = !operation->worker && operation->preparation == TaskPreparation::None && (operation->access == TaskAccess::None || operation->access == TaskAccess::ScopeValue || operation->access == TaskAccess::ScopeShared);
                if ((scopeAction || operation->access == TaskAccess::CancellationShared) && payload != result_.types.builtin("unit")) error(node,"Scope and cancellation checkpoints require unit payload metadata.");
                if (!operation->worker && callable != result_.types.builtin("unit")) error(node,"Non-worker Task operation has unexpected callable metadata.");
                for (std::size_t i = 0; i < operation->operands; ++i)
                {
                    const auto& argument = *node.children[3+i];
                    const auto actual = expression(argument, i == 1 && operation->preparation == TaskPreparation::Ready ? payload : InvalidTypeId);
                    if (i == 0 && operation->access != TaskAccess::None)
                    {
                        const bool byValue = operation->access == TaskAccess::ScopeValue || operation->access == TaskAccess::TaskValue;
                        const auto& borrow = result_.types.get(actual);
                        const auto receiver = byValue ? actual : borrow.element;
                        if (!byValue && (borrow.kind != TypeKind::Reference || (operation->access == TaskAccess::TaskExclusive && !borrow.isMutable)))
                            error(argument,"Task operation requires an explicit compatible scope or task borrow.");
                        if (operation->access == TaskAccess::TaskExclusive && readOnlyProjection(argument)) error(argument,"Cannot take a Task result through a read-only projection.");
                        const auto& owner = result_.types.get(receiver);
                        const bool scope = operation->access == TaskAccess::ScopeShared || operation->access == TaskAccess::ScopeValue;
                        const bool checkpoint = operation->access == TaskAccess::CancellationShared;
                        if (owner.kind != (checkpoint ? TypeKind::CancellationToken : scope ? TypeKind::TaskScope : TypeKind::Task) || (!scope && !checkpoint && owner.element != payload))
                            error(argument,"Task operation owner kind or application result does not match its metadata.");
                    }
                    else if (operation->worker) require(actual,callable,argument,"Task work capture");
                    else if (operation->preparation == TaskPreparation::Ready) require(actual,payload,argument,"Task ready value");
                    else if (node.value == "scope_open")
                    {
                        const auto& ref = result_.types.get(actual);
                        if (i == 0 ? ref.kind != TypeKind::Reference || !standardNominal(ref.element,"Executor","$task")
                            : !standardNominal(actual,"ScopeOptions","$task"))
                            error(argument,"Task.open_scope requires a borrowed task.Executor and task.ScopeOptions.");
                    }
                    else if (node.value == "wait_timeout" && !standardNominal(actual,"Duration","$time")) error(argument,"Task timeout requires time.Duration.");
                }
                if (operation->worker)
                {
                    const auto* declaration = nominalDeclaration(callable);
                    const auto ids = invoke && invoke->children.size() == 1 ? references(*invoke->children[0]) : std::vector<SymbolId>{};
                    bool valid = declaration && child(*declaration,"CompilerClosure") && ids.size() == 1 && ids[0] < names_.symbols.size();
                    if (valid)
                    {
                        const auto& target = signature(ids[0]);
                        const auto owner = owners_.find(target.node);
                        valid = target.node && target.node->value == "__invoke" && child(*target.node,"Block")
                            && owner != owners_.end() && owner->second == declaration && target.result == payload && target.parameters.size() == 1;
                        if (valid)
                        {
                            const auto& self = result_.types.get(target.parameters[0]);
                            valid = target.parameters[0] == callable || (self.kind == TypeKind::Reference && self.element == callable);
                        }
                        if (valid) result_.taskInvokes[&node] = ids[0];
                    }
                    if (!valid) error(node,"Owned Task work requires an exact concrete zero-argument compiler closure __invoke implementation.");
                }
                auto value = output;
                const bool fallible = taskFallible(operation->result);
                if (fallible)
                {
                    const auto carrier = result_.carriers.find(output);
                    if (carrier == result_.carriers.end() || carrier->second.optional)
                    { error(node,"Task operation requires an owned Result output."); return remember(node,result_.types.error()); }
                    value = carrier->second.value;
                    const auto expected = operation->result == TaskResult::Scope || operation->result == TaskResult::Spawn ? "SpawnError"
                        : operation->result == TaskResult::Close || operation->result == TaskResult::CloseAdmission ? "ScopeError" : operation->result == TaskResult::CheckCancel ? "Cancelled" : "TaskError";
                    if (!standardNominal(carrier->second.error,expected,expected == std::string_view("TaskError") ? "$error" : "$task")) error(node,"Task operation has the wrong canonical failure type.");
                }
                bool valid = true;
                switch (operation->result)
                {
                    case TaskResult::Scope: valid = result_.types.get(value).kind == TypeKind::TaskScope; break;
                    case TaskResult::Spawn: valid = result_.types.get(value).kind == TypeKind::Task && result_.types.get(value).element == payload; break;
                    case TaskResult::Join: valid = value == payload; break;
                    case TaskResult::TryTake: { const auto optional = result_.carriers.find(value); valid = optional != result_.carriers.end() && optional->second.optional && optional->second.value == payload; break; }
                    case TaskResult::Discard: valid = value == result_.types.builtin("unit"); break;
                    case TaskResult::Status: valid = standardNominal(value,"TaskStatus","$task"); break;
                    case TaskResult::Boolean: valid = value == result_.types.builtin("bool"); break;
                    case TaskResult::Token:
                    {
                        const auto& token = result_.types.get(value);
                        valid = token.kind == TypeKind::CancellationToken && token.declaration < names_.symbols.size();
                        if (valid)
                        {
                            const auto& declaration = names_.symbols[token.declaration];
                            valid = declaration.name == "CancellationToken" && declaration.module < graph_.modules.size()
                                && graph_.modules[declaration.module]->package == "$task" && declaration.declaration
                                && child(*declaration.declaration,"CompilerCancellationToken");
                        }
                        break;
                    }
                    case TaskResult::Executor: valid = standardNominal(value,"ExecutorId","$task"); break;
                    case TaskResult::Identity:
                    {
                        valid = standardNominal(value,"TaskId","$task");
                        const auto* identity = valid ? nominalDeclaration(value) : nullptr;
                        valid = valid && identity && child(*identity,"CompilerTaskId");
                        std::size_t fields = 0, markers = 0;
                        if (identity) for (const auto& member : identity->children) if (member->kind == "CompilerTaskId")
                        { ++markers; valid = valid && member->value.empty() && member->children.empty(); }
                        if (identity) for (const auto& field : identity->children) if (field->kind == "Variable")
                        {
                            ++fields; valid = valid && field->value == "serial" && field->children.size() == 2
                                && field->children[0]->kind == "Type" && field->children[0]->value == "u64" && field->children[0]->children.empty()
                                && field->children[1]->kind == "ArraySuffixes" && field->children[1]->children.empty() && !flag(*field,"public") && !flag(*field,"persist")
                                && !flag(*field,"mut") && symbolType(declarationId(*field)) == result_.types.builtin("u64");
                        }
                        valid = valid && fields == 1 && markers == 1; break;
                    }
                    case TaskResult::CloseAdmission: valid = value == result_.types.builtin("unit"); break;
                    case TaskResult::ActiveCount: valid = value == result_.types.builtin("usize"); break;
                    case TaskResult::ScopeState: valid = standardNominal(value,"ScopeStatus","$task"); break;
                    case TaskResult::Completion: case TaskResult::CheckCancel: valid = value == result_.types.builtin("unit"); break;
                    case TaskResult::Wait: valid = standardNominal(value,"WaitStatus","$task"); break;
                    case TaskResult::Request: valid = standardNominal(value,"TaskCancelRequest","$task"); break;
                    case TaskResult::Close: valid = standardNominal(value,"ScopeReport","$task"); break;
                }
                if (!valid) error(node,"Task operation output differs from its closed application-result contract.");
                return remember(node,output);
            }
            bool containsTaskStorage(TypeId type, std::unordered_set<TypeId>& seen)
            {
                if (!seen.insert(type).second) return false;
                const auto& item = result_.types.get(type);
                if (taskOwner(item.kind)) return true;
                if (item.element != InvalidTypeId && containsTaskStorage(item.element,seen)) return true;
                for (const auto field : item.parameters) if (containsTaskStorage(field,seen)) return true;
                if (const auto* declaration = nominalDeclaration(type))
                    for (const auto& field : declaration->children)
                    {
                        if (field->kind == "Variable" && containsTaskStorage(symbolType(declarationId(*field)),seen)) return true;
                        if (field->kind == "Variant") for (const auto& member : field->children)
                            if (member->kind == "Variable" && containsTaskStorage(symbolType(declarationId(*member)),seen)) return true;
                    }
                if (item.kind == TypeKind::Dynamic)
                {
                    for (const auto& [node,view] : result_.boxDynViews) { (void)node; if (view.interfaceType == type && containsTaskStorage(view.concreteType,seen)) return true; }
                    for (const auto& [node,view] : result_.dynViews) { (void)node; if (view.interfaceType == type && containsTaskStorage(view.concreteType,seen)) return true; }
                }
                return false;
            }
            void validateTaskGlobals()
            {
                const auto walk = [&](const auto& self, const Node& node, bool function, bool record) -> void
                {
                    if (node.kind == "Variable" && (flag(node,"persist") || (!function && !record)))
                    {
                        // 등록된 저장 타입을 검사하며 컴파일러 제공 상수의 초기화식을 다시 실행하지 않는다.
                        // Inspect registered storage types without rechecking compiler-provided constant initializers.
                        const auto id = declarationId(node);
                        const auto registered = result_.symbolTypes.find(id);
                        const auto storage = registered != result_.symbolTypes.end() ? registered->second
                            : !node.children.empty() && node.children[0]->kind != "InferredType" ? declaredType(node) : symbolType(id);
                        std::unordered_set<TypeId> seen;
                        if (containsTaskStorage(storage,seen)) error(node,"Task and TaskScope, including nested owners or views, cannot use global or persist storage in the owned-worker profile.");
                    }
                    const bool nestedFunction = function || node.kind == "Function";
                    const bool nestedRecord = record || node.kind == "Struct" || node.kind == "Enum" || node.kind == "Variant";
                    for (const auto& nested : node.children) self(self,*nested,nestedFunction,nestedRecord);
                };
                for (const auto& module : graph_.modules) walk(walk,*module->ast,false,false);
            }
            TypeId atomicExpression(const Node& node)
            {
                const auto* operation = atomicOperation(node.value);
                if (!operation || node.children.size() != operation->operands + 2)
                { error(node, "Invalid compiler Atomic operation shape."); return remember(node, result_.types.error()); }
                const auto output = resolveType(*node.children[0]);
                const auto scalar = resolveType(*node.children[1]); remember(*node.children[1], scalar);
                const bool query = operation->result == AtomicResult::Boolean || operation->result == AtomicResult::Alignment;
                if (!query && node.value != "fence" && node.value != "compiler_fence" && !atomicScalar(scalar))
                    error(node, "Atomic supports only bool and 8/16/32/64-bit CPU integers; no implicit emulation is provided.");
                if (operation->integerOnly && result_.types.get(scalar).kind != TypeKind::Integer)
                    error(node, "This Atomic arithmetic operation requires an integer cell.");
                for (std::size_t index = 0; index < operation->operands; ++index)
                {
                    const auto& argument = *node.children[index + 2];
                    const bool order = static_cast<int>(index) == operation->order || static_cast<int>(index) == operation->failure;
                    const bool cell = operation->cell && index == 0;
                    const bool timeout = node.value == "wait_timeout" && index == 2;
                    const auto actual = expression(argument, !order && !cell && !timeout ? scalar : InvalidTypeId);
                    if (cell)
                    {
                        auto cellType = actual;
                        if (node.value != "into_inner")
                        {
                            const auto& reference = result_.types.get(actual);
                            if (reference.kind != TypeKind::Reference || (node.value == "get_mut" && !reference.isMutable))
                                error(argument, "Atomic access requires an explicit compatible cell borrow.");
                            if (node.value == "get_mut" && readOnlyProjection(argument)) error(argument, "Cannot exclusively access Atomic through a read-only projection.");
                            cellType = reference.element;
                        }
                        const auto& owning = result_.types.get(cellType);
                        if (owning.kind != TypeKind::Atomic || owning.element != scalar)
                            error(argument, "Atomic cell scalar does not match the selected operation.");
                    }
                    else if (order)
                    {
                        if (!standardNominal(actual, "MemoryOrder", "$atomic")) error(argument, "Atomic order requires atomic.MemoryOrder.");
                        std::set<const Node*> visiting;
                        if (const auto constant = atomicOrderConstant(argument, visiting))
                        {
                            const bool load = node.value == "load" || node.value == "wait" || node.value == "wait_timeout" || static_cast<int>(index) == operation->failure;
                            const bool store = node.value == "store";
                            const bool fence = node.value == "fence" || node.value == "compiler_fence";
                            if (*constant > 4 || (load && *constant != 0 && *constant != 1 && *constant != 4)
                                || (store && *constant != 0 && *constant != 2 && *constant != 4) || (fence && *constant == 0))
                                error(argument, "Known invalid MemoryOrder for this Atomic operation.");
                        }
                    }
                    else if (timeout)
                    { if (!standardNominal(actual, "Duration", "$time")) error(argument, "Atomic.wait_timeout requires time.Duration."); }
                    else require(actual, scalar, argument, "Atomic scalar operand");
                }
                return remember(node, output);
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
                if (node.kind == "CompilerTaskExpression") return taskExpression(node);
                if (node.kind == "CompilerAtomicExpression") return atomicExpression(node);
                if (node.kind == "CompilerSyncExpression") return syncExpression(node);
                if (node.kind == "CompilerSliceExpression") return sliceExpression(node);
                if (node.kind == "CompilerTextExpression") return textExpression(node);
                if (node.kind == "CompilerMemoryExpression")
                {
                    if (node.children.size() != 3 || (node.value != "replace" && node.value != "swap"))
                    { error(node, "Invalid compiler memory operation."); return remember(node, result_.types.error()); }
                    const auto element = resolveType(*node.children[0]);
                    const auto elementKind = result_.types.get(element).kind;
                    if (elementKind == TypeKind::Error || elementKind == TypeKind::Void || elementKind == TypeKind::Never
                        || elementKind == TypeKind::Dynamic || elementKind == TypeKind::Function)
                        error(node, "memory operations require a concrete storable element type.");
                    const auto reference = result_.types.reference(element, true);
                    std::unordered_set<std::string> roles;
                    for (std::size_t index = 1; index < node.children.size(); ++index)
                    {
                        const auto& argument = *node.children[index];
                        const bool value = node.value == "replace" && argument.value == "replacement";
                        const auto required = value ? element : reference;
                        if (!roles.insert(argument.value).second || argument.children.size() != 1
                            || (node.value == "replace" ? argument.value != "target" && argument.value != "replacement"
                                : argument.value != "left" && argument.value != "right"))
                        { error(argument, "Invalid compiler memory argument."); continue; }
                        require(expression(*argument.children[0], required), required, argument, "memory operation argument");
                    }
                    return remember(node, node.value == "replace" ? element : result_.types.builtin("unit"));
                }
                // 반복 커서는 원본 설명자의 복사본이므로 이 조회는 원래 경계를 보존한다.
                // Iteration uses a copied cursor, so these inspections preserve original descriptor bounds.
                /*
                 * Return the original start bound, including when the descriptor is empty.
                 * @api Range.start
                 * @since 0.3
                 * @deprecated_at none
                 * @removed_at none
                 */
                /*
                 * Return the original end bound; inclusion is queried separately.
                 * @api Range.end
                 * @since 0.3
                 * @deprecated_at none
                 * @removed_at none
                 */
                /*
                 * Return the positive unsigned same-width magnitude without encoding direction in its sign.
                 * @api Range.step_magnitude
                 * @since 0.3
                 * @deprecated_at none
                 * @removed_at none
                 */
                /*
                 * Alias of step_magnitude with the same unsigned return type.
                 * @api Range.step
                 * @since 0.3
                 * @deprecated_at none
                 * @removed_at none
                 */
                /*
                 * Report whether the end bound is inclusive when reached exactly.
                 * @api Range.is_inclusive
                 * @since 0.3
                 * @deprecated_at none
                 * @removed_at none
                 */
                /*
                 * Report the declared direction independently of whether the range is empty.
                 * @api Range.is_descending
                 * @since 0.3
                 * @deprecated_at none
                 * @removed_at none
                 */
                /*
                 * Test directional bounds and inclusion without iteration or count arithmetic.
                 * @api Range.is_empty
                 * @since 0.3
                 * @deprecated_at none
                 * @removed_at none
                 */
                if (node.kind == "CompilerRangeInspectExpression")
                {
                    const auto* operation = rangeInspectOperation(node.value);
                    if (!operation || node.children.size() != 2)
                    { error(node,"Invalid closed Range descriptor inspection."); return remember(node,result_.types.error()); }
                    const auto output = resolveType(*node.children[0]);
                    const auto input = expression(*node.children[1]);
                    const auto& reference = result_.types.get(input);
                    const auto& descriptor = result_.types.get(reference.element);
                    const auto& element = result_.types.get(descriptor.element);
                    if (reference.kind != TypeKind::Reference || descriptor.kind != TypeKind::Range || element.kind != TypeKind::Integer)
                    { error(node,"Range inspection requires an explicit borrow of an integer Range descriptor."); return remember(node,result_.types.error()); }
                    const auto expected = operation->result == RangeInspectResult::Element ? descriptor.element
                        : operation->result == RangeInspectResult::Magnitude ? result_.types.builtin(element.name == "isize" || element.name == "usize" ? "usize" : "u" + std::to_string(element.bits))
                        : result_.types.builtin("bool");
                    require(output,expected,node,"Range inspection output");
                    return remember(node,output);
                }
                // 부동소수 표현의 모든 비트를 고정 폭 부호 없는 정수로 보존한다.
                /*
                 * Preserve every bit of a floating representation in its fixed-width unsigned counterpart.
                 * @api Float.to_bits
                 * @since 0.3
                 * @deprecated_at none
                 * @removed_at none
                 */
                // 명시한 부동소수 형식으로 같은 폭의 비트를 산술·할당 없이 복원한다.
                /*
                 * Restore same-width bits into an explicitly selected float format without arithmetic or allocation.
                 * @api Float.from_bits
                 * @since 0.3
                 * @deprecated_at none
                 * @removed_at none
                 */
                /*
                 * Convert an unsigned Unicode code point to a scalar, preserving rejected values in CharError.
                 * Surrogates and values above U+10FFFF fail; NUL and noncharacters remain valid.
                 * @api Char.from_u32
                 * @since 0.3
                 * @deprecated_at none
                 * @removed_at none
                 */
                /*
                 * Return the exact unsigned scalar value without allocation or normalization.
                 * @api Char.to_u32
                 * @since 0.3
                 * @deprecated_at none
                 * @removed_at none
                 */
                if (node.kind == "CompilerCharScalarExpression")
                {
                    if (node.children.size() != 2 || (node.value != "Char.from_u32" && node.value != "Char.to_u32"))
                    { error(node,"Invalid closed Char scalar conversion."); return remember(node,result_.types.error()); }
                    const bool from = node.value == "Char.from_u32";
                    const auto output = resolveType(*node.children[0]);
                    const auto inputType = result_.types.builtin(from ? "u32" : "char");
                    require(expression(*node.children[1],inputType),inputType,node,"Char scalar input");
                    if (!from) require(output,result_.types.builtin("u32"),node,"Char scalar output");
                    else
                    {
                        const auto carrier = result_.carriers.find(output);
                        bool valid = carrier != result_.carriers.end() && !carrier->second.optional
                            && carrier->second.value == result_.types.builtin("char");
                        if (valid)
                        {
                            const auto& errors = result_.types.get(carrier->second.error);
                            valid = errors.kind == TypeKind::Nominal && errors.name == "CharError"
                                && errors.declaration < names_.symbols.size()
                                && names_.symbols[errors.declaration].module < graph_.modules.size()
                                && graph_.modules[names_.symbols[errors.declaration].module]->name == "$core";
                        }
                        if (!valid) error(node,"Char.from_u32 requires the canonical Result<char,CharError> carrier.");
                    }
                    return remember(node,output);
                }
                if (node.kind == "CompilerFloatBitsExpression")
                {
                    if (node.children.size() != 2 || (node.value != "Float.to_bits" && node.value != "Float.from_bits"))
                    { error(node,"Invalid closed floating-point bit reinterpretation."); return remember(node,result_.types.error()); }
                    const auto output = resolveType(*node.children[0]);
                    const auto input = expression(*node.children[1]);
                    const bool toBits = node.value == "Float.to_bits";
                    const auto floating = toBits ? input : output;
                    const auto integer = toBits ? output : input;
                    const auto& format = result_.types.get(floating);
                    const auto counterpart = format.kind == TypeKind::Float && (format.bits == 16 || format.bits == 32 || format.bits == 64)
                        ? result_.types.builtin("u" + std::to_string(format.bits)) : result_.types.error();
                    if (format.kind != TypeKind::Float || counterpart == result_.types.error() || integer != counterpart)
                        error(node,"Float bit reinterpretation requires f16/bf16 with u16, f32 with u32, or f64 with u64; no numeric conversion is performed.");
                    return remember(node,output);
                }
                if (node.kind == "CompilerNumericExpression")
                {
                    const auto result = resolveType(*node.children[0]);
                    const auto carrier = result_.carriers.find(result);
                    const auto input = expression(*node.children[1]);
                    if (carrier == result_.carriers.end()) error(node, "Numeric conversion requires a core Result carrier.");
                    else
                    {
                        const auto output = carrier->second.value;
                        const auto sourceKind = result_.types.get(input).kind, destinationKind = result_.types.get(output).kind;
                        const bool integerSource = sourceKind == TypeKind::Integer;
                        const bool integerDestination = destinationKind == TypeKind::Integer;
                        if (node.value == "Integer.convert" ? !(integerSource && integerDestination)
                            : node.value == "Float.to_integer_exact" ? !(sourceKind == TypeKind::Float && integerDestination)
                            : !((integerSource || sourceKind == TypeKind::Float) && destinationKind == TypeKind::Float))
                            error(node, "Numeric conversion has incompatible source or destination domains.");
                    }
                    return remember(node, result);
                }
                if (node.kind == "CompilerSharedExpression")
                {
                    const auto output = resolveType(*node.children[0]);
                    const bool weak = node.value.starts_with("weak.");
                    const auto method = node.value.substr(node.value.find('.') + 1);
                    if (weak && method == "new")
                    { if (result_.types.get(output).kind != TypeKind::Weak) error(node, "Weak.new has an invalid payload type."); return remember(node, output); }
                    TypeId wanted = InvalidTypeId;
                    if (method == "new")
                    {
                        const auto carrier = result_.carriers.find(output);
                        if (carrier == result_.carriers.end() || result_.types.get(carrier->second.value).kind != TypeKind::Shared)
                        { error(node, "Shared.new requires Result<Shared<T>, InputError<T>>."); return remember(node, result_.types.error()); }
                        wanted = result_.types.get(carrier->second.value).element;
                        const auto kind = result_.types.get(wanted).kind;
                        if (kind == TypeKind::Void || kind == TypeKind::Never || kind == TypeKind::Error || kind == TypeKind::Function || kind == TypeKind::Dynamic)
                            error(node, "Shared payload requires a concrete storable value type.");
                    }
                    const auto input = expression(*node.children[1], wanted);
                    if (method == "new") require(input, wanted, node, "Shared input");
                    else
                    {
                        auto ownerType = input;
                        if (method != "try_unwrap")
                        {
                            const auto& reference = result_.types.get(input);
                            if (reference.kind != TypeKind::Reference || ((method == "get_mut" || method == "clear") && !reference.isMutable))
                                error(node, "Shared/Weak access requires a compatible explicit owner borrow.");
                            ownerType = reference.element;
                        }
                        if (result_.types.get(ownerType).kind != (weak ? TypeKind::Weak : TypeKind::Shared))
                            error(node, "Shared/Weak owner kind does not match the selected operation.");
                        if (method == "ptr_equal")
                        {
                            const auto other = expression(*node.children[2]);
                            if (result_.types.get(other).kind != TypeKind::Reference || result_.types.get(other).element != ownerType)
                                error(node, "Allocation identity requires borrows of the same Shared/Weak payload type.");
                        }
                    }
                    return remember(node, output);
                }
                if (node.kind == "CompilerBoxExpression")
                {
                    if (node.value == "into_inner" && node.children.size() != 2)
                    { error(node, "Box.into_inner requires exact result and owner metadata."); return remember(node, result_.types.error()); }
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
                    else if (node.value == "into_inner")
                    {
                        const auto& owner = result_.types.get(input);
                        const auto kind = result_.types.get(owner.element).kind;
                        if (owner.kind != TypeKind::Box || result != owner.element
                            || kind == TypeKind::Error || kind == TypeKind::Void || kind == TypeKind::Never || kind == TypeKind::Dynamic)
                            error(node, "Box.into_inner requires a sized owned Box<T> and returns exactly T; Box<dyn> cannot be extracted.");
                        else result_.boxExtractions[&node] = {input, owner.element};
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
                    if (dropMethod(ids[0]))
                    {
                        error(node, "Drop cleanup methods cannot be referenced or called explicitly.");
                        return remember(node, result_.types.error());
                    }
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
                    if (const auto intrinsic = simdIntrinsic(node, expected)) return remember(node, *intrinsic);
                    if (const auto intrinsic = parallelIntrinsic(node)) return remember(node, *intrinsic);
                    if (node.children.size() == 1 && node.children[0]->kind == "Member"
                        && node.children[0]->value == "unreachable" && builtinMember(*node.children[0], "Never"))
                    {
                        const auto namespaceIds = references(*node.children[0]->children[0]);
                        if (namespaceIds.size() == 1 && namespaceIds[0] < names_.symbols.size())
                        {
                            const auto& symbol = names_.symbols[namespaceIds[0]];
                            if (!symbol.declaration && symbol.kind == SymbolKind::Intrinsic && symbol.name == "Never")
                            {
                                result_.intrinsics[&node] = "never.unreachable";
                                return remember(node, result_.types.builtin("never"));
                            }
                        }
                    }
                    if (const auto intrinsic = valueIntrinsic(node, expected)) return remember(node, *intrinsic);
                    if (const auto intrinsic = collectionIntrinsic(node, expected)) return remember(node, *intrinsic);
                    if (node.children.size() == 1 && node.children[0]->kind == "Member" && node.children[0]->value == "value"
                        && builtinMember(*node.children[0], "Unit"))
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
                            error(node, "Vector mutation requires an explicit exclusive &mut Vector borrow.");
                        const bool input = callee.value == "push" || callee.value == "insert";
                        for (std::size_t i = 1; i + 1 < node.children.size(); ++i)
                        {
                            const auto& argument = *node.children[i];
                            if (!argument.value.empty()) error(argument, "Core Vector mutation currently uses positional arguments.");
                            if (i == 1) continue;
                            const auto wanted = input && i + 2 == node.children.size() ? vector.element : result_.types.builtin("usize");
                            require(expression(*argument.children[0], wanted), wanted, node, "Vector mutation input");
                        }
                        result_.collectionCalls[&node] = callee.value;
                        return remember(node, resolveType(*marker->children[0]));
                    }
                    const auto* collectionCallee = node.children.empty() ? nullptr : node.children[0].get();
                    if (collectionCallee && collectionCallee->kind == "Member" && (collectionCallee->value == "len" || collectionCallee->value == "capacity")
                        && (builtinMember(*collectionCallee, "Vector") || builtinMember(*collectionCallee, "Slice")))
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
                if (node.kind == "ArrayRepeat") return repeatedArray(node, expected);
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
                        && (constant.value.negative || constant.value.highMagnitude || constant.value.magnitude >= type.extent))
                        error(*node.children[1], "Constant array index is out of bounds.");
                    return adaptDynamic(node, type.element, expected);
                }
                if (node.kind == "Prefix")
                {
                    const auto& value = *node.children[0];
                    if ((node.value == "-" || node.value == "+") && numericLiteral(node)
                        && !floatingSpelling(numericLiteral(node)->value))
                    {
                        const auto suffix = numericSuffix(numericLiteral(node)->value);
                        auto type = !suffix.empty() ? result_.types.builtin(suffix) : integer(expected) ? expected : result_.types.builtin("i32");
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
                    if (result_.types.get(target).kind == TypeKind::Never) target = InvalidTypeId;
                    const auto left = expression(*node.children[1], target);
                    const auto right = expression(*node.children[2], target);
                    if (result_.types.get(left).kind == TypeKind::Never) return remember(node, right);
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
                const bool equality = operation == "==" || operation == "!=";
                const bool ordering = operation == "<" || operation == "<=" || operation == ">" || operation == ">=";
                // 비교 결과의 bool을 받을 정수 타입이 비교 피연산자의 리터럴 폭을 좁히지 않게 한다.
                // An integer destination for a bool comparison must not narrow its numeric literal operands.
                TypeId target = !equality && !ordering && numeric(expected) ? expected : InvalidTypeId;
                if (numericLiteral(leftNode) && !numericLiteral(rightNode)) target = expression(rightNode);
                else if (!numericLiteral(leftNode)) target = expression(leftNode);
                const auto left = expression(leftNode, target);
                const bool shift = operation == "<<" || operation == ">>";
                const auto right = expression(rightNode, shift ? InvalidTypeId : left);
                if (result_.types.get(left).kind == TypeKind::Never
                    || result_.types.get(right).kind == TypeKind::Never)
                    return remember(node, result_.types.builtin("never"));
                if (shift)
                {
                    if ((valid(left) && !integer(left)) || (valid(right) && !integer(right)))
                        error(node, "Shift operators require integer operands.");
                    return remember(node, left);
                }
                require(right, left, node, "Binary operands");
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
                        // 이 적응은 반환 경계에만 기록하며 bool 표현식의 타입과 일반 호환성을 바꾸지 않는다.
                        // Record adaptation only at the return boundary, preserving bool typing and general compatibility.
                        else if (result.kind == TypeKind::Integer && result_.types.get(value).kind == TypeKind::Bool)
                            result_.returnConversions[&node] = returnType_;
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
                    if (collection.kind == TypeKind::Array || collection.kind == TypeKind::Vector || collection.kind == TypeKind::Slice) element = collection.element;
                    else if (collection.kind == TypeKind::String || collection.kind == TypeKind::StringView) element = result_.types.builtin("char");
                    else if (valid(iterable)) error(node, "Enhanced for requires an Array, Vector, Slice, or text value.");
                    result_.iterationViews[&node] = collection.kind == TypeKind::String || collection.kind == TypeKind::StringView
                        ? result_.types.builtin("string_view") : result_.types.slice(element, false);
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
                if (erased_.contains(&declaration) || child(declaration,"CompilerTask") || child(declaration,"CompilerTaskScope") || child(declaration, "CompilerCancellationSource") || child(declaration, "CompilerCancellationToken") || syncMarker(declaration) != TypeKind::Error || child(declaration, "CompilerBox") || child(declaration, "CompilerShared") || child(declaration, "CompilerWeak") || child(declaration, "CompilerAtomic")) return;
                if (declaration.kind == "Import" || declaration.kind == "Modifier") return;
                if (declaration.kind == "TypeAlias") { if (!declaration.children.empty()) resolveType(*declaration.children[0]); return; }
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
