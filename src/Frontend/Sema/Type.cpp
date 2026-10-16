// ========================
//
// Created By : miruku_sora2026
//
// File name : Type.cpp
//
// Description :
//     기본 타입 별칭을 정규화하고 참조·함수·명목·정적 배열 타입을 재사용한다.
//     Normalize builtin aliases and intern reference, function, nominal, and fixed-array types.
//
// ========================

#include "aevix/Frontend/Sema/Type.hpp"

#include <algorithm>
#include <utility>

namespace aevix
{
    TypeContext::TypeContext()
    {
        Type errorType;
        errorType.kind = TypeKind::Error;
        errorType.name = "<error>";
        add(std::move(errorType));

        addBuiltin("void", TypeKind::Void);
        addBuiltin("never", TypeKind::Never);
        addBuiltin("unit", TypeKind::Unit);
        addBuiltin("bool", TypeKind::Bool);
        addBuiltin("i8", TypeKind::Integer, 8, true);
        addBuiltin("i16", TypeKind::Integer, 16, true);
        addBuiltin("i32", TypeKind::Integer, 32, true);
        addBuiltin("i64", TypeKind::Integer, 64, true);
        addBuiltin("i128", TypeKind::Integer, 128, true);
        addBuiltin("u8", TypeKind::Integer, 8);
        addBuiltin("u16", TypeKind::Integer, 16);
        addBuiltin("u32", TypeKind::Integer, 32);
        addBuiltin("u64", TypeKind::Integer, 64);
        addBuiltin("u128", TypeKind::Integer, 128);

        // 초기 대상인 ARM64와 x86-64에서는 포인터 크기 정수의 논리 폭이 64비트다.
        // Pointer-sized integers have a logical width of 64 bits on the initial ARM64 and x86-64 targets.
        addBuiltin("isize", TypeKind::Integer, 64, true);
        addBuiltin("usize", TypeKind::Integer, 64);
        addBuiltin("f16", TypeKind::Float, 16, true);
        addBuiltin("bf16", TypeKind::Float, 16, true);
        addBuiltin("f32", TypeKind::Float, 32, true);
        addBuiltin("f64", TypeKind::Float, 64, true);
        addBuiltin("char", TypeKind::Char);
        addBuiltin("byte", TypeKind::Byte, 8);
        addBuiltin("string", TypeKind::String);
        addBuiltin("string_view", TypeKind::StringView);

        builtins_.emplace("int", builtin("i32"));
        builtins_.emplace("float", builtin("f64"));
    }

    TypeId TypeContext::builtin(std::string_view name) const
    {
        const auto found = builtins_.find(std::string(name));
        return found == builtins_.end() ? error() : found->second;
    }

    TypeId TypeContext::error() const noexcept
    {
        return 0;
    }

    TypeId TypeContext::reference(TypeId element, bool isMutable)
    {
        if (get(element).kind == TypeKind::Error)
        {
            return error();
        }

        const auto key = std::make_pair(element, isMutable);
        if (const auto found = references_.find(key); found != references_.end())
        {
            return found->second;
        }

        Type type;
        type.kind = TypeKind::Reference;
        type.element = element;
        type.isMutable = isMutable;
        const TypeId id = add(std::move(type));
        references_.emplace(key, id);
        return id;
    }

    TypeId TypeContext::function(TypeId result, const std::vector<TypeId>& parameters)
    {
        if (get(result).kind == TypeKind::Error ||
            std::any_of(parameters.begin(), parameters.end(), [this](TypeId id)
            {
                return get(id).kind == TypeKind::Error;
            }))
        {
            return error();
        }

        const auto key = std::make_pair(result, parameters);
        if (const auto found = functions_.find(key); found != functions_.end())
        {
            return found->second;
        }

        Type type;
        type.kind = TypeKind::Function;
        type.result = result;
        type.parameters = parameters;
        const TypeId id = add(std::move(type));
        functions_.emplace(key, id);
        return id;
    }

    TypeId TypeContext::nominal(SymbolId declaration, std::string name)
    {
        if (declaration == InvalidSymbolId)
        {
            return error();
        }
        if (const auto found = nominals_.find(declaration); found != nominals_.end())
        {
            return found->second;
        }

        Type type;
        type.kind = TypeKind::Nominal;
        type.declaration = declaration;
        type.name = std::move(name);
        const TypeId id = add(std::move(type));
        nominals_.emplace(declaration, id);
        return id;
    }

    TypeId TypeContext::boxed(SymbolId declaration, std::string name, TypeId element)
    {
        const auto id = nominal(declaration, std::move(name));
        if (id != error()) { types_[id].kind = TypeKind::Box; types_[id].element = element; }
        return id;
    }

    TypeId TypeContext::dynamic(SymbolId declaration, std::string name)
    {
        const auto id = nominal(declaration, std::move(name));
        if (id != error()) types_[id].kind = TypeKind::Dynamic;
        return id;
    }

    TypeId TypeContext::array(TypeId element, std::size_t extent)
    {
        if (get(element).kind == TypeKind::Error)
        {
            return error();
        }

        const auto key = std::make_pair(element, extent);
        if (const auto found = arrays_.find(key); found != arrays_.end())
        {
            return found->second;
        }

        Type type;
        type.kind = TypeKind::Array;
        type.element = element;
        type.extent = extent;
        const TypeId id = add(std::move(type));
        arrays_.emplace(key, id);
        return id;
    }

