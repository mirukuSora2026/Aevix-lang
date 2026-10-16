// ========================
//
// Created By : miruku_sora2026
//
// File name : TypeLayout.cpp
//
// Description :
//     정렬 패딩, 고정 배열 크기와 구조체 값 순환을 안전하게 계산한다.
//     Safely compute alignment padding, fixed-array sizes, and recursive struct values.
//
// ========================

#include "aevix/Frontend/Sema/TypeLayout.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace aevix
{
    namespace
    {
        class Layout final
        {
        public:
            Layout(const TypeContext& types, const NameResolution& names,
                const std::unordered_map<SymbolId, TypeId>& symbolTypes)
                : types_(types), names_(names), symbolTypes_(symbolTypes)
            {
                for (const auto& symbol : names.symbols) if (symbol.declaration) ids_[symbol.declaration] = symbol.id;
            }
            TypeLayoutResult run(TypeId id, std::size_t depth = 0)
            {
                const auto cached = complete_.find(id);
                if (cached != complete_.end()) return {true, cached->second, {}, {}};
                const auto& type = types_.get(id);
                const Node* declaration = type.declaration < names_.symbols.size() ? names_.symbols[type.declaration].declaration : nullptr;
                if (depth > 256) return failure(declaration, "Type layout nesting limit exceeded.");
                if (active_.contains(id)) return failure(declaration, "Recursive by-value type has no finite layout; use a reference or an owning indirection.");
                TypeLayoutInfo result;
                if (type.kind == TypeKind::Error) return failure(declaration, "Cannot lay out an invalid type.");
                if (type.kind == TypeKind::Void) return failure(declaration, "void is a return marker and has no value layout.");
                if (type.kind == TypeKind::Never || type.kind == TypeKind::Unit) {}
                else if (type.kind == TypeKind::Reference || type.kind == TypeKind::Function)
                    result = {type.kind == TypeKind::Reference && types_.get(type.element).kind == TypeKind::Dynamic ? 16u : 8u, 8, {}};
                else if (type.kind == TypeKind::Dynamic) return failure(declaration, "dyn is unsized; use a borrowed view or Box owner.");
                else if (type.kind == TypeKind::Box) result = {types_.get(type.element).kind == TypeKind::Dynamic ? 16u : 8u, 8, {}};
                else if (type.kind == TypeKind::String || type.kind == TypeKind::Vector) result = {24, 8, {}};
                else if (type.kind == TypeKind::Slice) result = {16, 8, {}};
                else if (type.kind == TypeKind::StringView) result = {16, 8, {}};
                else if (type.kind == TypeKind::Slice) result = {16, 8, {}};
                else if (type.kind == TypeKind::Range)
                {
                    const auto element = run(type.element, depth + 1);
                    if (!element.succeeded) return element;
                    const auto alignment = element.layout.alignment;
                    const auto bytes = element.layout.size * 3 + 3;
                    result = {bytes + (alignment - bytes % alignment) % alignment, alignment, {}};
                }
                else if (type.kind == TypeKind::Simd || type.kind == TypeKind::SimdMask)
                {
                    const auto bytes = type.kind == TypeKind::SimdMask ? (type.extent + 7) / 8
                        : type.extent * ((types_.get(type.element).bits + 7) / 8);
                    result = {bytes, std::max<std::size_t>(1, bytes), {}};
                }
                else if (type.kind == TypeKind::Bool || type.kind == TypeKind::Byte) result = {1, 1, {}};
                else if (type.kind == TypeKind::Char) result = {4, 4, {}};
                else if (type.kind == TypeKind::Integer || type.kind == TypeKind::Float)
                {
                    result.size = (type.bits + 7) / 8;
                    result.alignment = std::min<std::size_t>(result.size, 16);
                    if (!result.alignment) return failure(declaration, "Invalid scalar width in type layout.");
                }
                else if (type.kind == TypeKind::Array)
                {
                    active_.insert(id);
                    auto element = run(type.element, depth + 1);
                    active_.erase(id);
                    if (!element.succeeded) return element;
                    result.alignment = element.layout.alignment;
                    if (type.extent && element.layout.size > maximum() / type.extent) return failure(declaration, "Fixed array storage size overflows the 64-bit target address space.");
                    result.size = element.layout.size * type.extent;
                }
                else if (type.kind == TypeKind::Tuple)
                {
                    active_.insert(id);
                    for (const auto field : type.parameters)
                    {
                        const auto layout = run(field, depth + 1);
                        if (!layout.succeeded) { active_.erase(id); return layout; }
                        const auto padding = (layout.layout.alignment - result.size % layout.layout.alignment) % layout.layout.alignment;
                        if (result.size > maximum() - padding || result.size + padding > maximum() - layout.layout.size)
                        { active_.erase(id); return failure(nullptr, "Tuple storage size overflows the target address space."); }
                        result.size += padding + layout.layout.size;
                        result.alignment = std::max(result.alignment, layout.layout.alignment);
                    }
                    const auto padding = (result.alignment - result.size % result.alignment) % result.alignment;
                    if (result.size > maximum() - padding) { active_.erase(id); return failure(nullptr, "Tuple tail padding overflows the target address space."); }
                    result.size += padding; active_.erase(id);
                }
                else if (type.kind == TypeKind::Nominal)
                {
                    if (!declaration || (declaration->kind != "Struct" && declaration->kind != "Enum"))
                        return failure(declaration, "This nominal type does not yet have a supported storage layout.");
                    active_.insert(id);
                    const auto fields = [&](const Node& owner, TypeLayoutInfo& payload) -> TypeLayoutResult
                    {
                        for (const auto& field : owner.children)
                        {
                            if (field->kind != "Variable") continue;
                            const auto foundId = ids_.find(field.get());
                            const auto foundType = foundId == ids_.end() ? symbolTypes_.end() : symbolTypes_.find(foundId->second);
                            if (foundType == symbolTypes_.end()) return failure(field.get(), "Missing field type during layout.");
                            auto fieldLayout = run(foundType->second, depth + 1);
                            if (!fieldLayout.succeeded) return fieldLayout;
                            const auto alignment = fieldLayout.layout.alignment;
                            const auto padding = (alignment - payload.size % alignment) % alignment;
                            if (payload.size > maximum() - padding || payload.size + padding > maximum() - fieldLayout.layout.size)
                                return failure(field.get(), "Aggregate storage size overflows the 64-bit target address space.");
                            payload.size += padding;
                            payload.fields.push_back({foundId->second, payload.size, foundType->second});
                            payload.size += fieldLayout.layout.size;
                            payload.alignment = std::max(payload.alignment, alignment);
                        }
                        const auto padding = (payload.alignment - payload.size % payload.alignment) % payload.alignment;
                        if (payload.size > maximum() - padding) return failure(&owner, "Aggregate tail padding overflows the 64-bit target address space.");
                        payload.size += padding;
                        return {true, payload, {}, {}};
                    };
                    if (declaration->kind == "Struct")
                    {
                        auto layout = fields(*declaration, result);
                        if (!layout.succeeded) { active_.erase(id); return layout; }
                    }
                    else
                    {
                        // enum은 u32 태그 뒤에 가장 큰 변형의 정렬된 저장공간을 둔다.
                        // An enum places aligned storage for its largest variant after a u32 tag.
                        result = {4, 4, {}};
                        std::size_t payloadSize = 0, payloadAlignment = 1;
                        std::vector<TypeLayoutInfo> variants;
                        for (const auto& variant : declaration->children)
                        {
                            if (variant->kind != "Variant") continue;
                            TypeLayoutInfo payload;
                            auto layout = fields(*variant, payload);
                            if (!layout.succeeded) { active_.erase(id); return layout; }
                            payloadSize = std::max(payloadSize, payload.size);
                            payloadAlignment = std::max(payloadAlignment, payload.alignment);
                            variants.push_back(std::move(payload));
                        }
                        const auto payloadOffset = 4 + (payloadAlignment - 4 % payloadAlignment) % payloadAlignment;
                        if (payloadSize > maximum() - payloadOffset)
                        { active_.erase(id); return failure(declaration, "Enum storage size overflows the 64-bit target address space."); }
                        result.size = payloadOffset + payloadSize;
                        result.alignment = std::max<std::size_t>(4, payloadAlignment);
                        const auto padding = (result.alignment - result.size % result.alignment) % result.alignment;
                        if (result.size > maximum() - padding)
                        { active_.erase(id); return failure(declaration, "Enum tail padding overflows the 64-bit target address space."); }
                        result.size += padding;
                        for (const auto& payload : variants)
                            for (const auto& field : payload.fields)
                                result.fields.push_back({field.field, payloadOffset + field.offset, field.type});
                    }
                    active_.erase(id);
                }
                complete_[id] = result;
                return {true, std::move(result), {}, {}};
            }
        private:
            const TypeContext& types_;
            const NameResolution& names_;
            const std::unordered_map<SymbolId, TypeId>& symbolTypes_;
            std::unordered_map<const Node*, SymbolId> ids_;
            std::unordered_set<TypeId> active_;
            std::unordered_map<TypeId, TypeLayoutInfo> complete_;
            static constexpr std::size_t maximum() { return std::numeric_limits<std::size_t>::max(); }
            static TypeLayoutResult failure(const Node* node, std::string message)
            {
                return {false, {}, node ? node->range : SourceRange{}, std::move(message)};
            }
        };
    }
    TypeLayout::TypeLayout(const TypeContext& types, const NameResolution& names,
        const std::unordered_map<SymbolId, TypeId>& symbolTypes, LayoutTarget target)
        : types_(types), names_(names), symbolTypes_(symbolTypes), target_(target) {}
    TypeLayoutResult TypeLayout::layout(TypeId type) const
    {
        // 두 초기 대상은 같은 64비트 언어 내부 배치를 사용한다.
        // Both initial targets use the same 64-bit internal language layout.
        (void)target_;
        return Layout(types_, names_, symbolTypes_).run(type);
    }
}
