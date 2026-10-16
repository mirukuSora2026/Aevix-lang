// ========================
//
// Created By : miruku_sora2026
//
// File name : SimdCodeGen.cpp
//
// Description :
//     CPU SIMD 연산, 마스크 및 안전한 배열 전송을 이식 가능한 LLVM 벡터 명령으로 출력한다.
//     Emit CPU SIMD arithmetic, masks, and checked array transfers as portable LLVM vector instructions.
//
// ========================

#include "aevix/Backend/LLVM/SimdCodeGen.hpp"

#include <cstdint>

namespace aevix
{
    void emitSimdInstruction(const AIRInstruction& item, const TypeContext& types,
                             const SimdCodeGenContext& context)
    {
        auto& output = context.output;
        const auto& vector = types.get(item.dynamicSignature);
        const auto& scalar = types.get(vector.element);
        const auto vectorType = context.type(item.dynamicSignature);
        const auto scalarType = context.type(vector.element);
        const auto maskType = "<" + std::to_string(vector.extent) + " x i1>";
        const auto result = context.value(item.result);
        const auto operand = [&](std::size_t index) { return context.value(item.operands[index]); };
        const auto method = item.operation.substr(item.operation.starts_with("simd.mask.") ? 10 : 5);
        const bool floating = scalar.kind == TypeKind::Float;
        const auto extract = [&](const std::string& input, std::size_t lane, const std::string& type)
        {
            const auto value = context.temporary();
            output << "  " << value << " = extractelement " << type << ' ' << input << ", i32 " << lane << '\n';
            return value;
        };
        const auto reduce = [&](const std::string& input, bool all)
        {
            std::string accumulated = all ? "true" : "false";
            for (std::size_t lane = 0; lane < vector.extent; ++lane)
            {
                const auto element = extract(input, lane, maskType);
                const auto combined = context.temporary();
                output << "  " << combined << " = " << (all ? "and" : "or") << " i1 " << accumulated << ", " << element << '\n';
                accumulated = combined;
            }
            return accumulated;
        };
        const auto arrayOffset = [&](std::size_t referenceIndex, const std::string& offset)
        {
            const auto& reference = types.get(context.valueType(item.operands[referenceIndex]));
            const auto& array = types.get(reference.element);
            if (array.extent < vector.extent) context.guard("true");
            else
            {
                const auto invalid = context.temporary();
                output << "  " << invalid << " = icmp ugt i64 " << offset << ", " << array.extent - vector.extent << '\n';
                context.guard(invalid);
            }
            const auto pointer = context.temporary();
            output << "  " << pointer << " = getelementptr " << scalarType << ", ptr " << operand(referenceIndex) << ", i64 " << offset << '\n';
            return pointer;
        };
        if (method == "lanes")
        {
            output << "  " << result << " = freeze i64 " << vector.extent << '\n';
            return;
        }
        if (item.operation.starts_with("simd.mask."))
        {
            if (method == "any" || method == "all")
            {
                const auto value = reduce(operand(0), method == "all");
                output << "  " << result << " = freeze i1 " << value << '\n';
            }
            else if (method == "not")
            {
                output << "  " << result << " = xor " << maskType << ' ' << operand(0) << ", <";
                for (std::size_t lane = 0; lane < vector.extent; ++lane)
                { if (lane) output << ", "; output << "i1 true"; }
                output << ">\n";
            }
            else output << "  " << result << " = " << method << ' ' << maskType << ' ' << operand(0) << ", " << operand(1) << '\n';
            return;
        }
        if (method == "splat")
        {
            const auto inserted = context.temporary();
            output << "  " << inserted << " = insertelement " << vectorType << " poison, " << scalarType << ' ' << operand(0) << ", i32 0\n"
                   << "  " << result << " = shufflevector " << vectorType << ' ' << inserted << ", " << vectorType
                   << " poison, <" << vector.extent << " x i32> zeroinitializer\n";
            return;
        }
        if (method == "load" || method == "from_array")
        {
            const auto pointer = arrayOffset(0, method == "load" ? operand(1) : "0");
            output << "  " << result << " = load " << vectorType << ", ptr " << pointer << ", align 1\n";
            return;
        }
        if (method == "store")
        {
            const auto pointer = arrayOffset(0, operand(1));
            output << "  store " << vectorType << ' ' << operand(2) << ", ptr " << pointer << ", align 1\n"
                   << "  " << result << " = freeze {} zeroinitializer\n";
            return;
        }
        if (method == "to_array")
        {
            const auto arrayType = context.type(item.type);
            std::string aggregate = "zeroinitializer";
            for (std::size_t lane = 0; lane < vector.extent; ++lane)
            {
                const auto value = extract(operand(0), lane, vectorType);
                const auto next = context.temporary();
                output << "  " << next << " = insertvalue " << arrayType << ' ' << aggregate << ", " << scalarType << ' ' << value << ", " << lane << '\n';
                aggregate = next;
            }
            output << "  " << result << " = freeze " << arrayType << ' ' << aggregate << '\n';
            return;
        }
        if (method == "extract")
        {
            const auto invalid = context.temporary();
            output << "  " << invalid << " = icmp uge i64 " << operand(1) << ", " << vector.extent << '\n';
            context.guard(invalid);
            output << "  " << result << " = extractelement " << vectorType << ' ' << operand(0) << ", i64 " << operand(1) << '\n';
            return;
        }
        if (method == "select")
        {
            output << "  " << result << " = select " << maskType << ' ' << operand(0) << ", " << vectorType << ' ' << operand(1)
                   << ", " << vectorType << ' ' << operand(2) << '\n';
            return;
        }
        if (method == "sum")
        {
            // 정수는 각 덧셈을 검사하고 실수는 왼쪽부터 합산해 재결합 오차를 피한다.
            // Check every integer addition and fold floats left to right without reassociation.
            std::string accumulated = floating ? "0.000000e+00" : "0";
            for (std::size_t lane = 0; lane < vector.extent; ++lane)
            {
                const auto value = extract(operand(0), lane, vectorType);
                const auto next = context.temporary();
                if (floating) output << "  " << next << " = fadd " << scalarType << ' ' << accumulated << ", " << value << '\n';
                else
                {
                    const auto intrinsic = "llvm." + std::string(scalar.isSigned ? "sadd" : "uadd") + ".with.overflow.i" + std::to_string(scalar.bits);
                    const auto pair = "{ " + scalarType + ", i1 }";
                    context.declarations.insert("declare " + pair + " @" + intrinsic + "(" + scalarType + ", " + scalarType + ")");
                    const auto checked = context.temporary(), overflow = context.temporary();
                    output << "  " << checked << " = call " << pair << " @" << intrinsic << '(' << scalarType << ' ' << accumulated << ", " << scalarType << ' ' << value << ")\n"
                           << "  " << next << " = extractvalue " << pair << ' ' << checked << ", 0\n"
                           << "  " << overflow << " = extractvalue " << pair << ' ' << checked << ", 1\n";
                    context.guard(overflow);
                }
                accumulated = next;
            }
            output << "  " << result << " = freeze " << scalarType << ' ' << accumulated << '\n';
            return;
        }
        if (method == "sqrt")
        {
            const auto intrinsic = "llvm.sqrt.v" + std::to_string(vector.extent) + "f" + std::to_string(scalar.bits);
            context.declarations.insert("declare " + vectorType + " @" + intrinsic + "(" + vectorType + ")");
            output << "  " << result << " = call " << vectorType << " @" << intrinsic << '(' << vectorType << ' ' << operand(0) << ")\n";
            return;
        }
        if (method == "eq" || method == "ne" || method == "lt" || method == "le" || method == "gt" || method == "ge")
        {
            const auto comparison = method == "eq" ? "eq" : method == "ne" ? "ne" : method == "lt" ? "lt" : method == "le" ? "le" : method == "gt" ? "gt" : "ge";
            const auto predicate = floating ? (method == "ne" ? "une" : std::string("o") + comparison)
                : (method == "eq" || method == "ne") ? std::string(comparison) : std::string(scalar.isSigned ? "s" : "u") + comparison;
            output << "  " << result << " = " << (floating ? "fcmp" : "icmp") << ' ' << predicate << ' ' << vectorType << ' ' << operand(0) << ", " << operand(1) << '\n';
            return;
        }
        if (!floating && method != "div")
        {
            const auto intrinsic = "llvm." + std::string(scalar.isSigned ? "s" : "u") + method + ".with.overflow.v"
                + std::to_string(vector.extent) + "i" + std::to_string(scalar.bits);
            const auto pair = "{ " + vectorType + ", " + maskType + " }";
            context.declarations.insert("declare " + pair + " @" + intrinsic + "(" + vectorType + ", " + vectorType + ")");
            const auto checked = context.temporary(), overflow = context.temporary();
            output << "  " << checked << " = call " << pair << " @" << intrinsic << '(' << vectorType << ' ' << operand(0) << ", " << vectorType << ' ' << operand(1) << ")\n"
                   << "  " << result << " = extractvalue " << pair << ' ' << checked << ", 0\n"
                   << "  " << overflow << " = extractvalue " << pair << ' ' << checked << ", 1\n";
            context.guard(reduce(overflow, false));
            return;
        }
        if (!floating && method == "div")
        {
            for (std::size_t lane = 0; lane < vector.extent; ++lane)
            {
                const auto right = extract(operand(1), lane, vectorType);
                const auto zero = context.temporary();
                output << "  " << zero << " = icmp eq " << scalarType << ' ' << right << ", 0\n";
                context.guard(zero);
                if (scalar.isSigned)
                {
                    const auto left = extract(operand(0), lane, vectorType);
                    const auto minimum = context.temporary(), negativeOne = context.temporary(), overflow = context.temporary();
                    output << "  " << minimum << " = icmp eq " << scalarType << ' ' << left << ", -" << (std::uint64_t(1) << (scalar.bits - 1)) << '\n'
                           << "  " << negativeOne << " = icmp eq " << scalarType << ' ' << right << ", -1\n"
                           << "  " << overflow << " = and i1 " << minimum << ", " << negativeOne << '\n';
                    context.guard(overflow);
                }
            }
        }
        output << "  " << result << " = " << (floating ? std::string("f") + method : scalar.isSigned ? "sdiv" : "udiv")
               << ' ' << vectorType << ' ' << operand(0) << ", " << operand(1) << '\n';
    }
}