    TypeId TypeContext::range(TypeId element)
    {
        if (get(element).kind != TypeKind::Integer) return error();
        if (const auto found = ranges_.find(element); found != ranges_.end()) return found->second;
        Type type; type.kind = TypeKind::Range; type.element = element;
        const auto id = add(std::move(type)); ranges_[element] = id; return id;
    }

    TypeId TypeContext::vector(TypeId element)
    {
        if (get(element).kind == TypeKind::Error) return error();
        if (const auto found = vectors_.find(element); found != vectors_.end()) return found->second;
        Type type; type.kind = TypeKind::Vector; type.element = element;
        const auto id = add(std::move(type)); vectors_[element] = id; return id;
    }

    TypeId TypeContext::slice(TypeId element, bool isMutable)
    {
        if (get(element).kind == TypeKind::Error || get(element).kind == TypeKind::Void) return error();
        const auto key = std::make_pair(element, isMutable);
        if (const auto found = slices_.find(key); found != slices_.end()) return found->second;
        Type type; type.kind = TypeKind::Slice; type.element = element; type.isMutable = isMutable;
        const auto id = add(std::move(type)); slices_[key] = id; return id;
    }

    const Type& TypeContext::get(TypeId id) const noexcept
    {
        return types_[id < types_.size() ? id : error()];
    }

    TypeId TypeContext::tuple(const std::vector<TypeId>& fields)
    {
        for (const auto field : fields)
            if (get(field).kind == TypeKind::Error || get(field).kind == TypeKind::Void || get(field).kind == TypeKind::Never) return error();
        if (const auto found = tuples_.find(fields); found != tuples_.end()) return found->second;
        Type type; type.kind = TypeKind::Tuple; type.parameters = fields;
        const auto id = add(std::move(type)); tuples_.emplace(fields, id); return id;
    }

    TypeId TypeContext::simd(TypeId element, std::size_t lanes)
    {
        const auto& scalar = get(element);
        if ((lanes != 2 && lanes != 4 && lanes != 8 && lanes != 16)
            || (scalar.kind != TypeKind::Integer && scalar.kind != TypeKind::Float)
            || (scalar.kind == TypeKind::Float && scalar.bits != 32 && scalar.bits != 64)
            || scalar.bits * lanes > 128) return error();
        const auto key = std::make_pair(element, lanes);
        if (const auto found = simd_.find(key); found != simd_.end()) return found->second;
        Type type; type.kind = TypeKind::Simd; type.element = element; type.extent = lanes;
        const auto result = add(std::move(type)); simd_[key] = result; return result;
    }

    TypeId TypeContext::simdMask(std::size_t lanes)
    {
        if (lanes != 2 && lanes != 4 && lanes != 8 && lanes != 16) return error();
        if (const auto found = simdMasks_.find(lanes); found != simdMasks_.end()) return found->second;
        Type type; type.kind = TypeKind::SimdMask; type.element = builtin("bool"); type.extent = lanes;
        const auto result = add(std::move(type)); simdMasks_[lanes] = result; return result;
    }

    std::string TypeContext::display(TypeId id) const
    {
        const Type& type = get(id);
        switch (type.kind)
        {
            case TypeKind::Simd: return "Simd<" + display(type.element) + ", " + std::to_string(type.extent) + ">";
            case TypeKind::SimdMask: return "SimdMask<" + std::to_string(type.extent) + ">";
            case TypeKind::Tuple:
            {
                std::string text = "Tuple<";
                for (std::size_t index = 0; index < type.parameters.size(); ++index)
                { if (index) text += ", "; text += display(type.parameters[index]); }
                return text + ">";
            }
            case TypeKind::Box:
                return "Box<" + display(type.element) + ">";
            case TypeKind::Dynamic:
                return "dyn " + type.name;
            case TypeKind::Reference:
                return (type.isMutable ? "&mut " : "&") + display(type.element);
            case TypeKind::Array:
                return "Array<" + display(type.element) + ", " + std::to_string(type.extent) + ">";
            case TypeKind::Range: return "Range<" + display(type.element) + ">";
            case TypeKind::Vector: return "Vector<" + display(type.element) + ">";
            case TypeKind::Slice: return std::string(type.isMutable ? "MutSlice<" : "Slice<") + display(type.element) + ">";
            case TypeKind::Function:
            {
                std::string text = display(type.result) + " (";
                for (std::size_t i = 0; i < type.parameters.size(); ++i)
                {
                    if (i != 0)
                    {
                        text += ", ";
                    }
                    text += display(type.parameters[i]);
                }
                return text + ")";
            }
            default:
                return type.name;
        }
    }

    TypeId TypeContext::add(Type type)
    {
        const TypeId id = types_.size();
        types_.push_back(std::move(type));
        return id;
    }

    void TypeContext::addBuiltin(std::string name, TypeKind kind, std::size_t bits, bool isSigned)
    {
        Type type;
        type.kind = kind;
        type.name = name;
        type.bits = bits;
        type.isSigned = isSigned;
        builtins_.emplace(std::move(name), add(std::move(type)));
    }
}
