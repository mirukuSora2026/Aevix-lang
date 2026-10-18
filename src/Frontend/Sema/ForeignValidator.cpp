// ========================
//
// Created By : miruku_sora2026
//
// File name : ForeignValidator.cpp
//
// Description :
//     명시적인 C 함수 경계에서 불완전한 ABI와 서로 다른 외부 서명을 거부한다.
//     Reject unsupported ABIs and conflicting external signatures at explicit C function boundaries.
//
// ========================

#include "aevix/Frontend/Sema/ForeignValidator.hpp"
#include "aevix/Native/Contract.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace aevix
{
    namespace
    {
        const Node* child(const Node& node, const std::string& kind)
        {
            for (const auto& item : node.children)
                if (item->kind == kind) return item.get();
            return nullptr;
        }
        bool flag(const Node& node, const std::string& name)
        {
            const auto* modifiers = child(node, "Modifier");
            return modifiers && (" " + modifiers->value + " ").find(" " + name + " ") != std::string::npos;
        }
        bool scalar(const Type& type, bool result)
        {
            return (result && type.kind == TypeKind::Void)
                || (type.kind == TypeKind::Integer && (type.bits == 32 || type.bits == 64))
                || (type.kind == TypeKind::Float && (type.bits == 32 || type.bits == 64));
        }
        bool linkName(const std::string& name)
        {
            if (name.empty() || name == "main" || name.starts_with("aevix_")) return false;
            const auto letter = [](unsigned char c)
            {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
            };
            if (!letter(static_cast<unsigned char>(name[0]))) return false;
            return std::all_of(name.begin() + 1, name.end(), [&](unsigned char c)
            {
                return letter(c) || (c >= '0' && c <= '9');
            });
        }
    }

    bool validateForeignDeclarations(const NameResolution& names, const TypeCheckResult& typed,
                                     Diagnostics& diagnostics)
    {
        const auto before = diagnostics.size();
        std::unordered_map<std::string, TypeId> signatures;
        std::unordered_set<const Node*> members;
        std::function<void(const Node&, bool)> mark = [&](const Node& node, bool insideType)
        {
            if (insideType) members.insert(&node);
            const bool nested = insideType || node.kind == "Struct" || node.kind == "Enum" || node.kind == "Interface";
            for (const auto& item : node.children) mark(*item, nested);
        };
        for (const auto& symbol : names.symbols)
            if (symbol.declaration && (symbol.declaration->kind == "Struct" || symbol.declaration->kind == "Enum"
                                       || symbol.declaration->kind == "Interface")) mark(*symbol.declaration, false);
        for (const auto& symbol : names.symbols)
        {
            if (!symbol.declaration || !flag(*symbol.declaration, "extern")) continue;
            const auto& node = *symbol.declaration;
            const auto error = [&](const std::string& detail)
            {
                diagnostics.push_back({node.range, "C FFI: " + detail});
            };
            if (node.kind != "Function" || members.contains(&node))
            { error("extern declarations must be module-level function prototypes."); continue; }
            if (nativeBridgeOperation(node.value))
                error("native bridge operations are compiler-managed and cannot be declared through source extern.");
            else if (!linkName(node.value)) error("unsupported or reserved external symbol name '" + node.value + "'.");
            if (child(node, "Block")) error("external function declarations cannot have a body.");
            if (const auto* generic = child(node, "GenericParameters"); generic && !generic->children.empty())
                error("external functions cannot have generic parameters.");
            if (child(node, "Where") || child(node, "Constraints"))
                error("external functions cannot have generic constraints.");
            if (flag(node, "mut") || flag(node, "const") || flag(node, "persist") || flag(node, "override"))
                error("external functions only accept visibility and extern modifiers.");
            if (const auto* parameters = child(node, "Parameters"))
                for (const auto& parameter : parameters->children)
                    if (parameter->kind == "VariadicParameter" || parameter->value == "self" || child(*parameter, "Default"))
                        error("external functions cannot have variadic, self, or defaulted parameters.");
            const auto found = typed.symbolTypes.find(symbol.id);
            if (found == typed.symbolTypes.end()) continue;
            const auto& function = typed.types.get(found->second);
            if (function.kind != TypeKind::Function) continue;
            if (!scalar(typed.types.get(function.result), true))
                error("return type must be void, a 32/64-bit integer, f32, or f64 in this stage.");
            for (const auto parameter : function.parameters)
                if (!scalar(typed.types.get(parameter), false))
                    error("parameters must be 32/64-bit integers, f32, or f64 in this stage.");
            if (const auto previous = signatures.find(node.value); previous != signatures.end() && previous->second != found->second)
                error("external symbol '" + node.value + "' has conflicting C signatures.");
            else signatures.emplace(node.value, found->second);
        }
        return diagnostics.size() == before;
    }
}
