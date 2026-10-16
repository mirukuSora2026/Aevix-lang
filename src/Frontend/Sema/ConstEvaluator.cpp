// ========================
//
// Created By : miruku_sora2026
//
// File name : ConstEvaluator.cpp
//
// Description :
//     호스트 정수 넘침 없이 상수 연산과 순환 의존성 및 계산 한도를 검사한다.
//     Evaluate constants without host integer overflow and check cycles and evaluation limits.
//
// ========================

#include "aevix/Frontend/Sema/ConstEvaluator.hpp"
#include "aevix/Frontend/Sema/NumericLiteral.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
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
        bool constantDeclaration(const Node& node)
        {
            const auto* modifier = child(node, "Modifier");
            return modifier && (" " + modifier->value + " ").find(" const ") != std::string::npos;
        }
        const Node* initializer(const Node& node)
        {
            for (std::size_t index = 1; index < node.children.size(); ++index)
                if (node.children[index]->kind != "ArraySuffixes" && node.children[index]->kind != "Modifier")
                    return node.children[index].get();
            return nullptr;
        }
        bool floatingSpelling(std::string_view text)
        {
            return numericFloating(text);
        }
        ConstEvaluation invalid(const Node& node, std::string message)
        {
            return {ConstStatus::Invalid, {}, node.range, std::move(message)};
        }
        ConstEvaluation value(ConstantValue constant)
        {
            return {ConstStatus::Constant, std::move(constant), {}, {}};
        }
        UInt128 mask(std::size_t bits) { return UInt128::mask(static_cast<unsigned>(bits)); }
        UInt128 representation(const ConstantValue& constant, std::size_t bits)
        {
            const auto magnitude = constant.integerMagnitude();
            return (constant.negative ? UInt128{} - magnitude : magnitude) & mask(bits);
        }
        ConstantValue fromRepresentation(UInt128 data, TypeId type, const Type& target)
        {
            ConstantValue result; result.kind = ConstantKind::Integer; result.type = type;
            data = data & mask(target.bits);
            result.negative = target.isSigned && static_cast<bool>(data & (UInt128{1} << (target.bits - 1)));
            result.setIntegerMagnitude(result.negative ? (UInt128{} - data) & mask(target.bits) : data);
            return result;
        }

        class Evaluation final
        {
        public:
            Evaluation(const NameResolution& names, const TypeContext& types,
                const std::unordered_map<const Node*, TypeId>& nodeTypes,
                const std::unordered_map<SymbolId, TypeId>& symbolTypes)
                : names_(names), types_(types), nodeTypes_(nodeTypes), symbolTypes_(symbolTypes) {}

            ConstEvaluation run(const Node& node, TypeId expected, std::size_t depth = 0)
            {
                if (++operations_ > 100000 || depth > 256)
                    return invalid(node, "Compile-time constant evaluation limit exceeded.");
                const auto found = nodeTypes_.find(&node);
                TypeId type = found != nodeTypes_.end() && types_.get(found->second).kind != TypeKind::Error ? found->second : expected;
                if (node.kind == "Literal" || node.kind == "ConstantTypeArgument") return literal(node, type);
                if (node.kind == "Group") return run(*node.children[0], type, depth + 1);
                if (node.kind == "Name" || node.kind == "Member" || node.kind == "Type" || node.kind == "QualifiedType")
                {
                    const auto reference = names_.references.find(&node);
                    if (reference == names_.references.end() || reference->second.size() != 1) return {};
                    const auto id = reference->second[0];
                    if (id >= names_.symbols.size()) return {};
                    const auto* declaration = names_.symbols[id].declaration;
                    if (!declaration || !constantDeclaration(*declaration)) return {};
                    if (!active_.insert(declaration).second) return invalid(node, "Cyclic compile-time constant dependency.");
                    const auto* initial = initializer(*declaration);
                    const auto declared = symbolTypes_.find(id);
                    auto answer = initial ? run(*initial, declared == symbolTypes_.end() ? type : declared->second, depth + 1) : ConstEvaluation{};
                    active_.erase(declaration);
                    return answer;
                }
                if (node.kind == "Conditional")
                {
                    const auto condition = run(*node.children[0], types_.builtin("bool"), depth + 1);
                    if (condition.status != ConstStatus::Constant) return condition;
                    if (condition.value.kind != ConstantKind::Boolean) return invalid(node, "A constant condition must be bool.");
                    return run(*node.children[condition.value.boolean ? 1 : 2], type, depth + 1);
                }
                if (node.kind == "Prefix")
                {
                    const auto& operand = *node.children[0];
                    // 최솟값의 양수 절댓값을 먼저 범위 검사하지 않는다.
                    // Do not range-check the positive magnitude of the signed minimum first.
                    if ((node.value == "-" || node.value == "+") && signedLiteral(operand))
                    {
                        auto answer = rawSigned(node, type);
                        if (answer.status == ConstStatus::Constant) return bounded(node, std::move(answer.value));
                        return answer;
                    }
                    auto answer = run(operand, type, depth + 1);
                    if (answer.status != ConstStatus::Constant) return answer;
                    auto result = answer.value;
                    if (node.value == "!" && result.kind == ConstantKind::Boolean) result.boolean = !result.boolean;
                    else if (node.value == "+" && (result.kind == ConstantKind::Integer || result.kind == ConstantKind::Float)) {}
                    else if (node.value == "-" && result.kind == ConstantKind::Integer)
                        result.negative = static_cast<bool>(result.integerMagnitude()) && !result.negative;
                    else if (node.value == "-" && result.kind == ConstantKind::Float) result.floating = -result.floating;
                    else if (node.value == "~" && result.kind == ConstantKind::Integer)
                    {
                        const auto& target = types_.get(result.type);
                        if (!target.bits || target.bits > 128) return invalid(node, "Invalid integer width in constant operation.");
                        result = fromRepresentation(~representation(result, target.bits), result.type, target);
                    }
                    else return {};
                    return bounded(node, std::move(result));
                }
                if (node.kind != "Binary") return {};
                const auto& operation = node.value;
                TypeId operandType = type;
                if (types_.get(type).kind == TypeKind::Bool) operandType = InvalidTypeId;
                auto left = run(*node.children[0], operandType, depth + 1);
                if (left.status != ConstStatus::Constant) return left;
                if ((operation == "&&" || operation == "||") && left.value.kind == ConstantKind::Boolean)
                {
                    if ((operation == "&&" && !left.value.boolean) || (operation == "||" && left.value.boolean)) return left;
                }
                const bool shift = operation == "<<" || operation == ">>";
                auto right = run(*node.children[1], shift ? InvalidTypeId : left.value.type, depth + 1);
                if (right.status != ConstStatus::Constant) return right;
                if (left.value.kind != right.value.kind) return invalid(node, "Constant operands have incompatible types.");
                if (operation == "==" || operation == "!=" || operation == "<" || operation == "<=" || operation == ">" || operation == ">=")
                    return compare(node, left.value, right.value);
                if (left.value.kind == ConstantKind::Boolean && (operation == "&&" || operation == "||"))
                {
                    left.value.boolean = operation == "&&" ? left.value.boolean && right.value.boolean : left.value.boolean || right.value.boolean;
                    return left;
                }
                if (left.value.kind == ConstantKind::Float) return floatBinary(node, left.value, right.value);
                if (left.value.kind != ConstantKind::Integer) return {};
                return integerBinary(node, left.value, right.value);
            }

        private:
            const NameResolution& names_;
            const TypeContext& types_;
            const std::unordered_map<const Node*, TypeId>& nodeTypes_;
            const std::unordered_map<SymbolId, TypeId>& symbolTypes_;
            std::unordered_set<const Node*> active_;
            std::size_t operations_ = 0;

            static bool signedLiteral(const Node& node)
            {
                if (node.kind == "Group" || (node.kind == "Prefix" && (node.value == "+" || node.value == "-")))
                    return !node.children.empty() && signedLiteral(*node.children[0]);
                return node.kind == "Literal" && !node.value.empty() && node.value[0] >= '0' && node.value[0] <= '9' && !floatingSpelling(node.value);
            }
            ConstEvaluation rawSigned(const Node& node, TypeId expected, bool negative = false)
            {
                if (node.kind == "Group") return rawSigned(*node.children[0], expected, negative);
                if (node.kind == "Prefix") return rawSigned(*node.children[0], expected, node.value == "-" ? !negative : negative);
                auto answer = integerLiteral(node, expected);
                answer.value.negative = negative && static_cast<bool>(answer.value.integerMagnitude());
                return answer;
            }
            ConstEvaluation integerLiteral(const Node& node, TypeId expected)
            {
                ConstantValue result;
                result.kind = ConstantKind::Integer;
                const auto kind = types_.get(expected).kind;
                result.type = kind == TypeKind::Integer || kind == TypeKind::Byte ? expected : types_.builtin("i32");
                const auto suffix = numericSuffix(node.value);
                if (!suffix.empty()) result.type = types_.builtin(suffix);
                std::string spelling(numericSpelling(node.value));
                spelling.erase(std::remove(spelling.begin(), spelling.end(), '_'), spelling.end());
                unsigned base = 10;
                std::size_t index = 0;
                if (spelling.size() > 2 && spelling[0] == '0')
                {
                    if (spelling[1] == 'x' || spelling[1] == 'X') { base = 16; index = 2; }
                    else if (spelling[1] == 'b' || spelling[1] == 'B') { base = 2; index = 2; }
                    else if (spelling[1] == 'o' || spelling[1] == 'O') { base = 8; index = 2; }
                }
                UInt128 parsed;
                if (!UInt128::parse(std::string_view(spelling).substr(index), parsed, base))
                    return invalid(node, "Compile-time integer literal overflow.");
                result.setIntegerMagnitude(parsed);
                return value(std::move(result));
            }
            ConstEvaluation literal(const Node& node, TypeId expected)
            {
                ConstantValue result;
                if (node.value == "true" || node.value == "false")
                {
                    result.kind = ConstantKind::Boolean; result.boolean = node.value == "true"; result.type = types_.builtin("bool");
                    return value(std::move(result));
                }
                if (node.value.starts_with('"') || node.value.starts_with('\''))
                {
                    result.kind = node.value.starts_with('"') ? ConstantKind::String : ConstantKind::Character;
                    result.type = types_.builtin(result.kind == ConstantKind::String ? "string_view" : "char");
                    result.text = node.value;
                    if (result.kind == ConstantKind::Character)
                    {
                        // 렉서가 검증한 단일 문자 이스케이프와 UTF-8을 정규화한다.
                        // Normalize lexer-validated single-character escapes and UTF-8.
                        const auto text = std::string_view(node.value).substr(1, node.value.size() - 2);
                        std::uint32_t scalar = 0;
                        if (text.starts_with('\\'))
                        {
                            switch (text[1])
                            {
                                case 'n': scalar = 10; break; case 'r': scalar = 13; break; case 't': scalar = 9; break;
                                case '0': scalar = 0; break; case '\\': scalar = '\\'; break; case '\'': scalar = '\''; break; case '"': scalar = '"'; break;
                                default:
                                {
                                    for (std::size_t index = 2; index < text.size(); ++index)
                                    {
                                        const char digit = text[index];
                                        if (digit == '{' || digit == '}') continue;
                                        scalar = scalar * 16 + static_cast<unsigned>(digit <= '9' ? digit - '0' : digit <= 'F' ? digit - 'A' + 10 : digit - 'a' + 10);
                                    }
                                }
                            }
                        }
                        else
                        {
                            const auto first = static_cast<unsigned char>(text[0]);
                            scalar = first < 128 ? first : first & (first < 224 ? 31 : first < 240 ? 15 : 7);
                            for (std::size_t index = 1; index < text.size(); ++index) scalar = (scalar << 6) | (static_cast<unsigned char>(text[index]) & 63);
                        }
                        result.magnitude = scalar;
                    }
                    return value(std::move(result));
                }
                if (!floatingSpelling(node.value))
                {
                    auto answer = integerLiteral(node, expected);
                    return answer.status == ConstStatus::Constant ? bounded(node, std::move(answer.value)) : answer;
                }
                std::string spelling;
                for (char character : numericSpelling(node.value)) if (character != '_') spelling += character;
                char* end = nullptr;
                result.kind = ConstantKind::Float;
                result.type = !numericSuffix(node.value).empty() ? types_.builtin(numericSuffix(node.value)) : types_.get(expected).kind == TypeKind::Float ? expected : types_.builtin("f64");
                result.floating = std::strtold(spelling.c_str(), &end);
                if (!end || *end || !std::isfinite(result.floating)) return invalid(node, "Invalid or out-of-range floating literal.");
                const auto& target = types_.get(result.type);
                const long double maximum = target.name == "f16" ? 65504.0L
                    : target.name == "bf16" ? 3.3895313892515354759e38L
                    : target.name == "f32" ? static_cast<long double>(std::numeric_limits<float>::max())
                    : static_cast<long double>(std::numeric_limits<double>::max());
                if (std::fabs(result.floating) > maximum) return invalid(node, "Out-of-range floating literal.");
                return bounded(node, std::move(result));
            }
            ConstEvaluation bounded(const Node& node, ConstantValue result)
            {
                const auto& type = types_.get(result.type);
                if (result.kind == ConstantKind::Integer)
                {
                    const auto bits = type.kind == TypeKind::Byte ? 8 : type.bits;
                    if (!bits || bits > 128) return invalid(node, "Invalid integer type in constant expression.");
                    const auto magnitude = result.integerMagnitude();
                    const auto bound = type.isSigned ? UInt128{1} << (bits - 1) : mask(bits);
                    if ((!type.isSigned && result.negative) || magnitude > bound
                        || (type.isSigned && !result.negative && magnitude == bound))
                        return invalid(node, "Compile-time integer overflow for " + types_.display(result.type) + ".");
                    if (!result.integerMagnitude()) result.negative = false;
                }
                if (result.kind == ConstantKind::Float)
                {
                    // 계산 중 생긴 무한대와 NaN은 IEEE 값으로 유지한다.
                    // Preserve computed infinity and NaN as IEEE values.
                    if (!std::isfinite(result.floating)) return value(std::move(result));
                    if (type.name == "f32" || type.name == "f64")
                    {
                        const bool narrow = type.name == "f32";
                        const long double maximum = narrow ? static_cast<long double>(std::numeric_limits<float>::max()) : static_cast<long double>(std::numeric_limits<double>::max());
                        if (std::fabs(result.floating) > maximum)
                        {
                            // 범위 밖 부동소수 변환 대신 반올림 경계를 직접 비교한다.
                            // Compare the rounding boundary instead of casting an out-of-range float.
                            const int exponent = narrow ? std::numeric_limits<float>::max_exponent : std::numeric_limits<double>::max_exponent;
                            const int precision = narrow ? std::numeric_limits<float>::digits : std::numeric_limits<double>::digits;
                            const long double boundary = maximum + std::ldexp(1.0L, exponent - precision - 1);
                            const long double rounded = std::fabs(result.floating) < boundary ? maximum : std::numeric_limits<long double>::infinity();
                            result.floating = std::copysign(rounded, result.floating);
                        }
                        else if (narrow) result.floating = static_cast<float>(result.floating);
                        else result.floating = static_cast<double>(result.floating);
                    }
                    else
                    {
                        // 16비트 형식은 명시적인 이진 유효숫자와 지수 범위로 반올림한다.
                        // Round 16-bit formats using their binary precision and exponent range.
                        const int precision = type.name == "bf16" ? 8 : 11;
                        const int minimumExponent = type.name == "bf16" ? -125 : -13;
                        const int maximumExponent = type.name == "bf16" ? 128 : 16;
                        int exponent = 0;
                        const auto fraction = std::frexp(std::fabs(result.floating), &exponent);
                        if (exponent > maximumExponent)
                        {
                            result.floating = std::copysign(std::numeric_limits<long double>::infinity(), result.floating);
                            return value(std::move(result));
                        }
                        const int effectiveExponent = std::max(exponent, minimumExponent);
                        const auto scaled = std::ldexp(fraction, exponent - effectiveExponent + precision);
                        const auto floor = std::floor(scaled);
                        const auto remainder = scaled - floor;
                        const auto rounded = floor + (remainder > 0.5L || (remainder == 0.5L && std::fmod(floor, 2.0L) != 0) ? 1 : 0);
                        result.floating = std::copysign(std::ldexp(rounded, effectiveExponent - precision), result.floating);
                        if (std::fabs(result.floating) >= std::ldexp(1.0L, maximumExponent))
                            result.floating = std::copysign(std::numeric_limits<long double>::infinity(), result.floating);
                    }
                }
                return value(std::move(result));
            }
            ConstEvaluation compare(const Node& node, const ConstantValue& left, const ConstantValue& right)
            {
                int order = 0;
                if (left.kind == ConstantKind::Integer)
                {
                    order = left.negative != right.negative ? (left.negative ? -1 : 1)
                        : left.integerMagnitude() == right.integerMagnitude() ? 0
                        : (left.integerMagnitude() < right.integerMagnitude() ? -1 : 1) * (left.negative ? -1 : 1);
                }
                else if (left.kind == ConstantKind::Float)
                {
                    ConstantValue result;
                    result.kind = ConstantKind::Boolean; result.type = types_.builtin("bool");
                    const auto& operation = node.value;
                    result.boolean = operation == "==" ? left.floating == right.floating : operation == "!=" ? left.floating != right.floating
                        : operation == "<" ? left.floating < right.floating : operation == "<=" ? left.floating <= right.floating
                        : operation == ">" ? left.floating > right.floating : left.floating >= right.floating;
                    return value(std::move(result));
                }
                else if (left.kind == ConstantKind::Boolean) order = static_cast<int>(left.boolean) - static_cast<int>(right.boolean);
                else if (left.kind == ConstantKind::Character) order = left.magnitude < right.magnitude ? -1 : left.magnitude > right.magnitude ? 1 : 0;
                else return {};
                ConstantValue result;
                result.kind = ConstantKind::Boolean; result.type = types_.builtin("bool");
                const auto& operation = node.value;
                result.boolean = operation == "==" ? order == 0 : operation == "!=" ? order != 0 : operation == "<" ? order < 0
                    : operation == "<=" ? order <= 0 : operation == ">" ? order > 0 : order >= 0;
                return value(std::move(result));
            }
            ConstEvaluation floatBinary(const Node& node, ConstantValue left, const ConstantValue& right)
            {
                if (node.value == "+") left.floating += right.floating;
                else if (node.value == "-") left.floating -= right.floating;
                else if (node.value == "*") left.floating *= right.floating;
                else if (node.value == "/")
                {
                    if (right.floating == 0)
                    {
                        if (left.floating == 0 || std::isnan(left.floating)) left.floating = std::numeric_limits<long double>::quiet_NaN();
                        else left.floating = std::copysign(std::numeric_limits<long double>::infinity(), std::signbit(left.floating) != std::signbit(right.floating) ? -1.0L : 1.0L);
                    }
                    else left.floating /= right.floating;
                }
                else return {};
                return bounded(node, std::move(left));
            }
            ConstEvaluation integerBinary(const Node& node, ConstantValue left, ConstantValue right)
            {
                const auto& operation = node.value;
                const auto& type = types_.get(left.type);
                auto a = left.integerMagnitude(), b = right.integerMagnitude();
                if (operation == "+" || operation == "-")
                {
                    if (operation == "-" && b) right.negative = !right.negative;
                    if (left.negative == right.negative)
                    {
                        const auto sum = a + b;
                        if (sum < a) return invalid(node, "Compile-time integer overflow.");
                        a = sum;
                    }
                    else if (a >= b) a = a - b;
                    else { a = b - a; left.negative = right.negative; }
                }
                else if (operation == "*")
                {
                    UInt128 product;
                    if (UInt128::multiply(a, b, product)) return invalid(node, "Compile-time integer overflow.");
                    a = product; left.negative = left.negative != right.negative;
                }
                else if (operation == "/" || operation == "%")
                {
                    if (!b) return invalid(node, "Division by zero in compile-time constant.");
                    if (type.isSigned && left.negative && a == (UInt128{1} << (type.bits - 1)) && right.negative && b == UInt128{1})
                        return invalid(node, "Compile-time integer overflow.");
                    if (operation == "/") { a = a / b; left.negative = left.negative != right.negative; }
                    else a = a % b;
                }
                else if (operation == "&" || operation == "|" || operation == "^")
                {
                    const auto x = representation(left, type.bits), y = representation(right, type.bits);
                    left = fromRepresentation(operation == "&" ? x & y : operation == "|" ? x | y : x ^ y, left.type, type);
                    return bounded(node, std::move(left));
                }
                else if (operation == "<<" || operation == ">>")
                {
                    if (right.negative || b.high || b.low >= type.bits) return invalid(node, "Invalid shift count in compile-time constant.");
                    const auto count = static_cast<unsigned>(b.low);
                    if (operation == "<<")
                    {
                        left = fromRepresentation(representation(left, type.bits) << count, left.type, type);
                        return bounded(node, std::move(left));
                    }
                    const bool remainder = static_cast<bool>(a & mask(count));
                    a = a >> count;
                    if (left.negative && remainder) a = a + UInt128{1};
                }
                else return {};
                left.setIntegerMagnitude(a);
                return bounded(node, std::move(left));
            }
        };
    }

    std::string ConstantValue::key() const
    {
        if (kind == ConstantKind::Integer) return "integer:" + integerSpelling();
        if (kind == ConstantKind::Boolean) return boolean ? "bool:true" : "bool:false";
        if (kind == ConstantKind::Character) return "char:" + std::to_string(magnitude);
        if (kind == ConstantKind::String) return "string:" + text;
        std::ostringstream stream;
        stream << "float:" << std::setprecision(std::numeric_limits<long double>::max_digits10) << floating;
        return stream.str();
    }
    ConstEvaluator::ConstEvaluator(const NameResolution& names, const TypeContext& types,
        const std::unordered_map<const Node*, TypeId>& nodeTypes,
        const std::unordered_map<SymbolId, TypeId>& symbolTypes)
        : names_(names), types_(types), nodeTypes_(nodeTypes), symbolTypes_(symbolTypes) {}
    ConstEvaluation ConstEvaluator::evaluate(const Node& node, TypeId expected) const
    {
        return Evaluation(names_, types_, nodeTypes_, symbolTypes_).run(node, expected);
    }
}
