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
#include "aevix/Backend/LLVM/SimdCodeGen.hpp"
#include "aevix/Frontend/Sema/NumericLiteral.hpp"
#include "aevix/Frontend/Sema/SmallFloat.hpp"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
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
                for (const auto& function : module_.functions) for (const auto& block : function.blocks)
                    for (const auto& item : block.instructions)
                        if (item.opcode == AIROpcode::Constant && typed_.types.get(item.type).kind == TypeKind::StringView) internText(item.operation);
                for (const auto& global : module_.globals) if (global.initializer) collectText(*global.initializer);
                if (!triple_.empty())
                {
                    if (triple_.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") != std::string::npos)
                    { error(nullptr, "invalid target triple."); return std::nullopt; }
                    output_ << "target triple = \"" << triple_ << "\"\n";
                }
                for (const auto& [spelling, data] : textLiterals_)
                { (void)spelling; output_ << data.first << " = private unnamed_addr constant [" << data.second.size() << " x i8] c\"" << escapedString(data.second) << "\"\n"; }
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
                for (const auto& function : module_.functions)
                    if (benchFunction(function))
                    {
                        usesBench_ = true;
                        const auto name = function.name + "#" + std::to_string(function.symbol);
                        output_ << "@aevix.bench.name." << function.symbol << " = private unnamed_addr constant [" << name.size() + 1
                                << " x i8] c\"" << escapedString(name) << "\\00\"\n";
                    }
                for (const auto& function : module_.functions) emitFunction(function);
                emitModuleLifecycle();
                emitEntry();
                emitReserves();
                emitPayloadDrops();
                emitDrops();
                if (usesText_)
                    output_ << "declare i32 @aevix_text_copy(ptr, ptr, i64, i32)\n"
                            << "declare i32 @aevix_text_append(ptr, ptr, i64)\n"
                            << "declare i32 @aevix_text_push_char(ptr, i32)\n"
                            << "declare i64 @aevix_text_count(ptr, i64)\n"
                            << "declare i32 @aevix_text_decode(ptr, i64, i64)\n"
                            << "declare i32 @aevix_text_boundary(ptr, i64, i64)\n"
                            << "declare i32 @aevix_text_equal(ptr, i64, ptr, i64)\n";
                if (usesBench_)
                    output_ << "declare i64 @aevix_bench_begin(ptr)\n"
                            << "declare void @aevix_bench_end(i64)\n"
                            << "declare void @aevix_bench_report()\n";
                if (usesComplex_)
                    output_ << "declare double @aevix_complex_magnitude(double, double)\n"
                            << "declare void @aevix_complex_divide(double, double, double, double, ptr, ptr)\n";
                if (usesWideInteger_)
                    output_ << "declare i32 @aevix_integer128_binary(ptr, i64, i64, i64, i64, i32, i32)\n"
                            << "declare i64 @aevix_integer128_to_float(i64, i64, i32, i32)\n"
                            << "declare i32 @aevix_integer128_from_float(ptr, i64, i32)\n"
                            << "declare i32 @aevix_integer128_range_count(ptr, i64, i64, i64, i64, i64, i64, i32)\n";
                emitSmallFloatHelpers();
                if (usesParallel_)
                    output_ << "declare i64 @aevix_parallel_worker_count()\ndeclare void @aevix_parallel_for_each(ptr, i64, i64, ptr)\n";
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
            bool usesParallel_ = false;
            bool usesText_ = false;
            bool usesBench_ = false;
            bool usesWideInteger_ = false;
            std::map<std::string, std::pair<std::string, std::string>> textLiterals_;
            std::set<TypeId> dropTypes_;
            std::set<TypeId> reserveTypes_;
            std::set<TypeId> payloadDropTypes_;
            std::set<bool> smallFloatFormats_;
            bool usesComplex_ = false;
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
                        return "i16";
                    case TypeKind::Reference: return typed_.types.get(type.element).kind == TypeKind::Dynamic ? "{ ptr, ptr }" : "ptr";
                    case TypeKind::Box: return typed_.types.get(type.element).kind == TypeKind::Dynamic ? "{ ptr, ptr }" : "ptr";
                    case TypeKind::Dynamic: error(nullptr, "unsized dyn has no direct storage type."); return "{}";
                    case TypeKind::Vector: return "{ ptr, i64, i64 }";
                    case TypeKind::String: return "{ ptr, i64, i64 }";
                    case TypeKind::StringView: return "{ ptr, i64 }";
                    case TypeKind::Tuple:
                    {
                        std::string result = "{ ";
                        for (std::size_t index = 0; index < type.parameters.size(); ++index)
                        { if (index) result += ", "; result += llvmType(type.parameters[index]); }
                        return result + " }";
                    }
                    case TypeKind::Unit: return "{}";
                    case TypeKind::Simd: return "<" + std::to_string(type.extent) + " x " + llvmType(type.element) + ">";
                    case TypeKind::SimdMask: return "<" + std::to_string(type.extent) + " x i1>";
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
            void internText(const std::string& spelling)
            {
                if (textLiterals_.contains(spelling)) return;
                std::string bytes;
                for (std::size_t index = 1; index + 1 < spelling.size(); ++index)
                {
                    char character = spelling[index];
                    if (character != '\\') { bytes += character; continue; }
                    character = spelling[++index];
                    if (character == 'n') bytes += '\n'; else if (character == 'r') bytes += '\r';
                    else if (character == 't') bytes += '\t'; else if (character == '0') bytes += '\0';
                    else if (character == 'u')
                    {
                        const auto end = spelling.find('}', index + 2);
                        const auto scalar = static_cast<std::uint32_t>(std::strtoul(spelling.substr(index + 2, end - index - 2).c_str(), nullptr, 16));
                        if (scalar < 0x80) bytes += static_cast<char>(scalar);
                        else if (scalar < 0x800) { bytes += static_cast<char>(0xc0 | (scalar >> 6)); bytes += static_cast<char>(0x80 | (scalar & 63)); }
                        else if (scalar < 0x10000) { bytes += static_cast<char>(0xe0 | (scalar >> 12)); bytes += static_cast<char>(0x80 | ((scalar >> 6) & 63)); bytes += static_cast<char>(0x80 | (scalar & 63)); }
                        else { bytes += static_cast<char>(0xf0 | (scalar >> 18)); bytes += static_cast<char>(0x80 | ((scalar >> 12) & 63)); bytes += static_cast<char>(0x80 | ((scalar >> 6) & 63)); bytes += static_cast<char>(0x80 | (scalar & 63)); }
                        index = end;
                    }
                    else bytes += character;
                }
                const auto name = "@aevix.text." + std::to_string(textLiterals_.size());
                textLiterals_.emplace(spelling, std::make_pair(name, bytes));
            }
            void collectText(const Node& node)
            {
                if (node.kind == "Literal" && node.value.starts_with('"')) internText(node.value);
                for (const auto& child : node.children) collectText(*child);
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
                spelling = std::string(numericSpelling(spelling));
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
                UInt128 number;
                if (!UInt128::parse(spelling, number, base))
                { error(nullptr, "Invalid numeric literal reached LLVM emission."); return "0"; }
                return (negative && number ? "-" : "") + number.decimal();
            }
            std::string literal(const std::string& spelling, TypeId id)
            {
                const auto& type = typed_.types.get(id);
                if (type.kind == TypeKind::StringView)
                {
                    const auto found = textLiterals_.find(spelling);
                    if (found == textLiterals_.end()) { error(nullptr, "missing static UTF-8 literal."); return "zeroinitializer"; }
                    return "{ ptr " + found->second.first + ", i64 " + std::to_string(found->second.second.size()) + " }";
                }
                if (type.kind == TypeKind::Unit) return "zeroinitializer";
                if (type.kind == TypeKind::Float)
                {
                    std::string text(numericSpelling(spelling)); text.erase(std::remove(text.begin(), text.end(), '_'), text.end());
                    double number = std::strtod(text.c_str(), nullptr);
                    if (type.bits == 16) return std::to_string(encodeSmallFloat(number, type.name == "bf16"));
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
                        return constant.integerSpelling();
                    if (constant.kind == ConstantKind::Boolean) return constant.boolean ? "1" : "0";
                    if (constant.kind == ConstantKind::Unit) return "zeroinitializer";
                    if (constant.kind == ConstantKind::String) return literal(constant.text, expected);
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
            std::string decodedFloat(TypeId id, const std::string& operand)
            {
                const auto& type = typed_.types.get(id);
                if (type.bits == 64) return operand;
                const auto result = temporary();
                if (type.bits == 32) output_ << "  " << result << " = fpext float " << operand << " to double\n";
                else
                {
                    const bool bfloat = type.name == "bf16"; smallFloatFormats_.insert(bfloat);
                    output_ << "  " << result << " = call double @aevix.small.decode." << (bfloat ? "bf16" : "f16") << "(i16 " << operand << ")\n";
                }
                return result;
            }
            std::string encodedFloat(TypeId id, const std::string& operand)
            {
                const auto& type = typed_.types.get(id);
                if (type.bits == 64) return operand;
                const auto result = temporary();
                if (type.bits == 32) output_ << "  " << result << " = fptrunc double " << operand << " to float\n";
                else
                {
                    const bool bfloat = type.name == "bf16"; smallFloatFormats_.insert(bfloat);
                    output_ << "  " << result << " = call i16 @aevix.small.encode." << (bfloat ? "bf16" : "f16") << "(double " << operand << ")\n";
                }
                return result;
            }
            void emitComplex(const AIRInstruction& item)
            {
                usesComplex_ = true;
                const auto* information = record(item.dynamicSignature);
                const auto component = information->fieldTypes[0];
                const auto storage = llvmType(item.dynamicSignature);
                std::vector<std::string> components;
                for (const auto operand : item.operands)
                    for (std::size_t slot = 0; slot < 2; ++slot)
                    {
                        const auto part = temporary();
                        output_ << "  " << part << " = extractvalue " << storage << ' ' << value(operand) << ", " << slot << '\n';
                        components.push_back(decodedFloat(component, part));
                    }
                if (item.operation == "magnitude")
                {
                    const auto magnitude = temporary();
                    output_ << "  " << magnitude << " = call double @aevix_complex_magnitude(double " << components[0] << ", double " << components[1] << ")\n";
                    const auto encoded = encodedFloat(component, magnitude);
                    output_ << "  " << value(item.result) << " = freeze " << llvmType(component) << ' ' << encoded << '\n';
                }
                else
                {
                    output_ << "  call void @aevix_complex_divide(double " << components[0] << ", double " << components[1]
                        << ", double " << components[2] << ", double " << components[3] << ", ptr %complex.real." << item.result
                        << ", ptr %complex.imag." << item.result << ")\n";
                    const auto real = temporary(), imag = temporary();
                    output_ << "  " << real << " = load double, ptr %complex.real." << item.result << '\n'
                        << "  " << imag << " = load double, ptr %complex.imag." << item.result << '\n';
                    const auto encodedReal = encodedFloat(component, real), encodedImag = encodedFloat(component, imag), partial = temporary();
                    output_ << "  " << partial << " = insertvalue " << storage << " zeroinitializer, " << llvmType(component) << ' ' << encodedReal << ", 0\n"
                        << "  " << value(item.result) << " = insertvalue " << storage << ' ' << partial << ", " << llvmType(component) << ' ' << encodedImag << ", 1\n";
                }
            }
            void emitSmallFloatHelpers()
            {
                // 비트 연산으로 저장 형식을 변환하므로 x86 F16C나 ARM FP16 명령이 필요 없다.
                // Bitwise storage conversion needs neither x86 F16C nor ARM FP16 instructions.
                for (const bool bfloat : smallFloatFormats_)
                {
                    const unsigned fraction = bfloat ? 7 : 10, bias = bfloat ? 127 : 15, maxExponent = bfloat ? 255 : 31;
                    const unsigned shift = 52 - fraction, normal = 1024 - bias;
                    const auto suffix = bfloat ? "bf16" : "f16";
                    output_ << "define internal i16 @aevix.small.encode." << suffix << "(double %input) {\nentry:\n"
                        << "  %raw = bitcast double %input to i64\n  %sg = lshr i64 %raw, 48\n  %sign = and i64 %sg, 32768\n  %abs = and i64 %raw, 9223372036854775807\n  %exp = lshr i64 %abs, 52\n  %special = icmp eq i64 %exp, 2047\n  br i1 %special, label %nonfinite, label %finite\nnonfinite:\n"
                        << "  %frac = and i64 %abs, 4503599627370495\n  %nan = icmp ne i64 %frac, 0\n  %nanbit = select i1 %nan, i64 " << (1U << (fraction - 1)) << ", i64 0\n  %nf0 = or i64 %sign, " << (maxExponent << fraction) << "\n  %nf1 = or i64 %nf0, %nanbit\n  %nf = trunc i64 %nf1 to i16\n  ret i16 %nf\nfinite:\n"
                        << "  %overflow = icmp ugt i64 %exp, " << 1023 + bias << "\n  br i1 %overflow, label %infinity, label %classify\ninfinity:\n  %inf0 = or i64 %sign, " << (maxExponent << fraction) << "\n  %inf = trunc i64 %inf0 to i16\n  ret i16 %inf\nclassify:\n"
                        << "  %subnormal = icmp ult i64 %exp, " << normal << "\n  br i1 %subnormal, label %sub, label %normal\nnormal:\n  %normal0 = lshr i64 %abs, " << shift << "\n  %normal1 = sub i64 %normal0, " << (std::uint64_t(1023 - bias) << fraction) << "\n  br label %round\nsub:\n"
                        << "  %distance = sub i64 " << normal << ", %exp\n  %shift = add i64 %distance, " << shift << "\n  %tiny = icmp ugt i64 %shift, 63\n  br i1 %tiny, label %zero, label %subvalue\nzero:\n  %zerovalue = trunc i64 %sign to i16\n  ret i16 %zerovalue\nsubvalue:\n"
                        << "  %fracsub = and i64 %abs, 4503599627370495\n  %mantissa = or i64 %fracsub, 4503599627370496\n  %subpacked = lshr i64 %mantissa, %shift\n  br label %round\nround:\n"
                        << "  %mant = phi i64 [ %abs, %normal ], [ %mantissa, %subvalue ]\n  %nshift = phi i64 [ " << shift << ", %normal ], [ %shift, %subvalue ]\n  %packed = phi i64 [ %normal1, %normal ], [ %subpacked, %subvalue ]\n"
                        << "  %mask0 = shl i64 1, %nshift\n  %mask = sub i64 %mask0, 1\n  %rem = and i64 %mant, %mask\n  %halfshift = sub i64 %nshift, 1\n  %half = shl i64 1, %halfshift\n  %more = icmp ugt i64 %rem, %half\n  %tie = icmp eq i64 %rem, %half\n  %lsb = and i64 %packed, 1\n  %odd = icmp ne i64 %lsb, 0\n  %tieodd = and i1 %tie, %odd\n  %up = or i1 %more, %tieodd\n  %increment = zext i1 %up to i64\n  %rounded = add i64 %packed, %increment\n  %signed = or i64 %rounded, %sign\n  %result = trunc i64 %signed to i16\n  ret i16 %result\n}\n";
                    const double unit = std::ldexp(1.0, 1 - static_cast<int>(bias) - static_cast<int>(fraction));
                    std::ostringstream unitHex; unitHex << "0x" << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << std::bit_cast<std::uint64_t>(unit);
                    output_ << "define internal double @aevix.small.decode." << suffix << "(i16 %input) {\nentry:\n  %raw = zext i16 %input to i64\n  %sign0 = and i64 %raw, 32768\n  %sign = shl i64 %sign0, 48\n  %exp0 = lshr i64 %raw, " << fraction << "\n  %exp = and i64 %exp0, " << maxExponent << "\n  %frac = and i64 %raw, " << ((1U << fraction) - 1) << "\n  %subnormal = icmp eq i64 %exp, 0\n  br i1 %subnormal, label %sub, label %normal\nsub:\n"
                        << "  %magnitude = uitofp i64 %frac to double\n  %scaled = fmul double %magnitude, " << unitHex.str() << "\n  %negative = icmp ne i64 %sign, 0\n  %negated = fneg double %scaled\n  %subvalue = select i1 %negative, double %negated, double %scaled\n  ret double %subvalue\nnormal:\n"
                        << "  %special = icmp eq i64 %exp, " << maxExponent << "\n  %biased = add i64 %exp, " << 1023 - bias << "\n  %targetexp = select i1 %special, i64 2047, i64 %biased\n  %expbits = shl i64 %targetexp, 52\n  %fracbits = shl i64 %frac, " << shift << "\n  %body = or i64 %expbits, %fracbits\n  %bits = or i64 %body, %sign\n  %result = bitcast i64 %bits to double\n  ret double %result\n}\n";
                }
            }
            std::pair<std::string, std::string> wideParts(const std::string& operand)
            {
                const auto low = temporary(), shifted = temporary(), high = temporary();
                output_ << "  " << low << " = trunc i128 " << operand << " to i64\n"
                    << "  " << shifted << " = lshr i128 " << operand << ", 64\n"
                    << "  " << high << " = trunc i128 " << shifted << " to i64\n";
                return {low, high};
            }
            void wideArithmetic(const AIRInstruction& item, unsigned operation, const std::string& left, const std::string& right)
            {
                usesWideInteger_ = true;
                const auto a = wideParts(left), b = wideParts(right);
                const auto status = temporary(), failed = temporary();
                output_ << "  " << status << " = call i32 @aevix_integer128_binary(ptr %wide.tmp." << item.result
                    << ", i64 " << a.first << ", i64 " << a.second << ", i64 " << b.first << ", i64 " << b.second
                    << ", i32 " << operation << ", i32 " << (typed_.types.get(item.type).isSigned ? 1 : 0) << ")\n"
                    << "  " << failed << " = icmp ne i32 " << status << ", 0\n";
                guard(failed);
                output_ << "  " << value(item.result) << " = load i128, ptr %wide.tmp." << item.result << '\n';
            }
            std::string numericAsDouble(TypeId source, const std::string& operand)
            {
                const auto& type = typed_.types.get(source);
                if (type.kind == TypeKind::Float) return decodedFloat(source, operand);
                if (type.bits == 128) return numericConverted(source, typed_.types.builtin("f64"), operand);
                const auto result = temporary();
                output_ << "  " << result << " = " << (type.isSigned ? "sitofp" : "uitofp") << ' ' << llvmType(source) << ' ' << operand << " to double\n";
                return result;
            }
            std::string numericConverted(TypeId source, TypeId destination, const std::string& operand)
            {
                const auto& from = typed_.types.get(source); const auto& to = typed_.types.get(destination);
                if (source == destination) return operand;
                if (from.kind == TypeKind::Integer && from.bits == 128 && to.kind == TypeKind::Float)
                {
                    usesWideInteger_ = true;
                    const auto parts = wideParts(operand); const auto bits = temporary();
                    const auto format = to.bits == 64 ? 3 : to.bits == 32 ? 2 : to.name == "bf16" ? 1 : 0;
                    output_ << "  " << bits << " = call i64 @aevix_integer128_to_float(i64 " << parts.first << ", i64 " << parts.second
                        << ", i32 " << (from.isSigned ? 1 : 0) << ", i32 " << format << ")\n";
                    const auto result = temporary();
                    if (to.bits == 64) output_ << "  " << result << " = bitcast i64 " << bits << " to double\n";
                    else if (to.bits == 16) output_ << "  " << result << " = trunc i64 " << bits << " to i16\n";
                    else
                    {
                        const auto narrowed = temporary();
                        output_ << "  " << narrowed << " = trunc i64 " << bits << " to i32\n"
                            << "  " << result << " = bitcast i32 " << narrowed << " to float\n";
                    }
                    return result;
                }
                if (from.kind == TypeKind::Float && to.kind == TypeKind::Integer && to.bits == 128)
                {
                    usesWideInteger_ = true;
                    const auto decoded = decodedFloat(source, operand), bits = temporary(), status = temporary(), result = temporary();
                    output_ << "  " << bits << " = bitcast double " << decoded << " to i64\n"
                        << "  " << status << " = call i32 @aevix_integer128_from_float(ptr %wide.conversion, i64 " << bits << ", i32 " << (to.isSigned ? 1 : 0) << ")\n"
                        << "  " << result << " = load i128, ptr %wide.conversion\n";
                    return result;
                }
                if (to.kind == TypeKind::Float)
                {
                    if (from.kind == TypeKind::Integer && to.bits == 32)
                    {
                        const auto direct = temporary();
                        output_ << "  " << direct << " = " << (from.isSigned ? "sitofp" : "uitofp") << ' ' << llvmType(source) << ' ' << operand << " to float\n";
                        return direct;
                    }
                    auto decoded = numericAsDouble(source, operand);
                    if (from.kind == TypeKind::Integer && from.bits == 64 && to.bits == 16)
                    {
                        // 정수의 버려진 비트를 홀수 최하위 비트로 보존하여 이중 반올림을 막는다.
                        // Preserve discarded integer bits with an odd low bit to avoid double rounding.
                        const auto bits = temporary(), absolute = temporary(), exponent = temporary(), loses = temporary(), delta = temporary(), shift = temporary(), power = temporary(), mask = temporary(), residue = temporary(), exact = temporary(), oddBits = temporary(), chosen = temporary(), sticky = temporary();
                        output_ << "  " << bits << " = bitcast double " << decoded << " to i64\n"
                            << "  " << absolute << " = and i64 " << bits << ", 9223372036854775807\n"
                            << "  " << exponent << " = lshr i64 " << absolute << ", 52\n"
                            << "  " << loses << " = icmp ugt i64 " << exponent << ", 1075\n"
                            << "  " << delta << " = sub i64 " << exponent << ", 1075\n"
                            << "  " << shift << " = select i1 " << loses << ", i64 " << delta << ", i64 0\n"
                            << "  " << power << " = shl i64 1, " << shift << '\n'
                            << "  " << mask << " = sub i64 " << power << ", 1\n"
                            << "  " << residue << " = and i64 " << operand << ", " << mask << '\n'
                            << "  " << exact << " = icmp eq i64 " << residue << ", 0\n"
                            << "  " << oddBits << " = or i64 " << bits << ", 1\n"
                            << "  " << chosen << " = select i1 " << exact << ", i64 " << bits << ", i64 " << oddBits << '\n'
                            << "  " << sticky << " = bitcast i64 " << chosen << " to double\n";
                        decoded = sticky;
                    }
                    return encodedFloat(destination, decoded);
                }
                const auto result = temporary();
                if (from.kind == TypeKind::Float)
                {
                    const auto decoded = decodedFloat(source, operand);
                    output_ << "  " << result << " = " << (to.isSigned ? "fptosi" : "fptoui") << " double " << decoded << " to " << llvmType(destination) << '\n';
                }
                else if (from.bits == to.bits) return operand;
                else output_ << "  " << result << " = " << (from.bits > to.bits ? "trunc" : from.isSigned ? "sext" : "zext") << ' ' << llvmType(source) << ' ' << operand << " to " << llvmType(destination) << '\n';
                return result;
            }
            void numericInstruction(const AIRInstruction& item)
            {
                const auto source = valueType(item.operands[0]); const auto operand = value(item.operands[0]);
                if (item.opcode == AIROpcode::NumericConvert)
                {
                    const auto converted = numericConverted(source, item.type, operand);
                    output_ << "  " << value(item.result) << " = freeze " << llvmType(item.type) << ' ' << converted << '\n'; return;
                }
                const auto destination = item.dynamicSignature;
                const auto& from = typed_.types.get(source); const auto& to = typed_.types.get(destination);
                if (from.kind == TypeKind::Integer && to.kind == TypeKind::Integer)
                {
                    const auto extended = temporary(), low = temporary(), high = temporary(), valid = temporary();
                    const auto lower = to.isSigned ? "-" + (UInt128{1} << (to.bits - 1)).decimal() : std::string("0");
                    const auto upper = (to.isSigned ? (UInt128{1} << (to.bits - 1)) - UInt128{1} : UInt128::mask(static_cast<unsigned>(to.bits))).decimal();
                    output_ << "  " << extended << " = " << (from.bits == 128 ? "freeze" : from.isSigned ? "sext" : "zext") << ' ' << llvmType(source) << ' ' << operand;
                    if (from.bits != 128) output_ << " to i128";
                    output_ << '\n';
                    if (to.isSigned && from.isSigned) output_ << "  " << low << " = icmp sge i128 " << extended << ", " << lower << '\n';
                    else if (!to.isSigned && from.isSigned) output_ << "  " << low << " = icmp sge i128 " << extended << ", 0\n";
                    else output_ << "  " << low << " = freeze i1 true\n";
                    if (!to.isSigned && to.bits == 128 && from.isSigned) output_ << "  " << high << " = freeze i1 true\n";
                    else output_ << "  " << high << " = icmp " << (to.isSigned && from.isSigned ? "sle" : "ule") << " i128 " << extended << ", " << upper << '\n';
                    output_ << "  " << valid << " = and i1 " << low << ", " << high << '\n'
                        << "  " << value(item.result) << " = select i1 " << valid << ", i32 0, i32 1\n";
                    return;
                }
                const auto decoded = numericAsDouble(source, operand);
                if (to.kind == TypeKind::Integer && to.bits == 128)
                {
                    usesWideInteger_ = true; const auto bits = temporary();
                    output_ << "  " << bits << " = bitcast double " << decoded << " to i64\n"
                        << "  " << value(item.result) << " = call i32 @aevix_integer128_from_float(ptr %wide.conversion, i64 " << bits << ", i32 " << (to.isSigned ? 1 : 0) << ")\n";
                    return;
                }
                const auto sourceBits = temporary(), absBits = temporary(), finite = temporary();
                output_ << "  " << sourceBits << " = bitcast double " << decoded << " to i64\n"
                    << "  " << absBits << " = and i64 " << sourceBits << ", 9223372036854775807\n"
                    << "  " << finite << " = icmp ult i64 " << absBits << ", 9218868437227405312\n";
                if (to.kind == TypeKind::Integer)
                {
                    const auto lower = std::ldexp(-1.0, static_cast<int>(to.bits) - 1);
                    const auto upper = std::ldexp(1.0, static_cast<int>(to.bits) - (to.isSigned ? 1 : 0));
                    const auto lo = temporary(), hi = temporary(), bounded = temporary(), valid = temporary();
                    const auto boundConstant = [&](double number) { std::ostringstream stream; stream << "0x" << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << std::bit_cast<std::uint64_t>(number); return stream.str(); };
                    output_ << "  " << lo << " = fcmp oge double " << decoded << ", " << boundConstant(to.isSigned ? lower : 0.0) << '\n'
                        << "  " << hi << " = fcmp olt double " << decoded << ", " << boundConstant(upper) << '\n'
                        << "  " << bounded << " = and i1 " << lo << ", " << hi << '\n'
                        << "  " << valid << " = and i1 " << finite << ", " << bounded << '\n';
                    const auto safe = label(), bad = label(), done = label();
                    output_ << "  br i1 " << valid << ", label %" << safe << ", label %" << bad << '\n' << safe << ":\n";
                    const auto converted = numericConverted(source, destination, operand), restored = temporary(), exact = temporary(), goodStatus = temporary();
                    output_ << "  " << restored << " = " << (to.isSigned ? "sitofp" : "uitofp") << ' ' << llvmType(destination) << ' ' << converted << " to double\n"
                        << "  " << exact << " = fcmp oeq double " << restored << ", " << decoded << '\n'
                        << "  " << goodStatus << " = select i1 " << exact << ", i32 0, i32 3\n  br label %" << done << '\n' << bad << ":\n";
                    const auto badStatus = temporary();
                    output_ << "  " << badStatus << " = select i1 " << finite << ", i32 1, i32 2\n  br label %" << done << '\n' << done << ":\n"
                        << "  " << value(item.result) << " = phi i32 [ " << goodStatus << ", %" << safe << " ], [ " << badStatus << ", %" << bad << " ]\n";
                    return;
                }
                const auto converted = numericConverted(source, destination, operand), restored = decodedFloat(destination, converted);
                const auto convertedBits = temporary(), convertedAbs = temporary(), within = temporary();
                output_ << "  " << convertedBits << " = bitcast double " << restored << " to i64\n"
                    << "  " << convertedAbs << " = and i64 " << convertedBits << ", 9223372036854775807\n"
                    << "  " << within << " = icmp ult i64 " << convertedAbs << ", 9218868437227405312\n";
                std::string exact = "true";
                if (item.operation == "Float.convert_exact")
                {
                    if (from.kind == TypeKind::Integer)
                    {
                        // 정수의 하위 비트로 정확성을 판정하여 128비트 변환 런타임을 요구하지 않는다.
                        // Check integer low bits for exactness without a 128-bit conversion runtime.
                        const auto precision = to.bits == 64 ? 53 : to.bits == 32 ? 24 : to.name == "bf16" ? 8 : 11;
                        const auto exponent = temporary(), loses = temporary(), delta = temporary(), shift = temporary(), power = temporary(), mask = temporary(), extended = temporary(), residue = temporary();
                        output_ << "  " << exponent << " = lshr i64 " << convertedAbs << ", 52\n"
                            << "  " << loses << " = icmp ugt i64 " << exponent << ", " << 1022 + precision << '\n'
                            << "  " << delta << " = sub i64 " << exponent << ", " << 1022 + precision << '\n';
                        const auto finiteShift = temporary();
                        output_ << "  " << finiteShift << " = and i1 " << loses << ", " << within << '\n'
                            << "  " << shift << " = select i1 " << finiteShift << ", i64 " << delta << ", i64 0\n";
                        const auto storage = from.bits == 128 ? "i128" : "i64";
                        std::string count = shift;
                        if (from.bits == 128)
                        { count = temporary(); output_ << "  " << count << " = zext i64 " << shift << " to i128\n"; }
                        output_ << "  " << power << " = shl " << storage << " 1, " << count << '\n'
                            << "  " << mask << " = sub " << storage << ' ' << power << ", 1\n";
                        std::string wide = operand;
                        if (from.bits < 64) { output_ << "  " << extended << " = " << (from.isSigned ? "sext" : "zext") << ' ' << llvmType(source) << ' ' << operand << " to i64\n"; wide = extended; }
                        output_ << "  " << residue << " = and " << storage << ' ' << wide << ", " << mask << '\n';
                        exact = temporary(); output_ << "  " << exact << " = icmp eq " << storage << ' ' << residue << ", 0\n";
                    }
                    else { exact = temporary(); output_ << "  " << exact << " = fcmp oeq double " << restored << ", " << decoded << '\n'; }
                }
                const auto precisionStatus = temporary(), rangeStatus = temporary();
                output_ << "  " << precisionStatus << " = select i1 " << exact << ", i32 0, i32 3\n"
                    << "  " << rangeStatus << " = select i1 " << within << ", i32 " << precisionStatus << ", i32 1\n"
                    << "  " << value(item.result) << " = select i1 " << finite << ", i32 " << rangeStatus << ", i32 2\n";
            }
            void checkedArithmetic(const AIRInstruction& item, const std::string& operation, const std::string& left, const std::string& right)
            {
                const auto& scalar = typed_.types.get(item.type);
                if (scalar.bits == 128)
                { wideArithmetic(item, operation == "add" ? 0 : operation == "sub" ? 1 : 2, left, right); return; }
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
                if (floating && scalar.bits == 16)
                {
                    const auto a = decodedFloat(typeId, left), b = decodedFloat(typeId, right);
                    const bool comparison = op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
                    const auto computed = comparison ? value(item.result) : temporary();
                    if (comparison)
                    {
                        const auto predicate = op == "==" ? "oeq" : op == "!=" ? "une" : op == "<" ? "olt" : op == "<=" ? "ole" : op == ">" ? "ogt" : "oge";
                        output_ << "  " << computed << " = fcmp " << predicate << " double " << a << ", " << b << '\n';
                    }
                    else
                    {
                        const auto operation = op == "+" ? "fadd" : op == "-" ? "fsub" : op == "*" ? "fmul" : op == "/" ? "fdiv" : "frem";
                        output_ << "  " << computed << " = " << operation << " double " << a << ", " << b << '\n';
                        const auto rounded = encodedFloat(typeId, computed);
                        output_ << "  " << value(item.result) << " = freeze i16 " << rounded << '\n';
                    }
                    return;
                }

                if ((op == "+" || op == "-" || op == "*") && !floating)
                { checkedArithmetic(item, op == "+" ? "add" : op == "-" ? "sub" : "mul", left, right); return; }
                if (op == "/" || op == "%")
                {
                    if (!floating && scalar.bits == 128) { wideArithmetic(item, op == "/" ? 3 : 4, left, right); return; }
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
                    if (scalar.bits == 128)
                    {
                        usesWideInteger_ = true;
                        const auto a = wideParts(cursor), b = wideParts(end), c = wideParts(step);
                        const auto first = temporary(), second = temporary(), third = temporary(), descendingFlag = temporary(), activeFlag = temporary(), closedDescending = temporary(), flags = temporary(), signedFlags = temporary(), status = temporary();
                        output_ << "  " << first << " = zext i1 " << closed << " to i32\n"
                            << "  " << second << " = zext i1 " << descending << " to i32\n"
                            << "  " << third << " = zext i1 " << active << " to i32\n"
                            << "  " << descendingFlag << " = shl i32 " << second << ", 1\n"
                            << "  " << activeFlag << " = shl i32 " << third << ", 2\n"
                            << "  " << closedDescending << " = or i32 " << first << ", " << descendingFlag << '\n'
                            << "  " << flags << " = or i32 " << closedDescending << ", " << activeFlag << '\n'
                            << "  " << signedFlags << " = or i32 " << flags << ", " << (scalar.isSigned ? 8 : 0) << '\n'
                            << "  " << status << " = call i32 @aevix_integer128_range_count(ptr %wide.range.count, i64 " << a.first << ", i64 " << a.second
                            << ", i64 " << b.first << ", i64 " << b.second << ", i64 " << c.first << ", i64 " << c.second << ", i32 " << signedFlags << ")\n";
                        if (item.opcode == AIROpcode::RangeCountFits) output_ << "  " << value(item.result) << " = icmp eq i32 " << status << ", 0\n";
                        else output_ << "  " << value(item.result) << " = load i64, ptr %wide.range.count\n";
                        return;
                    }
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
            std::pair<std::string, std::string> textDescriptor(AIRValueId input)
            {
                auto type = valueType(input); auto descriptor = value(input);
                if (typed_.types.get(type).kind == TypeKind::Reference)
                { type = typed_.types.get(type).element; descriptor = temporary(); output_ << "  " << descriptor << " = load " << llvmType(type) << ", ptr " << value(input) << '\n'; }
                const auto data = temporary(), length = temporary();
                output_ << "  " << data << " = extractvalue " << llvmType(type) << ' ' << descriptor << ", 0\n"
                        << "  " << length << " = extractvalue " << llvmType(type) << ' ' << descriptor << ", 1\n";
                return {data, length};
            }
            void emitText(const AIRInstruction& item)
            {
                const auto& operation = item.operation;
                const auto resultType = llvmType(item.type);
                if (operation == "text.clear")
                {
                    const auto length = temporary();
                    output_ << "  " << length << " = getelementptr { ptr, i64, i64 }, ptr " << value(item.operands[0]) << ", i32 0, i32 1\n"
                            << "  store i64 0, ptr " << length << "\n  " << value(item.result) << " = freeze {} zeroinitializer\n"; return;
                }
                const auto carrier = typed_.carriers.find(item.type);
                if (carrier == typed_.carriers.end())
                {
                    const auto [data, length] = textDescriptor(item.operands[0]);
                    if (operation == "text.byte_len") output_ << "  " << value(item.result) << " = add i64 " << length << ", 0\n";
                    else if (operation == "text.is_empty") output_ << "  " << value(item.result) << " = icmp eq i64 " << length << ", 0\n";
                    else if (operation == "text.view" || operation == "text.as_bytes")
                    {
                        const auto partial = temporary();
                        output_ << "  " << partial << " = insertvalue " << resultType << " zeroinitializer, ptr " << data << ", 0\n"
                                << "  " << value(item.result) << " = insertvalue " << resultType << ' ' << partial << ", i64 " << length << ", 1\n";
                    }
                    else if (operation == "text.scalar_count")
                    { usesText_ = true; output_ << "  " << value(item.result) << " = call i64 @aevix_text_count(ptr " << data << ", i64 " << length << ")\n"; }
                    else if (operation == "text.decode")
                    { usesText_ = true; output_ << "  " << value(item.result) << " = call i32 @aevix_text_decode(ptr " << data << ", i64 " << length << ", i64 " << value(item.operands[1]) << ")\n"; }
                    else if (operation == "text.next")
                    {
                        const auto scalar = value(item.operands[1]); const auto one = temporary(), two = temporary(), three = temporary(), amount2 = temporary(), amount3 = temporary(), amount4 = temporary();
                        output_ << "  " << one << " = icmp ult i32 " << scalar << ", 128\n"
                                << "  " << two << " = icmp ult i32 " << scalar << ", 2048\n"
                                << "  " << three << " = icmp ult i32 " << scalar << ", 65536\n"
                                << "  " << amount2 << " = select i1 " << one << ", i64 1, i64 2\n"
                                << "  " << amount3 << " = select i1 " << two << ", i64 " << amount2 << ", i64 3\n"
                                << "  " << amount4 << " = select i1 " << three << ", i64 " << amount3 << ", i64 4\n"
                                << "  " << value(item.result) << " = add i64 " << value(item.operands[2]) << ", " << amount4 << '\n';
                    }
                    else if (operation == "text.equals")
                    {
                        usesText_ = true; const auto [rightData, rightLength] = textDescriptor(item.operands[1]); const auto equal = temporary();
                        output_ << "  " << equal << " = call i32 @aevix_text_equal(ptr " << data << ", i64 " << length << ", ptr " << rightData << ", i64 " << rightLength << ")\n"
                                << "  " << value(item.result) << " = icmp ne i32 " << equal << ", 0\n";
                    }
                    else error(item.source, "unknown text operation " + operation + ".");
                    return;
                }
                usesText_ = true;
                const auto& result = carrier->second; const auto payloadType = llvmType(result.value);
                const auto payload = "%textpayload" + std::to_string(item.result);
                const auto errorStorage = "%texterror" + std::to_string(item.result);
                const auto storage = "%enumtmp" + std::to_string(item.result);
                const auto status = temporary(); std::string data, length;
                if (operation == "text.from_view" || operation == "text.from_utf8")
                {
                    std::tie(data, length) = textDescriptor(item.operands[0]);
                    output_ << "  " << status << " = call i32 @aevix_text_copy(ptr " << payload << ", ptr " << data << ", i64 " << length << ", i32 " << (operation == "text.from_utf8" ? 1 : 0) << ")\n";
                }
                else if (operation == "text.append")
                {
                    std::tie(data, length) = textDescriptor(item.operands[1]);
                    output_ << "  " << status << " = call i32 @aevix_text_append(ptr " << value(item.operands[0]) << ", ptr " << data << ", i64 " << length << ")\n";
                }
                else if (operation == "text.push_char")
                    output_ << "  " << status << " = call i32 @aevix_text_push_char(ptr " << value(item.operands[0]) << ", i32 " << value(item.operands[1]) << ")\n";
                else if (operation == "text.slice_bytes")
                {
                    std::tie(data, length) = textDescriptor(item.operands[0]);
                    const auto start = value(item.operands[1]), end = value(item.operands[2]);
                    const auto a = temporary(), b = temporary(), aok = temporary(), bok = temporary(), order = temporary(), both = temporary(), good = temporary();
                    output_ << "  " << a << " = call i32 @aevix_text_boundary(ptr " << data << ", i64 " << length << ", i64 " << start << ")\n"
                            << "  " << b << " = call i32 @aevix_text_boundary(ptr " << data << ", i64 " << length << ", i64 " << end << ")\n"
                            << "  " << aok << " = icmp ne i32 " << a << ", 0\n  " << bok << " = icmp ne i32 " << b << ", 0\n"
                            << "  " << order << " = icmp ule i64 " << start << ", " << end << "\n  " << both << " = and i1 " << aok << ", " << bok << '\n'
                            << "  " << good << " = and i1 " << both << ", " << order << "\n  " << status << " = select i1 " << good << ", i32 0, i32 4\n";
                }
                else { error(item.source, "unknown fallible text operation."); return; }
                const auto ok = temporary(), yes = label(), no = label(), done = label();
                output_ << "  " << ok << " = icmp eq i32 " << status << ", 0\n  br i1 " << ok << ", label %" << yes << ", label %" << no << '\n' << yes << ":\n";
                std::string successValue = "zeroinitializer";
                if (operation == "text.from_view" || operation == "text.from_utf8")
                { successValue = temporary(); output_ << "  " << successValue << " = load " << payloadType << ", ptr " << payload << '\n'; }
                else if (operation == "text.slice_bytes")
                {
                    const auto pointer = temporary(), count = temporary(), partial = temporary(); successValue = temporary();
                    output_ << "  " << pointer << " = getelementptr i8, ptr " << data << ", i64 " << value(item.operands[1]) << '\n'
                            << "  " << count << " = sub i64 " << value(item.operands[2]) << ", " << value(item.operands[1]) << '\n'
                            << "  " << partial << " = insertvalue " << payloadType << " zeroinitializer, ptr " << pointer << ", 0\n"
                            << "  " << successValue << " = insertvalue " << payloadType << ' ' << partial << ", i64 " << count << ", 1\n";
                }
                enumStore(storage, typed_.enumVariants.at(result.success), {{result.value, successValue}});
                output_ << "  br label %" << done << '\n' << no << ":\n";
                const auto tag = temporary(), errorValue = temporary();
                output_ << "  store " << llvmType(result.error) << " zeroinitializer, ptr " << errorStorage << '\n'
                        << "  " << tag << " = sub i32 " << status << ", 1\n  store i32 " << tag << ", ptr " << errorStorage << '\n'
                        << "  " << errorValue << " = load " << llvmType(result.error) << ", ptr " << errorStorage << '\n';
                enumStore(storage, typed_.enumVariants.at(result.failure), {{result.error, errorValue}});
                output_ << "  br label %" << done << '\n' << done << ":\n  " << value(item.result) << " = load " << resultType << ", ptr " << storage << '\n';
            }
            void emitInstruction(const AIRInstruction& item)
            {
                const auto type = item.type == InvalidTypeId ? std::string{} : llvmType(item.type);
                switch (item.opcode)
                {
                    case AIROpcode::TextIntrinsic: emitText(item); return;
                    case AIROpcode::ArrayRepeat:
                    {
                        const auto count = typed_.types.get(item.type).extent;
                        const auto storage = "%repeattmp" + std::to_string(item.result);
                        const auto entry = label(), test = label(), body = label(), done = label();
                        const auto index = temporary(), next = temporary(), stop = temporary(), pointer = temporary();
                        output_ << "  br label %" << entry << '\n' << entry << ":\n  br label %" << test << '\n'
                                << test << ":\n  " << index << " = phi i64 [ 0, %" << entry << " ], [ " << next << ", %" << body << " ]\n"
                                << "  " << stop << " = icmp eq i64 " << index << ", " << count << "\n  br i1 " << stop << ", label %" << done << ", label %" << body << '\n'
                                << body << ":\n  " << pointer << " = getelementptr " << type << ", ptr " << storage << ", i32 0, i64 " << index << '\n'
                                << "  store " << llvmType(valueType(item.operands[0])) << ' ' << value(item.operands[0]) << ", ptr " << pointer << '\n'
                                << "  " << next << " = add i64 " << index << ", 1\n  br label %" << test << '\n'
                                << done << ":\n  " << value(item.result) << " = load " << type << ", ptr " << storage << '\n';
                        return;
                    }
                    case AIROpcode::RangeConstruct: case AIROpcode::RangeTest: case AIROpcode::RangeNext: case AIROpcode::RangeValue:
                    case AIROpcode::RangeWithStep: case AIROpcode::RangeCount: case AIROpcode::RangeCountFits: emitRange(item); return;
                    case AIROpcode::SliceConstruct:
                    {
                        const auto [pointer, storageType] = address(item.place);
                        if (typed_.types.get(storageType).kind != TypeKind::Array)
                        {
                            const auto descriptor = temporary(), data = temporary(), length = temporary(), partial = temporary();
                            output_ << "  " << descriptor << " = load " << llvmType(storageType) << ", ptr " << pointer << '\n'
                                    << "  " << data << " = extractvalue " << llvmType(storageType) << ' ' << descriptor << ", 0\n"
                                    << "  " << length << " = extractvalue " << llvmType(storageType) << ' ' << descriptor << ", 1\n"
                                    << "  " << partial << " = insertvalue " << type << " zeroinitializer, ptr " << data << ", 0\n"
                                    << "  " << value(item.result) << " = insertvalue " << type << ' ' << partial << ", i64 " << length << ", 1\n";
                            return;
                        }
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
                    case AIROpcode::SliceSubview:
                    {
                        const auto data = temporary(), pointer = temporary(), count = temporary(), descriptor = temporary();
                        const auto element = typed_.types.get(item.type).element;
                        output_ << "  " << data << " = extractvalue " << type << ' ' << value(item.operands[0]) << ", 0\n"
                                << "  " << pointer << " = getelementptr " << llvmType(element) << ", ptr " << data << ", i64 " << value(item.operands[1]) << '\n'
                                << "  " << count << " = sub i64 " << value(item.operands[2]) << ", " << value(item.operands[1]) << '\n'
                                << "  " << descriptor << " = insertvalue " << type << " undef, ptr " << pointer << ", 0\n"
                                << "  " << value(item.result) << " = insertvalue " << type << ' ' << descriptor << ", i64 " << count << ", 1\n";
                        return;
                    }
                    case AIROpcode::ParallelIntrinsic:
                    {
                        usesParallel_ = true;
                        if (item.operation == "worker_count")
                        { output_ << "  " << value(item.result) << " = call i64 @aevix_parallel_worker_count()\n"; return; }
                        const auto sliceType = valueType(item.operands[0]);
                        const auto elementType = typed_.types.get(sliceType).element;
                        const auto data = temporary(); const auto length = temporary();
                        output_ << "  " << data << " = extractvalue " << llvmType(sliceType) << ' ' << value(item.operands[0]) << ", 0\n"
                                << "  " << length << " = extractvalue " << llvmType(sliceType) << ' ' << value(item.operands[0]) << ", 1\n"
                                << "  call void @aevix_parallel_for_each(ptr " << data << ", i64 " << length << ", i64 " << layout(elementType).size
                                << ", ptr @aevix.fn." << item.callee << ")\n"
                                << "  " << value(item.result) << " = freeze {} zeroinitializer\n";
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
                        if (item.operation == "+" || item.operation == "byte.from_u8" || item.operation == "byte.to_u8") output_ << "  " << value(item.result) << " = freeze " << type << ' ' << operand << '\n';
                        else if (item.operation == "~" || item.operation == "!") output_ << "  " << value(item.result) << " = xor " << type << ' ' << operand << ", -1\n";
                        else if (item.operation == "-" && typed_.types.get(item.type).kind == TypeKind::Float && typed_.types.get(item.type).bits == 16) output_ << "  " << value(item.result) << " = xor i16 " << operand << ", 32768\n";
                        else if (item.operation == "-" && typed_.types.get(item.type).kind == TypeKind::Float) output_ << "  " << value(item.result) << " = fneg " << type << ' ' << operand << '\n';
                        else if (item.operation == "-") checkedArithmetic(item, "sub", "0", operand);
                        else error(item.source, "unsupported unary operation.");
                        return;
                    }
                    case AIROpcode::ComplexIntrinsic: emitComplex(item); return;
                    case AIROpcode::Simd:
                        emitSimdInstruction(item, typed_.types, {output_, overflowIntrinsics_,
                            [this](TypeId id) { return llvmType(id); },
                            [this](AIRValueId id) { return valueType(id); },
                            [this](AIRValueId id) { return value(id); },
                            [this]() { return temporary(); },
                            [this](const std::string& failure) { guard(failure); }});
                        return;
                    case AIROpcode::NumericStatus: case AIROpcode::NumericConvert: numericInstruction(item); return;
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
                if (type.kind == TypeKind::Vector || type.kind == TypeKind::Box || type.kind == TypeKind::String) return true;
                if (!active.insert(id).second) return false;
                bool result = type.kind == TypeKind::Array && needsDrop(type.element, active);
                if (type.kind == TypeKind::Tuple) for (const auto field : type.parameters) result = result || needsDrop(field, active);
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
                    else if (type.kind == TypeKind::String)
                    {
                        usesAllocation_ = true;
                        output_ << "  %text = load { ptr, i64, i64 }, ptr %object\n  %data = extractvalue { ptr, i64, i64 } %text, 0\n  call void @free(ptr %data)\n";
                    }
                    else if (type.kind == TypeKind::Tuple)
                    {
                        for (std::size_t index = type.parameters.size(); index > 0; --index)
                        {
                            const auto pointer = temporary();
                            output_ << "  " << pointer << " = getelementptr " << llvmType(id) << ", ptr %object, i32 0, i32 " << index - 1 << '\n';
                            emitDropCall(type.parameters[index - 1], pointer);
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
            bool benchFunction(const AIRFunction& function) const
            {
                return function.source && std::any_of(function.source->children.begin(), function.source->children.end(),
                    [](const auto& marker) { return marker->kind == "Bench"; });
            }
            void emitFunction(const AIRFunction& function)
            {
                function_ = &function;
                temporary_ = 0; label_ = 0;
                output_ << "define " << llvmType(function.result) << " @aevix.fn." << function.symbol << '(';
                for (std::size_t index = 0; index < function.parameters.size(); ++index)
                { if (index) output_ << ", "; output_ << llvmType(function.locals[function.parameters[index]].type) << " %arg" << index; }
                output_ << ')';
                if (typed_.types.get(function.result).kind == TypeKind::Never) output_ << " noreturn";
                output_ << " {\n";
                for (std::size_t block = 0; block < function.blocks.size(); ++block)
                {
                    output_ << "b" << block << ":\n";
                    if (block == 0)
                    {
                        // 토큰은 SSA 값이므로 중첩·재귀 호출도 각 실행의 시작과 종료를 연결한다.
                        // An SSA token pairs each invocation's entry and exit, including nesting and recursion.
                        if (benchFunction(function))
                            output_ << "  %bench.token = call i64 @aevix_bench_begin(ptr @aevix.bench.name." << function.symbol << ")\n";
                        for (std::size_t index = 0; index < function.locals.size(); ++index)
                            output_ << "  %l" << index << " = alloca " << llvmType(function.locals[index].type) << "\n"
                                    << "  %initialized" << index << " = alloca i1\n  store i1 false, ptr %initialized" << index << '\n';
                        for (const auto& basicBlock : function.blocks)
                            for (const auto& instruction : basicBlock.instructions)
                            {
                                if (instruction.opcode == AIROpcode::ComplexIntrinsic && instruction.operation == "divide_components")
                                    output_ << "  %complex.real." << instruction.result << " = alloca double\n  %complex.imag." << instruction.result << " = alloca double\n";
                                if (instruction.opcode == AIROpcode::ArrayRepeat)
                                    output_ << "  %repeattmp" << instruction.result << " = alloca " << llvmType(instruction.type) << '\n';
                                if (instruction.opcode == AIROpcode::EnumConstruct || instruction.operation == "box.new"
                                    || (instruction.opcode == AIROpcode::TextIntrinsic && typed_.carriers.contains(instruction.type)))
                                {
                                    output_ << "  %enumtmp" << instruction.result << " = alloca " << llvmType(instruction.type) << '\n';
                                    if (instruction.operation == "box.new") output_ << "  %boxerrtmp" << instruction.result << " = alloca " << llvmType(typed_.carriers.at(instruction.type).error) << '\n';
                                    if (instruction.opcode == AIROpcode::TextIntrinsic)
                                    {
                                        const auto& carrier = typed_.carriers.at(instruction.type);
                                        output_ << "  %textpayload" << instruction.result << " = alloca " << llvmType(carrier.value) << '\n'
                                                << "  %texterror" << instruction.result << " = alloca " << llvmType(carrier.error) << '\n';
                                    }
                                }
                            }
                        bool wideConversion = false, wideRangeCount = false;
                        for (const auto& basicBlock : function.blocks)
                            for (const auto& instruction : basicBlock.instructions)
                            {
                                if (instruction.result != InvalidAIRId && typed_.types.get(instruction.type).kind == TypeKind::Integer
                                    && typed_.types.get(instruction.type).bits == 128
                                    && (instruction.opcode == AIROpcode::Binary || instruction.opcode == AIROpcode::Unary))
                                    output_ << "  %wide.tmp." << instruction.result << " = alloca i128\n";
                                if ((instruction.opcode == AIROpcode::RangeCount || instruction.opcode == AIROpcode::RangeCountFits) && !instruction.operands.empty())
                                {
                                    const auto rangeType = typed_.types.get(valueType(instruction.operands[0]));
                                    if (rangeType.kind == TypeKind::Range && typed_.types.get(rangeType.element).bits == 128) wideRangeCount = true;
                                }
                                const auto destination = instruction.opcode == AIROpcode::NumericStatus ? instruction.dynamicSignature : instruction.type;
                                if ((instruction.opcode == AIROpcode::NumericStatus || instruction.opcode == AIROpcode::NumericConvert)
                                    && typed_.types.get(destination).kind == TypeKind::Integer && typed_.types.get(destination).bits == 128)
                                    wideConversion = true;
                            }
                        if (wideConversion) output_ << "  %wide.conversion = alloca i128\n";
                        if (wideRangeCount) output_ << "  %wide.range.count = alloca i64\n";
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
                        if (benchFunction(function)) output_ << "  call void @aevix_bench_end(i64 %bench.token)\n";
                        output_ << "  ret " << llvmType(function.result);
                        if (end.value != InvalidAIRId) output_ << ' ' << value(end.value);
                        output_ << '\n';
                    }
                    else if (end.kind == AIRTerminatorKind::Trap)
                        output_ << "  call void @llvm.trap()\n  unreachable\n";
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
