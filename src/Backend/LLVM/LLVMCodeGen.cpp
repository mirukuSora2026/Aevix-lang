// ========================
//
// Created By : miruku_sora2026
//
// File name : LLVMCodeGen.cpp
//
// Description :
//     AIR를 LLVM IR로 출력하며 정수 오류와 범위 오류 전에 안전하게 중단한다.
//     Emit LLVM IR from AIR and stop safely before integer or bounds errors.
//
// ========================
#include "aevix/Backend/LLVM/LLVMCodeGen.hpp"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace aevix
{
    namespace
    {
        class Emitter final
        {
        public:
            Emitter(const AIRModule& module, const TypeCheckResult& typed, const NameResolution& names,
                    Diagnostics& diagnostics, std::string triple)
                : module_(module), typed_(typed), names_(names), diagnostics_(diagnostics), triple_(std::move(triple)) {}
            std::optional<std::string> run()
            {
                const auto before = diagnostics_.size();
                if (!triple_.empty())
                {
                    if (triple_.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") != std::string::npos)
                    { error(nullptr, "invalid target triple."); return std::nullopt; }
                    output_ << "target triple = \"" << triple_ << "\"\n";
                }
                for (const auto& record : module_.records)
                {
                    output_ << llvmType(record.type) << " = type { ";
                    if (record.isEnum)
                    {
                        const auto layout = enumLayout(record.type);
                        output_ << "i32, [" << layout.words << " x i" << layout.alignment * 8 << "]";
                    }
                    else for (std::size_t index = 0; index < record.fieldTypes.size(); ++index)
                    { if (index) output_ << ", "; output_ << llvmType(record.fieldTypes[index]); }
                    output_ << " }\n";
                }
                for (const auto& global : module_.globals)
                {
                    std::unordered_set<const Node*> visiting;
                    const auto constant = global.isPersist ? std::string("zeroinitializer")
                        : global.initializer ? constantExpression(*global.initializer, global.type, visiting) : std::string("zeroinitializer");
                    output_ << "@aevix.global." << global.symbol << " = internal " << (global.isConstant ? "constant " : "global ") << llvmType(global.type) << ' ' << constant << '\n';
                    if (global.isPersist)
                    {
                        hasPersist_ = true;
                        output_ << "@persist.guard." << global.symbol << " = internal global ptr null\n";
                        const auto name = global.initializationOrderKey.empty() ? global.name : global.initializationOrderKey;
                        output_ << "@persist.name." << global.symbol << " = private constant [" << name.size() + 1 << " x i8] c\"" << escapedString(name) << "\\00\"\n";
                    }
                }
                std::unordered_map<std::string, const AIRExternal*> externalNames;
                for (const auto& foreign : module_.externals)
                {
                    if (foreign.variadic || foreign.callingConvention != "C") error(nullptr, "unsupported external calling convention.");
                    if (const auto previous = externalNames.find(foreign.name); previous != externalNames.end())
                    {
                        if (previous->second->result != foreign.result || previous->second->parameters != foreign.parameters)
                            error(nullptr, "conflicting signatures for external symbol '" + foreign.name + "'.");
                        continue;
                    }
                    externalNames.emplace(foreign.name, &foreign);
                    output_ << "declare " << llvmType(foreign.result) << ' ' << foreignName(foreign.name) << '(';
                    for (std::size_t index = 0; index < foreign.parameters.size(); ++index)
                    { if (index) output_ << ", "; output_ << llvmType(foreign.parameters[index]); }
                    output_ << ")\n";
                }
                if (hasPersist_)
                {
                    output_ << "@aevix.persist.domain = internal global ptr null\n"
                            << "declare void @aevix_persist_ensure(ptr, ptr, ptr, ptr, ptr)\n"
                            << "declare void @aevix_persist_shutdown(ptr)\n";
                }
                std::set<std::pair<TypeId, TypeId>> emittedTables;
                std::vector<const DynViewInfo*> dynamicTables;
                for (const auto& [node, information] : typed_.dynViews) { (void)node; dynamicTables.push_back(&information); }
                for (const auto& [node, information] : typed_.boxDynViews) { (void)node; dynamicTables.push_back(&information); }
                for (const auto* table : dynamicTables)
                {
                    const auto& information = *table;
                    if (typed_.types.get(information.concreteType).kind == TypeKind::Dynamic
                        || !emittedTables.emplace(information.interfaceType, information.concreteType).second) continue;
                    output_ << "@aevix.dyn.vtable." << information.interfaceType << '.' << information.concreteType
                            << " = private constant [" << information.methods.size() + 1 << " x ptr] [ptr @aevix.box.payload.drop." << information.concreteType;
                    payloadDropTypes_.insert(information.concreteType);
                    for (std::size_t index = 0; index < information.methods.size(); ++index)
                        output_ << ", ptr @aevix.fn." << information.methods[index];
                    output_ << "]\n";
                }
                for (const auto& function : module_.functions) emitFunction(function);
                emitModuleLifecycle();
                emitEntry();
                emitReserves();
                emitPayloadDrops();
                emitDrops();
                if (usesAllocation_)
                {
                    for (const auto& foreign : module_.externals)
                        if (foreign.name == "malloc" || foreign.name == "free") error(nullptr, "Foreign malloc/free declarations conflict with compiler-managed collection allocation.");
                    output_ << "declare ptr @malloc(i64)\ndeclare void @free(ptr)\n";
                }
                if (!reserveTypes_.empty()) output_ << "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)\n";
                output_ << "declare void @llvm.trap()\n";
                for (const auto& intrinsic : overflowIntrinsics_) output_ << intrinsic << '\n';
                if (diagnostics_.size() != before) return std::nullopt;
                return output_.str();
            }
        private:
            const AIRModule& module_;
            const TypeCheckResult& typed_;
            const NameResolution& names_;
            Diagnostics& diagnostics_;
            std::string triple_;
            std::ostringstream output_;
            std::set<std::string> overflowIntrinsics_;
            const AIRFunction* function_ = nullptr;
            std::size_t temporary_ = 0;
            std::size_t label_ = 0;
            bool hasPersist_ = false;
            bool usesAllocation_ = false;
            std::set<TypeId> dropTypes_;
            std::set<TypeId> reserveTypes_;
            std::set<TypeId> payloadDropTypes_;
            void error(const Node* source, const std::string& message)
            {
                diagnostics_.push_back({source ? source->range : SourceRange{}, "LLVM emission: " + message});
            }
            std::string llvmType(TypeId id)
            {
                const auto& type = typed_.types.get(id);
                switch (type.kind)
                {
                    case TypeKind::Void: return "void";
                    case TypeKind::Never: return "void";
                    case TypeKind::Bool: return "i1";
                    case TypeKind::Char: return "i32";
                    case TypeKind::Byte: return "i8";
                    case TypeKind::Integer: return "i" + std::to_string(type.bits);
                    case TypeKind::Float:
                        if (type.bits == 32) return "float";
                        if (type.bits == 64) return "double";
                        error(nullptr, "f16/bf16 code generation requires a later numeric lowering."); return "half";
                    case TypeKind::Reference: return typed_.types.get(type.element).kind == TypeKind::Dynamic ? "{ ptr, ptr }" : "ptr";
                    case TypeKind::Box: return typed_.types.get(type.element).kind == TypeKind::Dynamic ? "{ ptr, ptr }" : "ptr";
                    case TypeKind::Dynamic: error(nullptr, "unsized dyn has no direct storage type."); return "{}";
                    case TypeKind::Vector: return "{ ptr, i64, i64 }";
                    case TypeKind::Unit: return "{}";
                    case TypeKind::Array: return "[" + std::to_string(type.extent) + " x " + llvmType(type.element) + "]";
                    case TypeKind::Slice: return "{ ptr, i64 }";
                    case TypeKind::Range:
                    {
                        const auto element = llvmType(type.element);
                        return "{ " + element + ", " + element + ", " + element + ", i1, i1, i1 }";
                    }
                    case TypeKind::Nominal: return "%aevix.t" + std::to_string(id);
                    default: error(nullptr, "unsupported runtime type " + typed_.types.display(id) + "."); return "i8";
                }
            }
            std::string value(AIRValueId id) const { return "%v" + std::to_string(id); }
            std::string temporary() { return "%t" + std::to_string(temporary_++); }
            std::string label() { return "guard" + std::to_string(label_++); }
            TypeId valueType(AIRValueId id) const
            {
                return id < function_->valueTypes.size() ? function_->valueTypes[id] : typed_.types.error();
            }
            const AIRGlobal* global(SymbolId id) const
            {
                for (const auto& item : module_.globals) if (item.symbol == id) return &item;
                return nullptr;
            }
            const AIRFunction* function(SymbolId id) const
            {
                for (const auto& item : module_.functions) if (item.symbol == id) return &item;
                return nullptr;
            }
            const AIRExternal* external(SymbolId id) const
            {
                for (const auto& item : module_.externals) if (item.symbol == id) return &item;
                return nullptr;
            }
            std::string escapedString(const std::string& value) const
            {
                std::ostringstream escaped;
                for (const unsigned char byte : value)
                    if (byte >= 32 && byte <= 126 && byte != '\\' && byte != '\"') escaped << static_cast<char>(byte);
                    else escaped << '\\' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
                return escaped.str();
            }
            void ensurePersist(const AIRGlobal& global)
            {
                if (!global.isPersist) return;
                output_ << "  call void @aevix_persist_ensure(ptr @aevix.persist.domain, ptr @persist.guard." << global.symbol
                        << ", ptr @persist.name." << global.symbol << ", ptr @persist.init." << global.symbol << ", ptr @persist.drop." << global.symbol << ")\n";
            }
            std::string foreignName(const std::string& name) const
            {
                std::ostringstream quoted;
                quoted << "@\"";
                for (const unsigned char byte : name)
                    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '_') quoted << static_cast<char>(byte);
                    else quoted << '\\' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
                quoted << '\"';
                return quoted.str();
            }
            const AIRRecord* record(TypeId id) const
            {
                for (const auto& item : module_.records) if (item.type == id) return &item;
                return nullptr;
            }
            struct EnumStorage final { std::size_t alignment = 1; std::size_t words = 0; };
            TypeLayoutInfo layout(TypeId type)
            {
                const auto found = typed_.layouts.find(type);
                if (found != typed_.layouts.end()) return found->second;
                const auto computed = TypeLayout(typed_.types, names_, typed_.symbolTypes).layout(type);
                if (!computed.succeeded) { error(nullptr, computed.message); return {}; }
                return computed.layout;
            }
            EnumStorage enumLayout(TypeId type)
            {
                EnumStorage storage;
                const auto description = layout(type);
                if (const auto* item = record(type))
                    for (const auto& variant : item->variants)
                        for (const auto fieldType : variant.fieldTypes) storage.alignment = std::max(storage.alignment, layout(fieldType).alignment);
                const auto offset = 4 + (storage.alignment - 4 % storage.alignment) % storage.alignment;
                const auto bytes = description.size > offset ? description.size - offset : 0;
                storage.words = bytes / storage.alignment + (bytes % storage.alignment != 0);
                return storage;
            }
            const EnumVariantInfo* variant(SymbolId symbol) const
            {
                for (const auto& item : module_.records)
                    for (const auto& alternative : item.variants) if (alternative.variant == symbol) return &alternative;
                return nullptr;
            }
            std::size_t enumOffset(TypeId type, SymbolId field)
            {
                const auto description = layout(type);
                for (const auto& item : description.fields) if (item.field == field) return item.offset;
                error(nullptr, "missing enum field layout.");
                return 0;
            }
            SymbolId reference(const Node& node) const
            {
                const auto found = names_.references.find(&node);
                return found != names_.references.end() && found->second.size() == 1 ? found->second[0] : InvalidSymbolId;
            }
            // 실패 경로를 별도 블록으로 분리하여 위험한 명령보다 먼저 검사한다.
            // Split failure into a separate block so checks precede dangerous instructions.
            void guard(const std::string& fails)
            {
                const auto bad = label(); const auto good = label();
                output_ << "  br i1 " << fails << ", label %" << bad << ", label %" << good << '\n'
                        << bad << ":\n  call void @llvm.trap()\n  unreachable\n" << good << ":\n";
            }
            std::string integerConstant(std::string spelling)
            {
                spelling.erase(std::remove(spelling.begin(), spelling.end(), '_'), spelling.end());
                if (spelling == "true") return "1";
                if (spelling == "false") return "0";
                bool negative = false;
                if (!spelling.empty() && (spelling[0] == '+' || spelling[0] == '-'))
                { negative = spelling[0] == '-'; spelling.erase(spelling.begin()); }
                unsigned base = 10;
                if (spelling.size() > 2 && spelling[0] == '0')
                {
                    if (spelling[1] == 'x' || spelling[1] == 'X') { base = 16; spelling.erase(0, 2); }
                    else if (spelling[1] == 'b' || spelling[1] == 'B') { base = 2; spelling.erase(0, 2); }
                    else if (spelling[1] == 'o' || spelling[1] == 'O') { base = 8; spelling.erase(0, 2); }
                }
                std::uint64_t number = 0;
                for (const auto character : spelling)
                {
                    const unsigned digit = std::isdigit(static_cast<unsigned char>(character)) ? character - '0' : std::tolower(static_cast<unsigned char>(character)) - 'a' + 10;
                    if (digit >= base) break;
                    number = number * base + digit;
                }
                return (negative && number != 0 ? "-" : "") + std::to_string(number);
            }
            std::string literal(const std::string& spelling, TypeId id)
            {
                const auto& type = typed_.types.get(id);
                if (type.kind == TypeKind::Unit) return "zeroinitializer";
                if (type.kind == TypeKind::Float)
                {
                    std::string text = spelling; text.erase(std::remove(text.begin(), text.end(), '_'), text.end());
                    double number = std::strtod(text.c_str(), nullptr);
                    if (type.bits == 32) number = static_cast<double>(static_cast<float>(number));
                    std::ostringstream stream;
                    stream << "0x" << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << std::bit_cast<std::uint64_t>(number);
                    return stream.str();
                }
                if (type.kind == TypeKind::Char && spelling.size() >= 3 && spelling.front() == '\'')
                {
                    const auto* bytes = reinterpret_cast<const unsigned char*>(spelling.data() + 1);
                    std::uint32_t scalar = bytes[0];
                    if (bytes[0] == '\\')
                    {
                        const char escaped = spelling[2];
                        if (escaped == 'n') scalar = 10; else if (escaped == 'r') scalar = 13; else if (escaped == 't') scalar = 9;
                        else if (escaped == '0') scalar = 0;
                        else if (escaped == 'u' && spelling.size() > 4)
                        {
                            const auto start = spelling.find('{'); const auto end = spelling.find('}');
                            scalar = start != std::string::npos && end != std::string::npos ? static_cast<std::uint32_t>(std::strtoul(spelling.substr(start + 1, end - start - 1).c_str(), nullptr, 16)) : 0;
                        }
                        else scalar = static_cast<unsigned char>(escaped);
                    }
                    else if (bytes[0] >= 0xc0)
                    {
                        const unsigned length = bytes[0] < 0xe0 ? 2 : bytes[0] < 0xf0 ? 3 : 4;
                        scalar = bytes[0] & (length == 2 ? 0x1f : length == 3 ? 0x0f : 0x07);
                        for (unsigned index = 1; index < length; ++index) scalar = (scalar << 6) | (bytes[index] & 0x3f);
                    }
                    return std::to_string(scalar);
                }
                return integerConstant(spelling);
            }
            std::string constantExpression(const Node& node, TypeId expected, std::unordered_set<const Node*>& visiting)
            {
                const auto evaluated = ConstEvaluator(names_, typed_.types, typed_.nodeTypes, typed_.symbolTypes).evaluate(node, expected);
                if (evaluated.status == ConstStatus::Constant)
                {
                    const auto& constant = evaluated.value;
                    if (constant.kind == ConstantKind::Integer || constant.kind == ConstantKind::Character)
                        return std::string(constant.negative ? "-" : "") + std::to_string(constant.magnitude);
                    if (constant.kind == ConstantKind::Boolean) return constant.boolean ? "1" : "0";
                    if (constant.kind == ConstantKind::Unit) return "zeroinitializer";
                    if (constant.kind == ConstantKind::Float)
                    {
                        std::ostringstream number;
                        number << std::setprecision(std::numeric_limits<long double>::max_digits10) << constant.floating;
                        return literal(number.str(), expected);
                    }
                }
                if (node.kind == "Literal") return literal(node.value, expected);
                if (node.kind == "Group") return constantExpression(*node.children[0], expected, visiting);
                if ((node.kind == "Name" || node.kind == "Member") && visiting.insert(&node).second)
                {
                    const auto* found = global(reference(node));
                    if (found && found->initializer)
                    {
                        auto result = constantExpression(*found->initializer, expected, visiting);
                        visiting.erase(&node); return result;
                    }
                }
                if (node.kind == "Prefix" && (node.value == "+" || node.value == "-") && node.children[0]->kind == "Literal")
                    return literal(node.value + node.children[0]->value, expected);
                if (node.kind == "ArrayLiteral")
                {
                    const auto& array = typed_.types.get(expected);
                    std::string result = "[";
                    for (std::size_t index = 0; index < node.children.size(); ++index)
                    { if (index) result += ", "; result += llvmType(array.element) + " " + constantExpression(*node.children[index], array.element, visiting); }
                    return result + "]";
                }
                error(&node, "global initialization currently requires a scalar or array constant.");
                return "zeroinitializer";
            }
            std::pair<std::string, TypeId> address(const AIRPlace& place, bool initializationWrite = false)
            {
                std::string pointer;
                TypeId current = typed_.types.error();
                if (place.local != InvalidAIRId)
                { pointer = "%l" + std::to_string(place.local); current = function_->locals[place.local].type; }
                else if (place.global != InvalidSymbolId)
                {
                    pointer = "@aevix.global." + std::to_string(place.global);
                    if (const auto* item = global(place.global))
                    {
                        current = item->type;
                        if (!initializationWrite) ensurePersist(*item);
                    }
                    else error(nullptr, "unknown global place.");
                }
                else if (place.pointer != InvalidAIRId)
                { pointer = value(place.pointer); current = typed_.types.get(valueType(place.pointer)).element; }
                else { error(nullptr, "place has no base."); pointer = "null"; }
                for (const auto& projection : place.projections)
                {
                    const auto next = temporary();
                    if (projection.kind == AIRProjectionKind::Field)
                        output_ << "  " << next << " = getelementptr " << llvmType(current) << ", ptr " << pointer << ", i32 0, i32 " << projection.field << '\n';
                    else if (projection.kind == AIRProjectionKind::EnumField)
                    {
                        const auto* info = variant(projection.variant);
                        if (!info || projection.field >= info->fields.size()) { error(nullptr, "invalid enum payload projection."); return {pointer, current}; }
                        output_ << "  " << next << " = getelementptr i8, ptr " << pointer << ", i64 " << enumOffset(current, info->fields[projection.field]) << '\n';
                    }
                    else if (projection.kind == AIRProjectionKind::Index)
                    {
                        const auto indexType = valueType(projection.index);
                        const auto numericType = llvmType(indexType);
                        const auto& integer = typed_.types.get(indexType);
                        const auto invalid = temporary();
                        // 매우 큰 길이에서도 음수가 허용되지 않도록 부호와 상한을 따로 검사한다.
                        // Check sign separately from the upper bound, including extremely large extents.
                        std::string index = value(projection.index);
                        std::string compareType = numericType;
                        if (integer.bits < 64)
                        {
                            const auto extended = temporary();
                            output_ << "  " << extended << " = " << (integer.isSigned ? "sext" : "zext") << ' ' << numericType << ' ' << index << " to i64\n";
                            index = extended; compareType = "i64";
                        }
                        const auto& collection = typed_.types.get(current);
                        std::string bound = std::to_string(projection.extent), data = pointer;
                        if (collection.kind == TypeKind::Vector || collection.kind == TypeKind::Slice)
                        {
                            const auto descriptor = temporary(); data = temporary(); bound = temporary();
                            output_ << "  " << descriptor << " = load " << llvmType(current) << ", ptr " << pointer << '\n'
                                    << "  " << data << " = extractvalue " << llvmType(current) << ' ' << descriptor << ", 0\n"
                                    << "  " << bound << " = extractvalue " << llvmType(current) << ' ' << descriptor << ", 1\n";
                        }
                        output_ << "  " << invalid << " = icmp uge " << compareType << ' ' << index << ", " << bound << '\n';
                        if (integer.isSigned)
                        {
                            const auto negative = temporary(); const auto outside = temporary();
                            output_ << "  " << negative << " = icmp slt " << numericType << ' ' << value(projection.index) << ", 0\n"
                                    << "  " << outside << " = or i1 " << invalid << ", " << negative << '\n';
                            guard(outside);
                        }
                        else guard(invalid);
                        if (collection.kind == TypeKind::Vector || collection.kind == TypeKind::Slice)
                            output_ << "  " << next << " = getelementptr " << llvmType(collection.element) << ", ptr " << data << ", " << compareType << ' ' << index << '\n';
                        else output_ << "  " << next << " = getelementptr " << llvmType(current) << ", ptr " << pointer << ", i32 0, " << compareType << ' ' << index << '\n';
                    }
                    else
                    { output_ << "  " << next << " = load ptr, ptr " << pointer << '\n'; }
                    pointer = next; current = projection.type;
                }
                return {pointer, current};
            }
            void checkedArithmetic(const AIRInstruction& item, const std::string& operation, const std::string& left, const std::string& right)
            {
                const auto& scalar = typed_.types.get(item.type);
                const auto type = llvmType(item.type);
                const auto intrinsic = "llvm." + std::string(scalar.isSigned ? "s" : "u") + operation + ".with.overflow." + type;
                overflowIntrinsics_.insert("declare { " + type + ", i1 } @" + intrinsic + "(" + type + ", " + type + ")");
                const auto pair = temporary(); const auto overflow = temporary();
                output_ << "  " << pair << " = call { " << type << ", i1 } @" << intrinsic << '(' << type << ' ' << left << ", " << type << ' ' << right << ")\n"
                        << "  " << value(item.result) << " = extractvalue { " << type << ", i1 } " << pair << ", 0\n"
                        << "  " << overflow << " = extractvalue { " << type << ", i1 } " << pair << ", 1\n";
                guard(overflow);
            }
            void binary(const AIRInstruction& item)
            {
                const auto leftId = item.operands[0], rightId = item.operands[1];
                const auto typeId = valueType(leftId);
                const auto& scalar = typed_.types.get(typeId);
                const auto type = llvmType(typeId);
                const auto left = value(leftId), right = value(rightId);
                const auto& op = item.operation;
                const bool floating = scalar.kind == TypeKind::Float;
                if ((op == "+" || op == "-" || op == "*") && !floating)
                { checkedArithmetic(item, op == "+" ? "add" : op == "-" ? "sub" : "mul", left, right); return; }
                if (op == "/" || op == "%")
                {
                    if (!floating)
                    {
                        const auto zero = temporary();
                        output_ << "  " << zero << " = icmp eq " << type << ' ' << right << ", 0\n";
                        guard(zero);
                        if (scalar.isSigned)
                        {
                            const auto minimum = scalar.bits == 64 ? std::string("-9223372036854775808") : "-" + std::to_string(std::uint64_t(1) << (scalar.bits - 1));
                            const auto isMin = temporary(), isNegative = temporary(), overflow = temporary();
                            output_ << "  " << isMin << " = icmp eq " << type << ' ' << left << ", " << minimum << '\n'
                                    << "  " << isNegative << " = icmp eq " << type << ' ' << right << ", -1\n"
                                    << "  " << overflow << " = and i1 " << isMin << ", " << isNegative << '\n';
                            guard(overflow);
                        }
                    }
                    output_ << "  " << value(item.result) << " = " << (floating ? "f" : scalar.isSigned ? "s" : "u") << (op == "/" ? "div" : "rem") << ' ' << type << ' ' << left << ", " << right << '\n';
                    return;
                }
                if (op == "<<" || op == ">>")
                {
                    const auto rightType = valueType(rightId);
                    const auto& countType = typed_.types.get(rightType);
                    const auto countSpelling = llvmType(rightType);
                    auto count = right;
                    auto compareType = countSpelling;
                    if (countType.bits < 64)
                    {
                        const auto extended = temporary();
                        output_ << "  " << extended << " = " << (countType.isSigned ? "sext" : "zext") << ' ' << countSpelling << ' ' << right << " to i64\n";
                        count = extended; compareType = "i64";
                    }
                    const auto invalid = temporary();
                    output_ << "  " << invalid << " = icmp uge " << compareType << ' ' << count << ", " << scalar.bits << '\n';
                    guard(invalid);
                    std::string resized = right;
                    if (countType.bits != scalar.bits)
                    {
                        resized = temporary();
                        output_ << "  " << resized << " = " << (countType.bits > scalar.bits ? "trunc" : "zext") << ' ' << countSpelling << ' ' << right << " to " << type << '\n';
                    }
                    output_ << "  " << value(item.result) << " = " << (op == "<<" ? "shl" : scalar.isSigned ? "ashr" : "lshr") << ' ' << type << ' ' << left << ", " << resized << '\n';
                    return;
                }
                if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=")
                {
                    if (scalar.kind == TypeKind::Unit)
                    { output_ << "  " << value(item.result) << " = freeze i1 " << (op == "==" ? "true" : "false") << '\n'; return; }
                    std::string predicate;
                    if (floating) predicate = op == "==" ? "oeq" : op == "!=" ? "une" : op == "<" ? "olt" : op == "<=" ? "ole" : op == ">" ? "ogt" : "oge";
                    else predicate = op == "==" ? "eq" : op == "!=" ? "ne" : std::string(scalar.isSigned ? "s" : "u") + (op == "<" ? "lt" : op == "<=" ? "le" : op == ">" ? "gt" : "ge");
                    output_ << "  " << value(item.result) << " = " << (floating ? "fcmp " : "icmp ") << predicate << ' ' << type << ' ' << left << ", " << right << '\n';
                    return;
                }
                std::string instruction = op == "+" ? "fadd" : op == "-" ? "fsub" : op == "*" ? "fmul" : op == "&" ? "and" : op == "|" ? "or" : op == "^" ? "xor" : "";
                if (instruction.empty()) { error(item.source, "unsupported binary operator '" + op + "'."); return; }
                output_ << "  " << value(item.result) << " = " << instruction << ' ' << type << ' ' << left << ", " << right << '\n';
            }
            void emitRange(const AIRInstruction& item)
            {
                const auto rangeId = item.opcode == AIROpcode::RangeConstruct ? item.type : valueType(item.operands[0]);
                const auto type = llvmType(rangeId);
                const auto& scalar = typed_.types.get(typed_.types.get(rangeId).element);
                const auto integer = "i" + std::to_string(scalar.bits);
                if (item.opcode == AIROpcode::RangeConstruct)
                {
                    const std::vector<std::pair<std::string, std::string>> fields = {
                        {integer, value(item.operands[0])}, {integer, value(item.operands[1])}, {integer, "1"},
                        {"i1", item.operation.ends_with("closed") ? "true" : "false"},
                        {"i1", item.operation.find("descending") != std::string::npos ? "true" : "false"}, {"i1", "true"}};
                    std::string aggregate = "zeroinitializer";
                    for (std::size_t i = 0; i < fields.size(); ++i)
                    {
                        const auto next = i + 1 == fields.size() ? value(item.result) : temporary();
                        output_ << "  " << next << " = insertvalue " << type << ' ' << aggregate << ", " << fields[i].first << ' ' << fields[i].second << ", " << i << '\n'; aggregate = next;
                    }
                    return;
                }
                const auto extract = [&](unsigned field)
                {
                    const auto result = temporary(); output_ << "  " << result << " = extractvalue " << type << ' ' << value(item.operands[0]) << ", " << field << '\n'; return result;
                };
                if (item.opcode == AIROpcode::RangeWithStep)
                {
                    output_ << "  " << value(item.result) << " = insertvalue " << type << ' ' << value(item.operands[0]) << ", " << integer << ' ' << value(item.operands[1]) << ", 2\n";
                    return;
                }
                const auto cursor = extract(0);
                if (item.opcode == AIROpcode::RangeCount || item.opcode == AIROpcode::RangeCountFits)
                {
                    const auto end = extract(1), step = extract(2), closed = extract(3), descending = extract(4), active = extract(5);
                    const auto wide = "i" + std::to_string(scalar.bits + 2);
                    const auto startWide = temporary(), endWide = temporary(), step64 = temporary(), positiveStep = temporary(), safeStep = temporary();
                    const auto ascendingDistance = temporary(), descendingDistance = temporary(), distance = temporary(), distance64 = temporary();
                    const auto quotient = temporary(), remainder = temporary(), roundUp = temporary(), extraHalfOpen = temporary(), extra = temporary(), count = temporary(), wrapped = temporary();
                    const auto less = temporary(), equal = temporary(), greater = temporary(), increasing = temporary(), decreasing = temporary(), inOrder = temporary(), nonempty = temporary(), result = temporary();
                    const std::string extension = scalar.isSigned ? "sext" : "zext", comparison = scalar.isSigned ? "s" : "u";
                    // 거리 자체는 u64에 들어가며 포함 길이의 마지막 +1만 넘칠 수 있다.
                    // Endpoint distance fits u64; only the final inclusive +1 can overflow cardinality.
                    output_ << "  " << startWide << " = " << extension << ' ' << integer << ' ' << cursor << " to " << wide << '\n'
                            << "  " << endWide << " = " << extension << ' ' << integer << ' ' << end << " to " << wide << '\n';
                    if (scalar.bits < 64) output_ << "  " << step64 << " = zext " << integer << ' ' << step << " to i64\n";
                    else output_ << "  " << step64 << " = freeze i64 " << step << '\n';
                    output_ << "  " << positiveStep << " = icmp ne i64 " << step64 << ", 0\n"
                            << "  " << safeStep << " = select i1 " << positiveStep << ", i64 " << step64 << ", i64 1\n"
                            << "  " << ascendingDistance << " = sub " << wide << ' ' << endWide << ", " << startWide << '\n'
                            << "  " << descendingDistance << " = sub " << wide << ' ' << startWide << ", " << endWide << '\n'
                            << "  " << distance << " = select i1 " << descending << ", " << wide << ' ' << descendingDistance << ", " << wide << ' ' << ascendingDistance << '\n';
                    output_ << "  " << distance64 << " = " << (scalar.bits + 2 < 64 ? "zext " : "trunc ") << wide << ' ' << distance << " to i64\n"
                            << "  " << quotient << " = udiv i64 " << distance64 << ", " << safeStep << '\n'
                            << "  " << remainder << " = urem i64 " << distance64 << ", " << safeStep << '\n'
                            << "  " << roundUp << " = icmp ne i64 " << remainder << ", 0\n"
                            << "  " << extraHalfOpen << " = zext i1 " << roundUp << " to i64\n"
                            << "  " << extra << " = select i1 " << closed << ", i64 1, i64 " << extraHalfOpen << '\n'
                            << "  " << count << " = add i64 " << quotient << ", " << extra << '\n'
                            << "  " << wrapped << " = icmp ult i64 " << count << ", " << quotient << '\n'
                            << "  " << less << " = icmp " << comparison << "lt " << integer << ' ' << cursor << ", " << end << '\n'
                            << "  " << equal << " = icmp eq " << integer << ' ' << cursor << ", " << end << '\n'
                            << "  " << greater << " = icmp " << comparison << "gt " << integer << ' ' << cursor << ", " << end << '\n';
                    const auto includeEqual = temporary();
                    output_ << "  " << includeEqual << " = and i1 " << closed << ", " << equal << '\n'
                            << "  " << increasing << " = or i1 " << less << ", " << includeEqual << '\n'
                            << "  " << decreasing << " = or i1 " << greater << ", " << includeEqual << '\n'
                            << "  " << inOrder << " = select i1 " << descending << ", i1 " << decreasing << ", i1 " << increasing << '\n'
                            << "  " << nonempty << " = and i1 " << active << ", " << inOrder << '\n'
                            << "  " << result << " = select i1 " << nonempty << ", i64 " << count << ", i64 0\n";
                    if (item.opcode == AIROpcode::RangeCountFits)
                    {
                        const auto overflow = temporary();
                        output_ << "  " << overflow << " = and i1 " << nonempty << ", " << wrapped << '\n'
                                << "  " << value(item.result) << " = xor i1 " << overflow << ", true\n";
                    }
                    else output_ << "  " << value(item.result) << " = freeze i64 " << result << '\n';
                    return;
                }
                if (item.opcode == AIROpcode::RangeValue)
                { output_ << "  " << value(item.result) << " = freeze " << integer << ' ' << cursor << '\n'; return; }
                const auto descending = extract(4), active = extract(5);
                if (item.opcode == AIROpcode::RangeTest)
                {
                    const auto end = extract(1), closed = extract(3);
                    const auto compare = [&](const std::string& predicate)
                    { const auto result = temporary(); output_ << "  " << result << " = icmp " << predicate << ' ' << integer << ' ' << cursor << ", " << end << '\n'; return result; };
                    const std::string prefix = scalar.isSigned ? "s" : "u";
                    const auto less = compare(prefix + "lt"), le = compare(prefix + "le"), greater = compare(prefix + "gt"), ge = compare(prefix + "ge");
                    const auto up = temporary(), down = temporary(), within = temporary();
                    output_ << "  " << up << " = select i1 " << closed << ", i1 " << le << ", i1 " << less << '\n'
                            << "  " << down << " = select i1 " << closed << ", i1 " << ge << ", i1 " << greater << '\n'
                            << "  " << within << " = select i1 " << descending << ", i1 " << down << ", i1 " << up << '\n'
                            << "  " << value(item.result) << " = and i1 " << active << ", " << within << '\n'; return;
                }
                // 확장 정수에서 다음 커서를 계산하고 원래 폭으로 재확장해 넘침을 검사한다.
                // Compute the next cursor in a wider integer and round-trip it to detect overflow.
                const auto step = extract(2), wide = "i" + std::to_string(scalar.bits + 2);
                const auto currentWide = temporary(), stepWide = temporary(), plus = temporary(), minus = temporary(), next = temporary(), narrowed = temporary(), roundTrip = temporary(), valid = temporary(), enabled = temporary(), updated = temporary();
                output_ << "  " << currentWide << " = " << (scalar.isSigned ? "sext" : "zext") << ' ' << integer << ' ' << cursor << " to " << wide << '\n'
                        << "  " << stepWide << " = zext " << integer << ' ' << step << " to " << wide << '\n'
                        << "  " << plus << " = add " << wide << ' ' << currentWide << ", " << stepWide << '\n'
                        << "  " << minus << " = sub " << wide << ' ' << currentWide << ", " << stepWide << '\n'
                        << "  " << next << " = select i1 " << descending << ", " << wide << ' ' << minus << ", " << wide << ' ' << plus << '\n'
                        << "  " << narrowed << " = trunc " << wide << ' ' << next << " to " << integer << '\n'
                        << "  " << roundTrip << " = " << (scalar.isSigned ? "sext" : "zext") << ' ' << integer << ' ' << narrowed << " to " << wide << '\n'
                        << "  " << valid << " = icmp eq " << wide << ' ' << roundTrip << ", " << next << '\n'
                        << "  " << enabled << " = and i1 " << active << ", " << valid << '\n'
                        << "  " << updated << " = insertvalue " << type << ' ' << value(item.operands[0]) << ", " << integer << ' ' << narrowed << ", 0\n"
                        << "  " << value(item.result) << " = insertvalue " << type << ' ' << updated << ", i1 " << enabled << ", 5\n";
            }
            void enumStore(const std::string& storage, const EnumVariantInfo& variant, const std::vector<std::pair<TypeId, std::string>>& fields)
            {
                output_ << "  store " << llvmType(variant.type) << " zeroinitializer, ptr " << storage << "\n  store i32 " << variant.tag << ", ptr " << storage << '\n';
                for (std::size_t index = 0; index < fields.size(); ++index)
                {
                    const auto pointer = temporary(); output_ << "  " << pointer << " = getelementptr i8, ptr " << storage << ", i64 " << enumOffset(variant.type, variant.fields[index]) << '\n'
                        << "  store " << llvmType(fields[index].first) << ' ' << fields[index].second << ", ptr " << pointer << '\n';
                }
            }
            void boxNew(const AIRInstruction& item)
            {
                usesAllocation_ = true;
                const auto& carrier = typed_.carriers.at(item.type); const auto box = carrier.value;
                const auto payload = typed_.types.get(box).element;
                const auto layout = typed_.layouts.find(payload);
                if (layout == typed_.layouts.end()) { error(item.source, "Box payload layout missing."); return; }
                const auto allocated = temporary(); const auto failed = temporary(); const auto no = label(); const auto yes = label(); const auto done = label();
                output_ << "  " << allocated << " = call ptr @malloc(i64 " << std::max<std::size_t>(1, layout->second.size) << ")\n"
                        << "  " << failed << " = icmp eq ptr " << allocated << ", null\n  br i1 " << failed << ", label %" << no << ", label %" << yes << '\n';
                const auto storage = "%enumtmp" + std::to_string(item.result);
                output_ << yes << ":\n  store " << llvmType(payload) << ' ' << value(item.operands[0]) << ", ptr " << allocated << '\n';
                enumStore(storage, *variant(carrier.success), {{box, allocated}}); output_ << "  br label %" << done << '\n';
                output_ << no << ":\n";
                const EnumVariantInfo* failure = nullptr;
                for (const auto& record : module_.records) if (record.type == carrier.error)
                    for (const auto& candidate : record.variants) if (candidate.tag == 0) failure = &candidate;
                if (!failure) { error(item.source, "Box InputError allocation variant missing."); return; }
                const auto errorStorage = "%boxerrtmp" + std::to_string(item.result);
                enumStore(errorStorage, *failure, {{payload, value(item.operands[0])}});
                const auto errorValue = temporary(); output_ << "  " << errorValue << " = load " << llvmType(carrier.error) << ", ptr " << errorStorage << '\n';
                enumStore(storage, *variant(carrier.failure), {{carrier.error, errorValue}});
                output_ << "  br label %" << done << '\n' << done << ":\n  " << value(item.result) << " = load " << llvmType(item.type) << ", ptr " << storage << '\n';
            }
            void emitInstruction(const AIRInstruction& item)
            {
                const auto type = item.type == InvalidTypeId ? std::string{} : llvmType(item.type);
                switch (item.opcode)
                {
                    case AIROpcode::RangeConstruct: case AIROpcode::RangeTest: case AIROpcode::RangeNext: case AIROpcode::RangeValue:
                    case AIROpcode::RangeWithStep: case AIROpcode::RangeCount: case AIROpcode::RangeCountFits: emitRange(item); return;
                    case AIROpcode::SliceConstruct:
                    {
                        const auto [pointer, storageType] = address(item.place);
                        const auto first = temporary();
                        output_ << "  " << first << " = insertvalue " << type << " zeroinitializer, ptr " << pointer << ", 0\n"
                                << "  " << value(item.result) << " = insertvalue " << type << ' ' << first << ", i64 " << typed_.types.get(storageType).extent << ", 1\n"; return;
                    }
                    case AIROpcode::SliceLength:
                    {
                        const auto input = item.operands[0]; auto descriptor = value(input);
                        auto view = valueType(input);
                        if (typed_.types.get(view).kind == TypeKind::Reference)
                        { view = typed_.types.get(view).element; const auto loaded = temporary(); output_ << "  " << loaded << " = load " << llvmType(view) << ", ptr " << descriptor << '\n'; descriptor = loaded; }
                        output_ << "  " << value(item.result) << " = extractvalue " << llvmType(view) << ' ' << descriptor << ", 1\n"; return;
                    }
                    case AIROpcode::ScopeBegin: case AIROpcode::ScopeEnd: return;
                    case AIROpcode::EnsurePersist:
                        if (const auto* storage = global(item.place.global)) ensurePersist(*storage);
                        else error(item.source, "persist declaration has no storage.");
                        return;
                    case AIROpcode::VectorAllocate:
                    {
                        usesAllocation_ = true;
                        const auto count = std::stoull(item.operation);
                        const auto element = typed_.types.get(item.type).element;
                        const auto layout = TypeLayout(typed_.types, names_, typed_.symbolTypes).layout(element);
                        if (!layout.succeeded || (count && layout.layout.size > std::numeric_limits<std::int64_t>::max() / count))
                        { error(item.source, "Collection allocation size exceeds the target index limit."); return; }
                        std::string pointer = "null";
                        if (count)
                        {
                            pointer = temporary();
                            output_ << "  " << pointer << " = call ptr @malloc(i64 " << std::max<std::uint64_t>(1, count * layout.layout.size) << ")\n";
                        }
                        const auto first = temporary();
                        output_ << "  " << first << " = insertvalue " << type << " zeroinitializer, ptr " << pointer << ", 0\n"
                                << "  " << value(item.result) << " = insertvalue " << type << ' ' << first << ", i64 " << count << ", 2\n";
                        return;
                    }
                    case AIROpcode::VectorAvailable:
                    {
                        const auto descriptorType = llvmType(valueType(item.operands[0]));
                        const auto pointer = temporary(), capacity = temporary(), empty = temporary(), allocated = temporary();
                        output_ << "  " << pointer << " = extractvalue " << descriptorType << ' ' << value(item.operands[0]) << ", 0\n"
                                << "  " << capacity << " = extractvalue " << descriptorType << ' ' << value(item.operands[0]) << ", 2\n"
                                << "  " << empty << " = icmp eq i64 " << capacity << ", 0\n"
                                << "  " << allocated << " = icmp ne ptr " << pointer << ", null\n"
                                << "  " << value(item.result) << " = or i1 " << empty << ", " << allocated << '\n';
                        return;
                    }
                    case AIROpcode::VectorAppend:
                    {
                        const auto [addressValue, descriptorType] = address(item.place);
                        const auto spelling = llvmType(descriptorType);
                        const auto descriptor = temporary(), pointer = temporary(), length = temporary(), destination = temporary(), next = temporary(), updated = temporary();
                        const auto element = typed_.types.get(descriptorType).element;
                        output_ << "  " << descriptor << " = load " << spelling << ", ptr " << addressValue << '\n'
                                << "  " << pointer << " = extractvalue " << spelling << ' ' << descriptor << ", 0\n"
                                << "  " << length << " = extractvalue " << spelling << ' ' << descriptor << ", 1\n"
                                << "  " << destination << " = getelementptr " << llvmType(element) << ", ptr " << pointer << ", i64 " << length << '\n'
                                << "  store " << llvmType(element) << ' ' << value(item.operands[0]) << ", ptr " << destination << '\n'
                                << "  " << next << " = add i64 " << length << ", 1\n"
                                << "  " << updated << " = insertvalue " << spelling << ' ' << descriptor << ", i64 " << next << ", 1\n"
                                << "  store " << spelling << ' ' << updated << ", ptr " << addressValue << '\n';
                        return;
                    }
                    case AIROpcode::VectorReserve:
                    {
                        const auto [pointer, vectorType] = address(item.place);
                        reserveTypes_.insert(vectorType); usesAllocation_ = true;
                        output_ << "  " << value(item.result) << " = call i32 @aevix.vector.reserve." << vectorType << "(ptr " << pointer << ", i64 " << value(item.operands[0]) << ")\n";
                        return;
                    }
                    case AIROpcode::VectorLength:
                    {
                        auto operandType = valueType(item.operands[0]); auto descriptor = value(item.operands[0]);
                        if (typed_.types.get(operandType).kind == TypeKind::Reference)
                        {
                            operandType = typed_.types.get(operandType).element; const auto loaded = temporary();
                            output_ << "  " << loaded << " = load " << llvmType(operandType) << ", ptr " << descriptor << '\n'; descriptor = loaded;
                        }
                        const auto field = item.operation == "capacity" ? 2 : 1;
                        if (field == 2 && typed_.types.get(operandType).kind != TypeKind::Vector) { error(item.source, "capacity requires a Vector."); return; }
                        output_ << "  " << value(item.result) << " = extractvalue " << llvmType(operandType) << ' ' << descriptor << ", " << field << '\n';
                        return;
                    }
                    case AIROpcode::Constant:
                        output_ << "  " << value(item.result) << " = freeze " << type << ' ' << literal(item.operation, item.type) << '\n'; return;
                    case AIROpcode::Read: case AIROpcode::Move:
                    {
                        const auto [pointer, ignored] = address(item.place); (void)ignored;
                        output_ << "  " << value(item.result) << " = load " << type << ", ptr " << pointer << '\n';
                        // 이동한 payload는 정리 가능한 빈 저장소로 바꾸고 나머지 필드는 보존한다.
                        // Replace moved payload storage with a drop-safe empty value and preserve other fields.
                        if (item.opcode == AIROpcode::Move && item.operation == "enum.extract")
                            output_ << "  store " << type << " zeroinitializer, ptr " << pointer << '\n';
                        if (item.opcode == AIROpcode::Move && item.place.local != InvalidAIRId && item.place.projections.empty())
                            output_ << "  store i1 false, ptr %initialized" << item.place.local << '\n';
                        return;
                    }
                    case AIROpcode::Write:
                    {
                        const bool initialization = function_->initializesGlobal == item.place.global && item.place.projections.empty() && item.operation == "persist.initialize";
                        const auto [pointer, ignored] = address(item.place, initialization); (void)ignored;
                        output_ << "  store " << type << ' ' << value(item.operands[0]) << ", ptr " << pointer << '\n';
                        if (item.place.local != InvalidAIRId && item.place.projections.empty()) output_ << "  store i1 true, ptr %initialized" << item.place.local << '\n';
                        return;
                    }
                    case AIROpcode::BorrowShared: case AIROpcode::BorrowMut:
                    {
                        if (item.operation == "dyn.reborrow")
                        { output_ << "  " << value(item.result) << " = freeze { ptr, ptr } " << value(item.place.pointer) << '\n'; return; }
                        const auto [pointer, ignored] = address(item.place); (void)ignored;
                        if (item.operation == "box.borrow")
                        { output_ << "  " << value(item.result) << " = load " << type << ", ptr " << pointer << '\n'; return; }
                        if (item.operation.starts_with("dyn.view."))
                        {
                            const auto partial = temporary();
                            output_ << "  " << partial << " = insertvalue { ptr, ptr } zeroinitializer, ptr " << pointer << ", 0\n"
                                    << "  " << value(item.result) << " = insertvalue { ptr, ptr } " << partial << ", ptr @aevix.dyn.vtable."
                                    << item.operation.substr(9) << ", 1\n";
                            return;
                        }
                        output_ << "  " << value(item.result) << " = getelementptr i8, ptr " << pointer << ", i64 0\n"; return;
                    }
                    case AIROpcode::Unary:
                    {
                        if (item.operation == "box.new") { boxNew(item); return; }
                        if (item.operation.starts_with("box.into_dyn."))
                        {
                            const auto partial = temporary();
                            output_ << "  " << partial << " = insertvalue { ptr, ptr } zeroinitializer, ptr " << value(item.operands[0]) << ", 0\n"
                                << "  " << value(item.result) << " = insertvalue { ptr, ptr } " << partial << ", ptr @aevix.dyn.vtable." << item.operation.substr(13) << ", 1\n"; return;
                        }
                        const auto operand = value(item.operands[0]);
                        if (item.operation == "+") output_ << "  " << value(item.result) << " = freeze " << type << ' ' << operand << '\n';
                        else if (item.operation == "~" || item.operation == "!") output_ << "  " << value(item.result) << " = xor " << type << ' ' << operand << ", -1\n";
                        else if (item.operation == "-" && typed_.types.get(item.type).kind == TypeKind::Float) output_ << "  " << value(item.result) << " = fneg " << type << ' ' << operand << '\n';
                        else if (item.operation == "-") checkedArithmetic(item, "sub", "0", operand);
                        else error(item.source, "unsupported unary operation.");
                        return;
                    }
                    case AIROpcode::Binary: binary(item); return;
                    case AIROpcode::Call:
                    {
                        if (item.operation.starts_with("dyn.call."))
                        {
                            const auto receiver = value(item.operands[0]);
                            const auto data = temporary(); const auto table = temporary(); const auto slot = temporary(); const auto callable = temporary();
                            output_ << "  " << data << " = extractvalue { ptr, ptr } " << receiver << ", 0\n"
                                    << "  " << table << " = extractvalue { ptr, ptr } " << receiver << ", 1\n"
                                    << "  " << slot << " = getelementptr ptr, ptr " << table << ", i64 " << std::stoull(item.operation.substr(9)) + 1 << '\n'
                                    << "  " << callable << " = load ptr, ptr " << slot << "\n  ";
                            if (item.result != InvalidAIRId) output_ << value(item.result) << " = ";
                            output_ << "call " << type << ' ' << callable << "(ptr " << data;
                            for (std::size_t index = 1; index < item.operands.size(); ++index)
                                output_ << ", " << llvmType(valueType(item.operands[index])) << ' ' << value(item.operands[index]);
                            output_ << ")\n"; return;
                        }
                        const auto* target = function(item.callee);
                        const auto* foreign = external(item.callee);
                        if (!target && !foreign) { error(item.source, "call target has no generated body or external declaration."); return; }
                        output_ << "  ";
                        if (item.result != InvalidAIRId) output_ << value(item.result) << " = ";
                        output_ << "call " << llvmType(target ? target->result : foreign->result) << ' '
                                << (target ? "@aevix.fn." + std::to_string(item.callee) : foreignName(foreign->name)) << '(';
                        for (std::size_t index = 0; index < item.operands.size(); ++index)
                        { if (index) output_ << ", "; output_ << llvmType(valueType(item.operands[index])) << ' ' << value(item.operands[index]); }
                        output_ << ")\n";
                        return;
                    }
                    case AIROpcode::EnumTag:
                    {
                        const auto [pointer, ignored] = address(item.place); (void)ignored;
                        output_ << "  " << value(item.result) << " = load i32, ptr " << pointer << '\n';
                        return;
                    }
                    case AIROpcode::EnumConstruct:
                    {
                        const auto* info = variant(item.callee);
                        if (!info || info->fields.size() != item.operands.size()) { error(item.source, "invalid enum construction metadata."); return; }
                        const auto storage = "%enumtmp" + std::to_string(item.result);
                        // 비활성 변형은 객체가 아닌 공용 저장 바이트로만 존재한다.
                        // Inactive variants exist only as shared storage bytes, never as live objects.
                        output_ << "  store " << type << " zeroinitializer, ptr " << storage << '\n'
                                << "  store i32 " << info->tag << ", ptr " << storage << '\n';
                        for (std::size_t index = 0; index < item.operands.size(); ++index)
                        {
                            const auto pointer = temporary();
                            output_ << "  " << pointer << " = getelementptr i8, ptr " << storage << ", i64 " << enumOffset(info->type, info->fields[index]) << '\n'
                                    << "  store " << llvmType(info->fieldTypes[index]) << ' ' << value(item.operands[index]) << ", ptr " << pointer << '\n';
                        }
                        output_ << "  " << value(item.result) << " = load " << type << ", ptr " << storage << '\n';
                        return;
                    }
                    case AIROpcode::Aggregate:
                    {
                        std::string aggregate = "zeroinitializer";
                        for (std::size_t index = 0; index < item.operands.size(); ++index)
                        {
                            const auto next = index + 1 == item.operands.size() ? value(item.result) : temporary();
                            output_ << "  " << next << " = insertvalue " << type << ' ' << aggregate << ", " << llvmType(valueType(item.operands[index])) << ' ' << value(item.operands[index]) << ", " << index << '\n';
                            aggregate = next;
                        }
                        if (item.operands.empty()) output_ << "  " << value(item.result) << " = freeze " << type << " zeroinitializer\n";
                        return;
                    }
                    case AIROpcode::Drop:
                    {
                        if (item.place.local == InvalidAIRId) return;
                        const auto slot = item.place.local;
                        const auto live = temporary(); const auto drop = label(); const auto done = label();
                        output_ << "  " << live << " = load i1, ptr %initialized" << slot << '\n'
                                << "  br i1 " << live << ", label %" << drop << ", label %" << done << '\n'
                                << drop << ":\n";
                        emitDropCall(function_->locals[slot].type, "%l" + std::to_string(slot));
                        output_ << "  store " << llvmType(function_->locals[slot].type) << " zeroinitializer, ptr %l" << slot << '\n'
                                << "  store i1 false, ptr %initialized" << slot << "\n  br label %" << done << '\n' << done << ":\n";
                        return;
                    }
                }
            }
            bool needsDrop(TypeId id, std::set<TypeId>& active)
            {
                const auto& type = typed_.types.get(id);
                if (type.kind == TypeKind::Vector || type.kind == TypeKind::Box) return true;
                if (!active.insert(id).second) return false;
                bool result = type.kind == TypeKind::Array && needsDrop(type.element, active);
                if (const auto* info = record(id))
                {
                    for (const auto field : info->fieldTypes) result = result || needsDrop(field, active);
                    for (const auto& variant : info->variants) for (const auto field : variant.fieldTypes) result = result || needsDrop(field, active);
                }
                active.erase(id); return result;
            }
            void emitDropCall(TypeId id, const std::string& pointer)
            {
                std::set<TypeId> active;
                if (!needsDrop(id, active)) return;
                dropTypes_.insert(id);
                output_ << "  call void @aevix.collection.drop." << id << "(ptr " << pointer << ")\n";
            }
            void emitReserves()
            {
                for (const auto id : reserveTypes_)
                {
                    const auto& type = typed_.types.get(id);
                    const auto layout = TypeLayout(typed_.types, names_, typed_.symbolTypes).layout(type.element);
                    if (!layout.succeeded) { error(nullptr, "Cannot determine Vector element storage size."); continue; }
                    const auto bytes = layout.layout.size;
                    const auto maximum = std::uint64_t(std::numeric_limits<std::int64_t>::max()) / std::max<std::size_t>(1, bytes);
                    const auto spelling = llvmType(id);
                    output_ << "define internal i32 @aevix.vector.reserve." << id << "(ptr %owner, i64 %additional) {\nentry:\n"
                            << "  %descriptor = load " << spelling << ", ptr %owner\n"
                            << "  %old = extractvalue " << spelling << " %descriptor, 0\n"
                            << "  %length = extractvalue " << spelling << " %descriptor, 1\n"
                            << "  %capacity = extractvalue " << spelling << " %descriptor, 2\n"
                            << "  %added = add i64 %length, %additional\n"
                            << "  %wrapped = icmp ult i64 %added, %length\n"
                            << "  %large = icmp ugt i64 %added, " << maximum << "\n"
                            << "  %invalid = or i1 %wrapped, %large\n"
                            << "  br i1 %invalid, label %overflow, label %check\ncheck:\n"
                            << "  %fits = icmp ule i64 %added, %capacity\n"
                            << "  br i1 %fits, label %success, label %grow\ngrow:\n"
                            << "  %doubling_overflows = icmp ugt i64 %capacity, " << maximum / 2 << "\n"
                            << "  %doubled = mul i64 %capacity, 2\n"
                            << "  %safe_double = select i1 %doubling_overflows, i64 " << maximum << ", i64 %doubled\n"
                            << "  %too_small = icmp ult i64 %safe_double, %added\n"
                            << "  %wanted = select i1 %too_small, i64 %added, i64 %safe_double\n"
                            << "  %bytes = mul i64 %wanted, " << bytes << "\n"
                            << "  %zero_sized = icmp eq i64 %bytes, 0\n"
                            << "  %allocation_bytes = select i1 %zero_sized, i64 1, i64 %bytes\n"
                            << "  %new = call ptr @malloc(i64 %allocation_bytes)\n"
                            << "  %failed = icmp eq ptr %new, null\n"
                            << "  br i1 %failed, label %allocation, label %relocate\nrelocate:\n"
                            << "  %copy_bytes = mul i64 %length, " << bytes << "\n"
                            << "  call void @llvm.memcpy.p0.p0.i64(ptr %new, ptr %old, i64 %copy_bytes, i1 false)\n"
                            << "  call void @free(ptr %old)\n"
                            << "  %new_pointer = insertvalue " << spelling << " %descriptor, ptr %new, 0\n"
                            << "  %updated = insertvalue " << spelling << " %new_pointer, i64 %wanted, 2\n"
                            << "  store " << spelling << " %updated, ptr %owner\n"
                            << "  br label %success\nsuccess:\n  ret i32 0\nallocation:\n  ret i32 1\noverflow:\n  ret i32 2\n}\n";
                }
            }
            void emitPayloadDrops()
            {
                for (const auto type : payloadDropTypes_)
                {
                    temporary_ = 0; label_ = 0; usesAllocation_ = true;
                    output_ << "define internal void @aevix.box.payload.drop." << type << "(ptr %storage) {\nentry:\n";
                    emitDropCall(type, "%storage"); output_ << "  call void @free(ptr %storage)\n  ret void\n}\n";
                }
            }
            void emitDrops()
            {
                std::set<TypeId> emitted;
                while (emitted.size() < dropTypes_.size())
                {
                    TypeId id = InvalidTypeId;
                    for (const auto candidate : dropTypes_) if (!emitted.contains(candidate)) { id = candidate; break; }
                    emitted.insert(id); const auto& type = typed_.types.get(id);
                    output_ << "define internal void @aevix.collection.drop." << id << "(ptr %object) {\nentry:\n";
                    temporary_ = 0; label_ = 0;
                    if (type.kind == TypeKind::Box)
                    {
                        usesAllocation_ = true;
                        if (typed_.types.get(type.element).kind == TypeKind::Dynamic)
                            output_ << "  %owner = load { ptr, ptr }, ptr %object\n  %data = extractvalue { ptr, ptr } %owner, 0\n  %empty = icmp eq ptr %data, null\n  br i1 %empty, label %done, label %destroy\ndestroy:\n  %table = extractvalue { ptr, ptr } %owner, 1\n  %drop = load ptr, ptr %table\n  call void %drop(ptr %data)\n  br label %done\ndone:\n";
                        else
                        {
                            output_ << "  %payload = load ptr, ptr %object\n  %empty = icmp eq ptr %payload, null\n  br i1 %empty, label %done, label %destroy\ndestroy:\n";
                            emitDropCall(type.element, "%payload"); output_ << "  call void @free(ptr %payload)\n  br label %done\ndone:\n";
                        }
                    }
                    else if (type.kind == TypeKind::Vector || type.kind == TypeKind::Array)
                    {
                        usesAllocation_ = usesAllocation_ || type.kind == TypeKind::Vector;
                        std::string pointer = "%object", count = std::to_string(type.extent);
                        if (type.kind == TypeKind::Vector)
                        {
                            pointer = temporary(); count = temporary(); const auto descriptor = temporary();
                            output_ << "  " << descriptor << " = load " << llvmType(id) << ", ptr %object\n"
                                    << "  " << pointer << " = extractvalue " << llvmType(id) << ' ' << descriptor << ", 0\n"
                                    << "  " << count << " = extractvalue " << llvmType(id) << ' ' << descriptor << ", 1\n";
                        }
                        std::set<TypeId> active;
                        if (needsDrop(type.element, active))
                        {
                            output_ << "  br label %loop\nloop:\n  %index = phi i64 [ " << count << ", %entry ], [ %previous, %body ]\n"
                                    << "  %empty = icmp eq i64 %index, 0\n  br i1 %empty, label %done, label %body\nbody:\n  %previous = sub i64 %index, 1\n";
                            const auto element = temporary();
                            output_ << "  " << element << " = getelementptr " << llvmType(type.element) << ", ptr " << pointer << ", i64 %previous\n";
                            emitDropCall(type.element, element);
                            output_ << "  br label %loop\ndone:\n";
                        }
                        if (type.kind == TypeKind::Vector) output_ << "  call void @free(ptr " << pointer << ")\n";
                    }
                    else if (const auto* info = record(id))
                    {
                        if (info->isEnum)
                        {
                            output_ << "  %tag = load i32, ptr %object\n  switch i32 %tag, label %done [\n";
                            for (const auto& variant : info->variants) output_ << "    i32 " << variant.tag << ", label %variant" << variant.tag << '\n';
                            output_ << "  ]\n";
                            for (const auto& variant : info->variants)
                            {
                                output_ << "variant" << variant.tag << ":\n";
                                for (std::size_t i = variant.fields.size(); i > 0; --i)
                                {
                                    const auto pointer = temporary();
                                    output_ << "  " << pointer << " = getelementptr i8, ptr %object, i64 " << enumOffset(id, variant.fields[i - 1]) << '\n';
                                    emitDropCall(variant.fieldTypes[i - 1], pointer);
                                }
                                output_ << "  br label %done\n";
                            }
                            output_ << "done:\n";
                        }
                        else for (std::size_t i = info->fieldTypes.size(); i > 0; --i)
                        {
                            const auto pointer = temporary();
                            output_ << "  " << pointer << " = getelementptr " << llvmType(id) << ", ptr %object, i32 0, i32 " << i - 1 << '\n';
                            emitDropCall(info->fieldTypes[i - 1], pointer);
                        }
                    }
                    output_ << "  ret void\n}\n";
                }
            }
            void emitFunction(const AIRFunction& function)
            {
                function_ = &function;
                temporary_ = 0; label_ = 0;
                output_ << "define " << llvmType(function.result) << " @aevix.fn." << function.symbol << '(';
                for (std::size_t index = 0; index < function.parameters.size(); ++index)
                { if (index) output_ << ", "; output_ << llvmType(function.locals[function.parameters[index]].type) << " %arg" << index; }
                output_ << ") {\n";
                for (std::size_t block = 0; block < function.blocks.size(); ++block)
                {
                    output_ << "b" << block << ":\n";
                    if (block == 0)
                    {
                        for (std::size_t index = 0; index < function.locals.size(); ++index)
                            output_ << "  %l" << index << " = alloca " << llvmType(function.locals[index].type) << "\n"
                                    << "  %initialized" << index << " = alloca i1\n  store i1 false, ptr %initialized" << index << '\n';
                        for (const auto& basicBlock : function.blocks)
                            for (const auto& instruction : basicBlock.instructions)
                                if (instruction.opcode == AIROpcode::EnumConstruct || instruction.operation == "box.new")
                                {
                                    output_ << "  %enumtmp" << instruction.result << " = alloca " << llvmType(instruction.type) << '\n';
                                    if (instruction.operation == "box.new") output_ << "  %boxerrtmp" << instruction.result << " = alloca " << llvmType(typed_.carriers.at(instruction.type).error) << '\n';
                                }
                        for (std::size_t index = 0; index < function.parameters.size(); ++index)
                        {
                            const auto slot = function.parameters[index];
                            output_ << "  store " << llvmType(function.locals[slot].type) << " %arg" << index << ", ptr %l" << slot << '\n'
                                    << "  store i1 true, ptr %initialized" << slot << '\n';
                        }
                    }
                    for (const auto& item : function.blocks[block].instructions) emitInstruction(item);
                    const auto& end = function.blocks[block].terminator;
                    if (end.kind == AIRTerminatorKind::Jump) output_ << "  br label %b" << end.target << '\n';
                    else if (end.kind == AIRTerminatorKind::Branch) output_ << "  br i1 " << value(end.value) << ", label %b" << end.target << ", label %b" << end.alternative << '\n';
                    else if (end.kind == AIRTerminatorKind::Return)
                    {
                        output_ << "  ret " << llvmType(function.result);
                        if (end.value != InvalidAIRId) output_ << ' ' << value(end.value);
                        output_ << '\n';
                    }
                    else output_ << "  unreachable\n";
                }
                output_ << "}\n";
            }
            void emitModuleLifecycle()
            {
                if (hasPersist_)
                    for (const auto& storage : module_.globals)
                    {
                        if (!storage.isPersist) continue;
                        const auto* initializer = function(storage.initializerFunction);
                        if (!initializer || typed_.types.get(initializer->result).kind != TypeKind::Void || !initializer->parameters.empty())
                        { error(storage.source, "persist storage has no valid initializer function."); continue; }
                        output_ << "define internal void @persist.init." << storage.symbol << "() {\nentry:\n  call void @aevix.fn." << storage.initializerFunction << "()\n  ret void\n}\n";
                        output_ << "define internal void @persist.drop." << storage.symbol << "() {\nentry:\n";
                        emitDropCall(storage.type, "@aevix.global." + std::to_string(storage.symbol));
                        output_ << "  store " << llvmType(storage.type) << " zeroinitializer, ptr @aevix.global." << storage.symbol << "\n  ret void\n}\n";
                    }
                output_ << "define void @aevix.module.initialize() {\nentry:\n";
                if (hasPersist_)
                    for (const auto symbol : module_.initializationOrder)
                        if (const auto* storage = global(symbol)) ensurePersist(*storage);
                output_ << "  ret void\n}\n";
                output_ << "define void @aevix.module.shutdown() {\nentry:\n";
                if (hasPersist_) output_ << "  call void @aevix_persist_shutdown(ptr @aevix.persist.domain)\n";
                output_ << "  ret void\n}\n";
                output_ << "@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 65535, ptr @aevix.module.initialize, ptr null }]\n"
                        << "@llvm.global_dtors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 65535, ptr @aevix.module.shutdown, ptr null }]\n";
            }
            void emitEntry()
            {
                if (module_.entry == InvalidSymbolId) return;
                const auto* entry = function(module_.entry);
                if (!entry) return;
                if (!entry->parameters.empty()) { error(entry->source, "main must have no parameters in this execution stage."); return; }
                if (typed_.types.get(entry->result).kind == TypeKind::Void)
                    output_ << "define i32 @main() {\nentry:\n  call void @aevix.module.initialize()\n  call void @aevix.fn." << entry->symbol << "()\n  call void @aevix.module.shutdown()\n  ret i32 0\n}\n";
                else if (entry->result == typed_.types.builtin("i32"))
                    output_ << "define i32 @main() {\nentry:\n  call void @aevix.module.initialize()\n  %result = call i32 @aevix.fn." << entry->symbol << "()\n  call void @aevix.module.shutdown()\n  ret i32 %result\n}\n";
                else error(entry->source, "main must return i32 or void.");
            }
        };
    }
    std::optional<std::string> emitLLVM(const AIRModule& module, const TypeCheckResult& types,
                                      const NameResolution& names, Diagnostics& diagnostics, std::string targetTriple)
    {
        if (!diagnostics.empty() || !verifyAIR(module, types.types, diagnostics)) return std::nullopt;
        return Emitter(module, types, names, diagnostics, std::move(targetTriple)).run();
    }
}
