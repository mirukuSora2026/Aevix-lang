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
#include "aevix/Optimizer/ProofRangeAnalysis.hpp"
#include <chrono>
#include "aevix/Native/AtomicOperation.hpp"
#include "aevix/Native/TaskOperation.hpp"
#include "aevix/Native/RangeOperation.hpp"
#include "aevix/Frontend/Ownership/ThreadSafetyAnalysis.hpp"
#include "aevix/Native/SyncOperation.hpp"
#include "aevix/Backend/LLVM/SimdCodeGen.hpp"
#include "aevix/Frontend/Lexer/LiteralText.hpp"
#include "aevix/Frontend/Sema/NumericLiteral.hpp"
#include "aevix/Frontend/Sema/SmallFloat.hpp"
#include "aevix/Native/OsOperation.hpp"
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
                    Diagnostics& diagnostics, std::string triple, ProofOptimizationOptions proof)
                : module_(module), typed_(typed), names_(names), diagnostics_(diagnostics), triple_(std::move(triple)), proofOptions_(proof) {}
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
                emitVectorTruncations();
                emitPayloadDrops();
                emitSharedPayloadDrops();
                emitTaskThunks();
                emitDrops();
                if (usesTask_)
                    output_ << "declare i32 @aevix_task_scope_open(ptr)\n"
                            << "declare i32 @aevix_task_scope_prepare_identified(i64, i64, i64, i64, i64, ptr, ptr, ptr, ptr, ptr, ptr)\n"
                            << "declare i32 @aevix_scoped_task_commit(i64)\n"
                            << "declare i32 @aevix_task_scope_prepare_ready(i64, i64, i64, ptr, ptr, ptr, ptr)\n"
                            << "declare i32 @aevix_scoped_task_ready_commit(i64)\n"
                            << "declare i32 @aevix_scoped_task_join(i64, ptr, i64)\n"
                            << "declare i32 @aevix_scoped_task_try_take(i64, ptr, i64)\n"
                            << "declare i32 @aevix_scoped_task_discard(i64)\n"
                            << "declare i32 @aevix_scoped_task_wait(i64)\n"
                            << "declare i32 @aevix_scoped_task_wait_timeout(i64, i64)\n"
                            << "declare i32 @aevix_scoped_task_request_cancel(i64, ptr)\n"
                            << "declare i32 @aevix_scoped_task_snapshot(i64, ptr)\n"
                            << "declare i32 @aevix_scoped_task_cancel_token(i64, ptr)\n"
                            << "declare i32 @aevix_task_scope_close_admission(i64)\n"
                            << "declare i32 @aevix_task_scope_active_count(i64, ptr)\n"
                            << "declare i32 @aevix_task_scope_status(i64, ptr)\n"
                            << "declare i32 @aevix_task_scope_close(i64, ptr)\n"
                            << "declare i32 @aevix_task_scope_cleanup(i64)\n"
                            << "declare i32 @aevix_task_last_error()\n"
                            << "declare i32 @aevix_task_error_domain()\n";
                if (usesCancellation_)
                    output_ << "declare i32 @aevix_cancel_source_new(ptr)\n"
                            << "declare i32 @aevix_cancel_source_clone(i64, ptr)\n"
                            << "declare i32 @aevix_cancel_token_from_source(i64, ptr)\n"
                            << "declare i32 @aevix_cancel_token_clone(i64, ptr)\n"
                            << "declare i32 @aevix_cancel_source_request(i64, ptr)\n"
                            << "declare i32 @aevix_cancel_token_requested(i64, ptr)\n"
                            << "declare i32 @aevix_cancel_source_release(i64)\n"
                            << "declare i32 @aevix_cancel_token_release(i64)\n";
                if (usesSync_)
                    output_ << "declare i32 @aevix_mutex_lock(ptr, ptr)\n"
                            << "declare i32 @aevix_mutex_try_lock(ptr, ptr)\n"
                            << "declare i32 @aevix_mutex_lock_timeout(ptr, i64, ptr)\n"
                            << "declare i32 @aevix_mutex_unlock(ptr, i32)\n"
                            << "declare i32 @aevix_mutex_is_poisoned(ptr, ptr)\n"
                            << "declare i32 @aevix_mutex_clear_poison(ptr)\n"
                            << "declare i32 @aevix_rwlock_read(ptr, ptr)\n"
                            << "declare i32 @aevix_rwlock_write(ptr, ptr)\n"
                            << "declare i32 @aevix_rwlock_try_read(ptr, ptr)\n"
                            << "declare i32 @aevix_rwlock_try_write(ptr, ptr)\n"
                            << "declare i32 @aevix_rwlock_read_timeout(ptr, i64, ptr)\n"
                            << "declare i32 @aevix_rwlock_write_timeout(ptr, i64, ptr)\n"
                            << "declare i32 @aevix_rwlock_read_unlock(ptr)\n"
                            << "declare i32 @aevix_rwlock_write_unlock(ptr, i32)\n"
                            << "declare i32 @aevix_rwlock_is_poisoned(ptr, ptr)\n"
                            << "declare i32 @aevix_rwlock_clear_poison(ptr)\n";
                if (usesAtomic_)
                    output_ << "declare i32 @aevix_atomic_operation(ptr, i32, i32, i64, i64, i32, i32, ptr, ptr)\n"
                            << "declare i32 @aevix_atomic_fence(i32, i32)\n"
                            << "declare i32 @aevix_atomic_wait(ptr, i32, i64, i32, i64, i32, ptr, ptr)\n"
                            << "declare i32 @aevix_atomic_alignment(i32)\n"
                            << "declare i32 @aevix_atomic_lock_free(i32)\n";
                if (usesShared_)
                    output_ << "declare i32 @aevix_shared_prepare(i64, i64, ptr, ptr, ptr)\n"
                            << "declare i32 @aevix_shared_commit(i64)\n"
                            << "declare i32 @aevix_shared_clone(i64, ptr)\n"
                            << "declare i32 @aevix_shared_release(i64)\n"
                            << "declare i32 @aevix_shared_downgrade(i64, ptr)\n"
                            << "declare i32 @aevix_weak_clone(i64, ptr)\n"
                            << "declare i32 @aevix_weak_release(i64)\n"
                            << "declare i32 @aevix_weak_upgrade(i64, ptr)\n"
                            << "declare i32 @aevix_shared_payload(i64, ptr)\n"
                            << "declare i32 @aevix_shared_unique_payload(i64, ptr)\n"
                            << "declare i32 @aevix_shared_take(i64, ptr, i64)\n"
                            << "declare i32 @aevix_shared_counts(i64, ptr, ptr)\n"
                            << "declare i32 @aevix_shared_same_allocation(i64, i64, ptr)\n";
                if (usesText_)
                    output_ << "declare i32 @aevix_text_copy(ptr, ptr, i64, i32)\n"
                            << "declare i32 @aevix_text_append(ptr, ptr, i64)\n"
                            << "declare i32 @aevix_text_push_char(ptr, i32)\n"
                            << "declare i64 @aevix_text_count(ptr, i64)\n"
                            << "declare i32 @aevix_text_decode(ptr, i64, i64)\n"
                            << "declare i32 @aevix_text_boundary(ptr, i64, i64)\n"
                            << "declare i32 @aevix_text_equal(ptr, i64, ptr, i64)\n";
                if (usesOs_)
                    output_ << "declare i32 @aevix_os_file_open_utf8(ptr, i64, i32, ptr)\n"
                            << "declare i32 @aevix_os_file_read(i64, ptr, i64, ptr)\n"
                            << "declare i32 @aevix_os_file_write(i64, ptr, i64, ptr)\n"
                            << "declare i32 @aevix_os_file_seek(i64, i64, i32, ptr)\n"
                            << "declare i32 @aevix_os_file_flush(i64)\n"
                            << "declare i32 @aevix_os_file_close(i64)\n"
                            << "declare i32 @aevix_os_file_size(i64, ptr)\n"
                            << "declare i32 @aevix_os_file_set_len(i64, i64)\n"
                            << "declare void @aevix_os_file_cleanup(i64)\n"
                            << "declare i32 @aevix_os_last_error()\n"
                            << "declare i32 @aevix_os_error_kind()\n";
                if (usesOs_)
                    output_ << "declare i32 @aevix_os_env_get_utf8(ptr, i64, ptr, ptr, ptr)\n"
                            << "declare i32 @aevix_os_env_set_utf8(ptr, i64, ptr, i64)\n"
                            << "declare i32 @aevix_os_env_remove_utf8(ptr, i64)\n"
                            << "declare i32 @aevix_os_monotonic_ns(ptr)\n"
                            << "declare i32 @aevix_os_unix_time_ns(ptr)\n"
                            << "declare i32 @aevix_os_sleep_ns(i64)\n"
                            << "declare i32 @aevix_os_path_normalize_utf8(ptr, i64, ptr, ptr)\n"
                            << "declare i32 @aevix_os_path_metadata_utf8(ptr, i64, i32, ptr)\n"
                            << "declare i32 @aevix_os_path_mkdir_utf8(ptr, i64)\n"
                            << "declare i32 @aevix_os_path_remove_utf8(ptr, i64)\n"
                            << "declare i32 @aevix_os_process_spawn(ptr, i64, ptr)\n"
                            << "declare i32 @aevix_os_process_wait(i64, ptr)\n"
                            << "declare i32 @aevix_os_process_try_wait(i64, ptr)\n"
                            << "declare i32 @aevix_os_process_kill(i64)\n"
                            << "declare void @aevix_os_process_cleanup(i64)\n";
                if (usesParse_)
                    output_ << "declare i32 @aevix_parse_scalar(ptr, i64, i32, i32, i32, ptr)\n";
                if (usesSystem_)
                    output_ << "declare i32 @aevix_system_output(ptr, i64, i8, i8, ptr, i64)\n"
                            << "declare i32 @aevix_system_flush(i8)\n"
                            << "declare i32 @aevix_system_input(i8, ptr, ptr, ptr)\n"
                            << "declare i32 @aevix_system_error_code()\n"
                            << "declare i64 @aevix_system_error_progress()\n";
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
                if (usesMemmove_) output_ << "declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1 immarg)\n";
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
            ProofOptimizationOptions proofOptions_;
            ProofRangeResult proofRanges_;
            const AIRInstruction* currentInstruction_ = nullptr;
            std::ostringstream output_;
            std::set<std::string> overflowIntrinsics_;
            const AIRFunction* function_ = nullptr;
            std::size_t temporary_ = 0;
            std::size_t label_ = 0;
            bool hasPersist_ = false;
            bool usesAllocation_ = false;
            bool usesMemmove_ = false;
            bool usesParallel_ = false;
            bool usesText_ = false;
            bool usesParse_ = false;
            bool usesSystem_ = false;
            bool usesOs_ = false;
            bool usesShared_ = false;
            bool usesAtomic_ = false;
            bool usesCancellation_ = false;
            bool usesTask_ = false;
            std::set<std::tuple<TypeId,TypeId,SymbolId>> taskThunks_;
            std::set<TypeId> taskReadyDrops_;
            bool usesSync_ = false;
            bool usesBench_ = false;
            bool usesWideInteger_ = false;
            std::map<std::string, std::pair<std::string, std::string>> textLiterals_;
            std::set<TypeId> dropTypes_;
            std::set<TypeId> reserveTypes_;
            std::set<TypeId> truncateTypes_;
            std::set<TypeId> payloadDropTypes_;
            std::set<TypeId> sharedPayloadDropTypes_;
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
                    case TypeKind::Shared: case TypeKind::Weak: case TypeKind::CancellationSource: case TypeKind::CancellationToken: case TypeKind::TaskScope: return "i64";
                    case TypeKind::Task: return "{ i64, i64 }";
                    case TypeKind::Atomic: return llvmType(type.element);
                    case TypeKind::Mutex: case TypeKind::RwLock: return "{ i64, " + llvmType(type.element) + ", i8 }";
                    case TypeKind::MutexGuard: case TypeKind::RwLockReadGuard: case TypeKind::RwLockWriteGuard: return "{ ptr, ptr, i1 }";
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
                // 원시 문자열은 이스케이프 처리 없이 검증된 본문 바이트를 보존한다.
                // Preserve validated raw string bytes without escape processing.
                if (const auto raw = rawStringContents(spelling)) bytes.assign(*raw);
                else for (std::size_t index = 1; index + 1 < spelling.size(); ++index)
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
                if (node.kind == "Literal" && isStringLiteralSpelling(node.value)) internText(node.value);
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
                std::size_t projectionOrdinal = 0;
                for (const auto& projection : place.projections)
                {
                    const auto ordinal = projectionOrdinal++;
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
                        // 동일 AIR 명령의 실제 고정 배열 장소에 대한 수렴한 증명만 소비한다.
                        // Consume converged proof only for this exact instruction's actual fixed-array place.
                        const auto proven = currentInstruction_ ? proofRanges_.fixedArrayIndices.find(currentInstruction_)
                            : proofRanges_.fixedArrayIndices.end();
                        const bool safeIndex = proofOptions_.enabled && currentInstruction_ && &place == &currentInstruction_->place
                            && collection.kind == TypeKind::Array && collection.extent == projection.extent
                            && proven != proofRanges_.fixedArrayIndices.end() && proven->second.contains(ordinal);
                        if (safeIndex)
                        {
                            if (proofOptions_.statistics) ++proofOptions_.statistics->boundsChecksRemoved;
                        }
                        else
                        {
                            output_ << "  " << invalid << " = icmp uge " << compareType << ' ' << index << ", " << bound << '\n';
                            if (integer.isSigned)
                            {
                                const auto negative = temporary(); const auto outside = temporary();
                                output_ << "  " << negative << " = icmp slt " << numericType << ' ' << value(projection.index) << ", 0\n"
                                        << "  " << outside << " = or i1 " << invalid << ", " << negative << '\n';
                                guard(outside);
                            }
                            else guard(invalid);
                        }
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
                if (item.opcode == AIROpcode::NumericConvert && (item.operation == "Float.to_bits" || item.operation == "Float.from_bits"))
                {
                    // 16비트 저장은 이미 i16이며 큰 형식도 산술 없이 비트를 그대로 옮긴다.
                    // Sixteen-bit storage already uses i16; wider formats transfer bits without arithmetic.
                    const auto from = llvmType(source), to = llvmType(item.type);
                    output_ << "  " << value(item.result) << " = " << (from == to ? "freeze " : "bitcast ") << from << ' ' << operand;
                    if (from != to) output_ << " to " << to;
                    output_ << '\n'; return;
                }
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
                if (proofOptions_.enabled && proofRanges_.arithmeticNoOverflow.contains(&item))
                {
                    // 수학적 결과가 타입 범위 안임을 증명해도 poison 약속 플래그를 추가하지 않는다.
                    // Even with a proven mathematical result, emit no poison-producing no-wrap promises.
                    output_ << "  " << value(item.result) << " = " << operation << ' ' << type << ' ' << left << ", " << right << '\n';
                    if (proofOptions_.statistics) ++proofOptions_.statistics->arithmeticChecksRemoved;
                    return;
                }
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
            void emitRangeInspect(const AIRInstruction& item)
            {
                const auto* operation = rangeInspectOperation(item.operation);
                if (!operation) { error(item.source,"Unknown closed Range inspection."); return; }
                const auto rangeId = typed_.types.get(valueType(item.operands[0])).element;
                const auto& element = typed_.types.get(typed_.types.get(rangeId).element);
                const auto type = llvmType(rangeId), aggregate = temporary();
                output_ << "  " << aggregate << " = load " << type << ", ptr " << value(item.operands[0]) << '\n';
                if (operation->result != RangeInspectResult::Empty)
                {
                    output_ << "  " << value(item.result) << " = extractvalue " << type << ' ' << aggregate << ", " << operation->field << '\n';
                    return;
                }
                const auto start = temporary(), end = temporary(), closed = temporary(), descending = temporary();
                output_ << "  " << start << " = extractvalue " << type << ' ' << aggregate << ", 0\n"
                        << "  " << end << " = extractvalue " << type << ' ' << aggregate << ", 1\n"
                        << "  " << closed << " = extractvalue " << type << ' ' << aggregate << ", 3\n"
                        << "  " << descending << " = extractvalue " << type << ' ' << aggregate << ", 4\n";
                const auto less = temporary(), greater = temporary(), equal = temporary(), strict = temporary(), singleton = temporary(), present = temporary();
                const auto integer = llvmType(typed_.types.get(rangeId).element);
                output_ << "  " << less << " = icmp " << (element.isSigned ? "slt" : "ult") << ' ' << integer << ' ' << start << ", " << end << '\n'
                        << "  " << greater << " = icmp " << (element.isSigned ? "sgt" : "ugt") << ' ' << integer << ' ' << start << ", " << end << '\n'
                        << "  " << equal << " = icmp eq " << integer << ' ' << start << ", " << end << '\n'
                        << "  " << strict << " = select i1 " << descending << ", i1 " << greater << ", i1 " << less << '\n'
                        << "  " << singleton << " = and i1 " << closed << ", " << equal << '\n'
                        << "  " << present << " = or i1 " << strict << ", " << singleton << '\n'
                        << "  " << value(item.result) << " = xor i1 " << present << ", true\n";
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
            void emitCharScalar(const AIRInstruction& item)
            {
                if (item.operation == "Char.to_u32")
                { output_ << "  " << value(item.result) << " = freeze i32 " << value(item.operands[0]) << '\n'; return; }
                const auto carrier = typed_.carriers.find(item.type);
                if (carrier == typed_.carriers.end()) { error(item.source,"Char scalar Result carrier is missing."); return; }
                const auto* rejected = namedVariant(carrier->second.error,"InvalidScalar");
                if (!rejected || rejected->fieldTypes.size() != 1)
                { error(item.source,"Char scalar error payload is missing."); return; }
                const auto raw = value(item.operands[0]);
                const auto bounded = temporary(), lower = temporary(), upper = temporary(), outside = temporary(), valid = temporary();
                const auto yes = label(), no = label(), done = label();
                const auto storage = "%enumtmp" + std::to_string(item.result), errors = "%charerror" + std::to_string(item.result);
                // 모든 검사는 u32에서 이루어져 잘못된 입력을 자르거나 char로 먼저 만들지 않는다.
                // Check the full u32 before constructing char; rejected input is never truncated or exposed as a scalar.
                output_ << "  " << bounded << " = icmp ule i32 " << raw << ", 1114111\n"
                        << "  " << lower << " = icmp ult i32 " << raw << ", 55296\n"
                        << "  " << upper << " = icmp ugt i32 " << raw << ", 57343\n"
                        << "  " << outside << " = or i1 " << lower << ", " << upper << '\n'
                        << "  " << valid << " = and i1 " << bounded << ", " << outside << '\n'
                        << "  br i1 " << valid << ", label %" << yes << ", label %" << no << '\n' << yes << ":\n";
                enumStore(storage,typed_.enumVariants.at(carrier->second.success),{{carrier->second.value,raw}});
                output_ << "  br label %" << done << '\n' << no << ":\n";
                enumStore(errors,*rejected,{{typed_.types.builtin("u32"),raw}});
                const auto errorValue = temporary();
                output_ << "  " << errorValue << " = load " << llvmType(carrier->second.error) << ", ptr " << errors << '\n';
                enumStore(storage,typed_.enumVariants.at(carrier->second.failure),{{carrier->second.error,errorValue}});
                output_ << "  br label %" << done << '\n' << done << ":\n"
                        << "  " << value(item.result) << " = load " << llvmType(item.type) << ", ptr " << storage << '\n';
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
            const EnumVariantInfo* namedVariant(TypeId type, std::string_view name)
            {
                for (const auto& record : module_.records) if (record.type == type)
                    for (const auto& variant : record.variants)
                        if (variant.variant < names_.symbols.size() && names_.symbols[variant.variant].name == name) return &variant;
                error(nullptr, "missing system error variant " + std::string(name) + "."); return nullptr;
            }
            void systemIoError(TypeId type, const std::string& storage, const std::string& status, bool input, bool flush)
            {
                const auto done = label();
                const auto save = [&](std::string_view name)
                {
                    const auto* variant = namedVariant(type, name);
                    if (!variant) return;
                    std::vector<std::pair<TypeId, std::string>> values;
                    if (name == "ReadFailed" || name == "WriteFailed" || name == "FlushFailed")
                    {
                        const auto code = temporary(); output_ << "  " << code << " = call i32 @aevix_system_error_code()\n";
                        values.emplace_back(typed_.types.builtin("i32"), code);
                        if (!flush)
                        { const auto progress = temporary(); output_ << "  " << progress << " = call i64 @aevix_system_error_progress()\n"; values.emplace_back(typed_.types.builtin("usize"), progress); }
                    }
                    enumStore(storage, *variant, values); output_ << "  br label %" << done << '\n';
                };
                const std::vector<std::pair<unsigned, std::string>> cases = input
                    ? std::vector<std::pair<unsigned, std::string>>{{2,"AllocationFailure"},{3,"SizeOverflow"},{4,"ReadFailed"},{5,"InvalidUtf8"}}
                    : std::vector<std::pair<unsigned, std::string>>{{1,flush ? "FlushFailed" : "WriteFailed"},{2,"AllocationFailure"},{3,"SizeOverflow"}};
                for (const auto& [code, name] : cases)
                {
                    const auto matches = temporary(), selected = label(), next = label();
                    output_ << "  " << matches << " = icmp eq i32 " << status << ", " << code << '\n'
                            << "  br i1 " << matches << ", label %" << selected << ", label %" << next << '\n' << selected << ":\n";
                    save(name); output_ << next << ":\n";
                }
                save("Unsupported"); output_ << done << ":\n";
            }
            void emitSystem(const AIRInstruction& item)
            {
                usesSystem_ = true;
                const auto& carrier = typed_.carriers.at(item.type);
                const auto suffix = std::to_string(item.result);
                const auto storage = "%enumtmp" + suffix, errorStorage = "%syserror" + suffix;
                const auto status = temporary();
                if (item.operation == "input")
                {
                    usesAllocation_ = true;
                    const auto& optional = typed_.carriers.at(carrier.value);
                    const auto& target = typed_.types.get(optional.value);
                    const bool text = target.kind == TypeKind::String;
                    const auto dataSlot = "%sysdata" + suffix, lengthSlot = "%syslen" + suffix, capacitySlot = "%syscap" + suffix;
                    const auto optionalSlot = "%sysoptional" + suffix, ioSlot = "%sysio" + suffix;
                    const auto* ioVariant = namedVariant(carrier.error, "Io");
                    const auto* parseVariant = namedVariant(carrier.error, "Parse");
                    if (!ioVariant || !parseVariant || ioVariant->fieldTypes.size() != 1 || parseVariant->fieldTypes.size() != 1) return;
                    output_ << "  " << status << " = call i32 @aevix_system_input(i8 " << (text ? 0 : 1) << ", ptr " << dataSlot
                            << ", ptr " << lengthSlot << ", ptr " << capacitySlot << ")\n";
                    const auto ok = temporary(), yes = label(), maybeEof = label(), eof = label(), failed = label(), done = label();
                    output_ << "  " << ok << " = icmp eq i32 " << status << ", 0\n  br i1 " << ok << ", label %" << yes << ", label %" << maybeEof << '\n'
                            << maybeEof << ":\n";
                    const auto ended = temporary();
                    output_ << "  " << ended << " = icmp eq i32 " << status << ", 1\n  br i1 " << ended << ", label %" << eof << ", label %" << failed << '\n' << eof << ":\n";
                    enumStore(optionalSlot, typed_.enumVariants.at(optional.failure), {});
                    const auto empty = temporary(); output_ << "  " << empty << " = load " << llvmType(carrier.value) << ", ptr " << optionalSlot << '\n';
                    enumStore(storage, typed_.enumVariants.at(carrier.success), {{carrier.value, empty}}); output_ << "  br label %" << done << '\n';
                    output_ << failed << ":\n";
                    systemIoError(ioVariant->fieldTypes[0], ioSlot, status, true, false);
                    const auto ioValue = temporary(); output_ << "  " << ioValue << " = load " << llvmType(ioVariant->fieldTypes[0]) << ", ptr " << ioSlot << '\n';
                    enumStore(errorStorage, *ioVariant, {{ioVariant->fieldTypes[0], ioValue}});
                    const auto reason = temporary(); output_ << "  " << reason << " = load " << llvmType(carrier.error) << ", ptr " << errorStorage << '\n';
                    enumStore(storage, typed_.enumVariants.at(carrier.failure), {{carrier.error, reason}}); output_ << "  br label %" << done << '\n' << yes << ":\n";
                    const auto data = temporary(), length = temporary(), capacity = temporary();
                    output_ << "  " << data << " = load ptr, ptr " << dataSlot << '\n'
                            << "  " << length << " = load i64, ptr " << lengthSlot << '\n'
                            << "  " << capacity << " = load i64, ptr " << capacitySlot << '\n';
                    std::string payload;
                    if (text)
                    {
                        const auto a = temporary(), b = temporary(); payload = temporary();
                        output_ << "  " << a << " = insertvalue { ptr, i64, i64 } zeroinitializer, ptr " << data << ", 0\n"
                                << "  " << b << " = insertvalue { ptr, i64, i64 } " << a << ", i64 " << length << ", 1\n"
                                << "  " << payload << " = insertvalue { ptr, i64, i64 } " << b << ", i64 " << capacity << ", 2\n";
                    }
                    else
                    {
                        usesParse_ = true;
                        const auto domain = target.kind == TypeKind::Integer ? 0 : target.kind == TypeKind::Float ? 1 : target.kind == TypeKind::Bool ? 2 : target.kind == TypeKind::Char ? 3 : 4;
                        const auto bits = target.kind == TypeKind::Bool ? 1u : target.kind == TypeKind::Char ? 32u : target.bits;
                        const auto parsed = temporary(), valid = temporary(), good = label(), bad = label();
                        output_ << "  " << parsed << " = call i32 @aevix_parse_scalar(ptr " << data << ", i64 " << length << ", i32 " << domain
                                << ", i32 " << bits << ", i32 " << (target.kind == TypeKind::Integer && target.isSigned ? 1 : 0) << ", ptr %syspayload" << suffix << ")\n"
                                << "  call void @free(ptr " << data << ")\n  " << valid << " = icmp eq i32 " << parsed << ", 0\n"
                                << "  br i1 " << valid << ", label %" << good << ", label %" << bad << '\n' << bad << ":\n";
                        const auto tag = temporary(), parseError = temporary();
                        const auto parseSlot = "%sysparseerror" + suffix;
                        output_ << "  store " << llvmType(parseVariant->fieldTypes[0]) << " zeroinitializer, ptr " << parseSlot << '\n'
                                << "  " << tag << " = sub i32 " << parsed << ", 1\n  store i32 " << tag << ", ptr " << parseSlot << '\n'
                                << "  " << parseError << " = load " << llvmType(parseVariant->fieldTypes[0]) << ", ptr " << parseSlot << '\n';
                        enumStore(errorStorage, *parseVariant, {{parseVariant->fieldTypes[0], parseError}});
                        const auto readError = temporary(); output_ << "  " << readError << " = load " << llvmType(carrier.error) << ", ptr " << errorStorage << '\n';
                        enumStore(storage, typed_.enumVariants.at(carrier.failure), {{carrier.error, readError}}); output_ << "  br label %" << done << '\n' << good << ":\n";
                        payload = temporary(); output_ << "  " << payload << " = load " << llvmType(optional.value) << ", ptr %syspayload" << suffix << '\n';
                    }
                    enumStore(optionalSlot, typed_.enumVariants.at(optional.success), {{optional.value, payload}});
                    const auto some = temporary(); output_ << "  " << some << " = load " << llvmType(carrier.value) << ", ptr " << optionalSlot << '\n';
                    enumStore(storage, typed_.enumVariants.at(carrier.success), {{carrier.value, some}});
                    output_ << "  br label %" << done << '\n' << done << ":\n  " << value(item.result) << " = load " << llvmType(item.type) << ", ptr " << storage << '\n';
                    return;
                }
                if (item.operation == "flush") output_ << "  " << status << " = call i32 @aevix_system_flush(i8 0)\n";
                else
                {
                    const bool log = item.operation == "log";
                    const auto begin = log ? 1u : 0u;
                    const auto end = item.operands.size() - (log ? 1u : 0u);
                    const auto count = end - begin;
                    const std::string partType = "{ ptr, i64, i32, i32 }";
                    for (std::size_t index = begin; index < end; ++index)
                    {
                        const auto input = item.operands[index]; const auto& type = typed_.types.get(valueType(input));
                        const bool text = type.kind == TypeKind::StringView || type.kind == TypeKind::Reference;
                        std::string pointer, length = "0";
                        unsigned kind = 6, bits = 0;
                        if (text) std::tie(pointer, length) = textDescriptor(input);
                        else
                        {
                            pointer = "%syspart" + suffix + "." + std::to_string(index);
                            kind = type.kind == TypeKind::Integer ? type.isSigned ? 0 : 1 : type.kind == TypeKind::Float ? 2 : type.kind == TypeKind::Bool ? 3 : type.kind == TypeKind::Char ? 4 : 5;
                            bits = type.kind == TypeKind::Bool ? 1u : type.kind == TypeKind::Char ? 32u : type.bits;
                            auto scalar = value(input);
                            if (type.kind == TypeKind::Bool)
                            { const auto extended = temporary(); output_ << "  " << extended << " = zext i1 " << scalar << " to i8\n"; scalar = extended; }
                            output_ << "  store " << (type.kind == TypeKind::Bool ? "i8" : llvmType(valueType(input))) << ' ' << scalar << ", ptr " << pointer << '\n';
                        }
                        const auto slot = temporary(); output_ << "  " << slot << " = getelementptr [" << count << " x " << partType << "], ptr %sysparts" << suffix << ", i32 0, i64 " << index - begin << '\n';
                        const auto a = temporary(), b = temporary(), c = temporary(), d = temporary();
                        output_ << "  " << a << " = insertvalue " << partType << " zeroinitializer, ptr " << pointer << ", 0\n"
                                << "  " << b << " = insertvalue " << partType << ' ' << a << ", i64 " << length << ", 1\n"
                                << "  " << c << " = insertvalue " << partType << ' ' << b << ", i32 " << kind << ", 2\n"
                                << "  " << d << " = insertvalue " << partType << ' ' << c << ", i32 " << bits << ", 3\n"
                                << "  store " << partType << ' ' << d << ", ptr " << slot << '\n';
                    }
                    std::string level = "0", code = "null", codeLength = "0";
                    if (log)
                    {
                        const auto tag = temporary(); level = temporary();
                        output_ << "  " << tag << " = extractvalue " << llvmType(valueType(item.operands[0])) << ' ' << value(item.operands[0]) << ", 0\n"
                                << "  " << level << " = trunc i32 " << tag << " to i8\n";
                        std::tie(code, codeLength) = textDescriptor(item.operands.back());
                    }
                    output_ << "  " << status << " = call i32 @aevix_system_output(ptr " << (count ? "%sysparts" + suffix : "null")
                            << ", i64 " << count << ", i8 " << (log ? 2 : item.operation == "println" ? 1 : 0) << ", i8 " << level << ", ptr " << code << ", i64 " << codeLength << ")\n";
                }
                const auto ok = temporary(), yes = label(), no = label(), done = label();
                output_ << "  " << ok << " = icmp eq i32 " << status << ", 0\n  br i1 " << ok << ", label %" << yes << ", label %" << no << '\n' << yes << ":\n";
                enumStore(storage, typed_.enumVariants.at(carrier.success), {{carrier.value, "zeroinitializer"}});
                output_ << "  br label %" << done << '\n' << no << ":\n";
                systemIoError(carrier.error, errorStorage, status, false, item.operation == "flush");
                const auto reason = temporary(); output_ << "  " << reason << " = load " << llvmType(carrier.error) << ", ptr " << errorStorage << '\n';
                enumStore(storage, typed_.enumVariants.at(carrier.failure), {{carrier.error, reason}});
                output_ << "  br label %" << done << '\n' << done << ":\n  " << value(item.result) << " = load " << llvmType(item.type) << ", ptr " << storage << '\n';
            }
            void emitOs(const AIRInstruction& item)
            {
                usesOs_ = true;
                const auto* operation = osOperation(item.operation);
                if (!operation) { error(item.source, "unknown closed OS operation."); return; }
                if (operation->unitResult)
                {
                    output_ << "  call void @aevix_os_" << item.operation << "(i64 " << value(item.operands[0]) << ")\n"
                            << "  " << value(item.result) << " = freeze {} zeroinitializer\n";
                    return;
                }
                const auto storage = "%osout" + std::to_string(item.result);
                auto status = temporary();
                const auto payloadType = typed_.types.get(item.type).parameters[1];
                // 모든 경로에서 출력 저장소를 초기화하므로 실패 결과에도 이전 호출의 토큰이 섞이지 않는다.
                // Initialize output on every path so failures cannot expose a previous invocation's token.
                output_ << "  store " << llvmType(payloadType) << " zeroinitializer, ptr " << storage << '\n';
                if (item.operation == "file_open")
                {
                    const auto [data, length] = textDescriptor(item.operands[0]);
                    output_ << "  " << status << " = call i32 @aevix_os_file_open_utf8(ptr " << data
                            << ", i64 " << length << ", i32 " << value(item.operands[1]) << ", ptr " << storage << ")\n";
                }
                else if (item.operation == "file_read" || item.operation == "file_write")
                {
                    const auto [data, length] = textDescriptor(item.operands[1]);
                    output_ << "  " << status << " = call i32 @aevix_os_" << item.operation << "(i64 "
                            << value(item.operands[0]) << ", ptr " << data << ", i64 " << length << ", ptr " << storage << ")\n";
                }
                else if (item.operation == "file_seek")
                    output_ << "  " << status << " = call i32 @aevix_os_file_seek(i64 " << value(item.operands[0])
                            << ", i64 " << value(item.operands[1]) << ", i32 " << value(item.operands[2]) << ", ptr " << storage << ")\n";
                else if (item.operation == "file_size")
                    output_ << "  " << status << " = call i32 @aevix_os_file_size(i64 " << value(item.operands[0]) << ", ptr " << storage << ")\n";
                else if (item.operation == "file_set_len")
                    output_ << "  " << status << " = call i32 @aevix_os_file_set_len(i64 " << value(item.operands[0])
                            << ", i64 " << value(item.operands[1]) << ")\n";
                else if (item.operation == "file_flush" || item.operation == "file_close")
                    output_ << "  " << status << " = call i32 @aevix_os_" << item.operation << "(i64 " << value(item.operands[0]) << ")\n";
                else if (item.operation == "monotonic_ns" || item.operation == "unix_time_ns")
                    output_ << "  " << status << " = call i32 @aevix_os_" << item.operation << "(ptr " << storage << ")\n";
                else if (item.operation == "sleep_ns")
                    output_ << "  " << status << " = call i32 @aevix_os_sleep_ns(i64 " << value(item.operands[0]) << ")\n";
                else if (item.operation == "process_spawn")
                {
                    const auto [data, count] = textDescriptor(item.operands[0]);
                    output_ << "  " << status << " = call i32 @aevix_os_process_spawn(ptr " << data << ", i64 " << count << ", ptr " << storage << ")\n";
                }
                else if (item.operation == "process_wait" || item.operation == "process_try_wait")
                    output_ << "  " << status << " = call i32 @aevix_os_" << item.operation << "(i64 " << value(item.operands[0]) << ", ptr " << storage << ")\n";
                else if (item.operation == "process_kill")
                    output_ << "  " << status << " = call i32 @aevix_os_process_kill(i64 " << value(item.operands[0]) << ")\n";
                else if (item.operation == "env_set")
                {
                    const auto [name, nameLength] = textDescriptor(item.operands[0]);
                    const auto [text, textLength] = textDescriptor(item.operands[1]);
                    output_ << "  " << status << " = call i32 @aevix_os_env_set_utf8(ptr " << name << ", i64 " << nameLength
                            << ", ptr " << text << ", i64 " << textLength << ")\n";
                }
                else
                {
                    const auto [data, length] = textDescriptor(item.operands[0]);
                    if (item.operation == "env_get" || item.operation == "path_normalize")
                    {
                        const auto dataSlot = temporary(), lengthSlot = temporary(), capacitySlot = temporary();
                        output_ << "  " << dataSlot << " = getelementptr { ptr, i64, i64 }, ptr " << storage << ", i32 0, i32 0\n"
                                << "  " << lengthSlot << " = getelementptr { ptr, i64, i64 }, ptr " << storage << ", i32 0, i32 1\n"
                                << "  " << capacitySlot << " = getelementptr { ptr, i64, i64 }, ptr " << storage << ", i32 0, i32 2\n";
                        output_ << "  " << status << " = call i32 @aevix_os_" << item.operation << "_utf8(ptr " << data << ", i64 " << length
                                << ", ptr " << dataSlot << ", ptr " << lengthSlot;
                        if (item.operation == "env_get") output_ << ", ptr %ospresent" << item.result;
                        output_ << ")\n";
                        const auto count = temporary(); output_ << "  " << count << " = load i64, ptr " << lengthSlot << '\n'
                            << "  store i64 " << count << ", ptr " << capacitySlot << '\n';
                        if (item.operation == "env_get")
                        {
                            const auto present = temporary(), missing = temporary(), succeeded = temporary(), absent = temporary(), adapted = temporary();
                            output_ << "  " << present << " = load i32, ptr %ospresent" << item.result << '\n'
                                    << "  " << missing << " = icmp eq i32 " << present << ", 0\n"
                                    << "  " << succeeded << " = icmp eq i32 " << status << ", 0\n"
                                    << "  " << absent << " = and i1 " << missing << ", " << succeeded << '\n'
                                    << "  " << adapted << " = select i1 " << absent << ", i32 8, i32 " << status << '\n';
                            status = adapted;
                        }
                        usesAllocation_ = true;
                    }
                    else if (item.operation == "path_metadata")
                        output_ << "  " << status << " = call i32 @aevix_os_path_metadata_utf8(ptr " << data << ", i64 " << length
                                << ", i32 " << value(item.operands[1]) << ", ptr " << storage << ")\n";
                    else
                        output_ << "  " << status << " = call i32 @aevix_os_" << item.operation << "_utf8(ptr " << data << ", i64 " << length << ")\n";
                }
                const auto native = temporary(), kind = temporary(), payload = temporary();
                output_ << "  " << native << " = call i32 @aevix_os_last_error()\n"
                        << "  " << kind << " = call i32 @aevix_os_error_kind()\n"
                        << "  " << payload << " = load " << llvmType(payloadType) << ", ptr " << storage << '\n';
                const auto first = temporary(), second = temporary(), third = temporary();
                const auto tuple = llvmType(item.type);
                output_ << "  " << first << " = insertvalue " << tuple << " zeroinitializer, i32 " << status << ", 0\n"
                        << "  " << second << " = insertvalue " << tuple << ' ' << first << ", " << llvmType(payloadType) << ' ' << payload << ", 1\n"
                        << "  " << third << " = insertvalue " << tuple << ' ' << second << ", i32 " << native << ", 2\n"
                        << "  " << value(item.result) << " = insertvalue " << tuple << ' ' << third << ", i32 " << kind << ", 3\n";
            }
            void emitParse(const AIRInstruction& item)
            {
                usesParse_ = true;
                const auto& carrier = typed_.carriers.at(item.type);
                const auto& scalar = typed_.types.get(carrier.value);
                const auto domain = scalar.kind == TypeKind::Integer ? 0 : scalar.kind == TypeKind::Float ? 1
                    : scalar.kind == TypeKind::Bool ? 2 : scalar.kind == TypeKind::Char ? 3 : 4;
                const auto bits = scalar.kind == TypeKind::Bool ? 1u : scalar.kind == TypeKind::Char ? 32u : scalar.bits;
                const auto [data, length] = textDescriptor(item.operands[0]);
                const auto payload = "%parsepayload" + std::to_string(item.result);
                const auto errorStorage = "%parseerror" + std::to_string(item.result);
                const auto storage = "%enumtmp" + std::to_string(item.result);
                const auto status = temporary(), ok = temporary(), yes = label(), no = label(), done = label();
                output_ << "  " << status << " = call i32 @aevix_parse_scalar(ptr " << data << ", i64 " << length
                        << ", i32 " << domain << ", i32 " << bits << ", i32 " << (scalar.kind == TypeKind::Integer && scalar.isSigned ? 1 : 0)
                        << ", ptr " << payload << ")\n  " << ok << " = icmp eq i32 " << status << ", 0\n"
                        << "  br i1 " << ok << ", label %" << yes << ", label %" << no << '\n' << yes << ":\n";
                const auto result = temporary();
                output_ << "  " << result << " = load " << llvmType(carrier.value) << ", ptr " << payload << '\n';
                enumStore(storage, typed_.enumVariants.at(carrier.success), {{carrier.value, result}});
                output_ << "  br label %" << done << '\n' << no << ":\n";
                const auto tag = temporary(), reason = temporary();
                output_ << "  store " << llvmType(carrier.error) << " zeroinitializer, ptr " << errorStorage << '\n'
                        << "  " << tag << " = sub i32 " << status << ", 1\n  store i32 " << tag << ", ptr " << errorStorage << '\n'
                        << "  " << reason << " = load " << llvmType(carrier.error) << ", ptr " << errorStorage << '\n';
                enumStore(storage, typed_.enumVariants.at(carrier.failure), {{carrier.error, reason}});
                output_ << "  br label %" << done << '\n' << done << ":\n  " << value(item.result) << " = load " << llvmType(item.type) << ", ptr " << storage << '\n';
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
            void emitSync(const AIRInstruction& item)
            {
                const auto* operation = syncOperation(item.operation);
                if (!operation) { error(item.source, "Unknown closed Sync operation."); return; }
                const auto input = value(item.operands[0]);
                const auto payloadType = llvmType(item.syncElement);
                const auto base = "%sync." + std::to_string(item.result);
                const auto method = item.operation.substr(item.operation.find('.') + 1);
                const auto inputType = valueType(item.operands[0]);
                const auto owner = operation->access == SyncAccess::None ? item.type : operation->access == SyncAccess::Value ? inputType : typed_.types.get(inputType).element;
                const auto ownerType = llvmType(owner);
                if (operation->result == SyncResult::Recovered)
                { output_ << "  " << value(item.result) << " = extractvalue " << ownerType << ' ' << input << ", 0\n"; return; }
                if (operation->result == SyncResult::Owner)
                {
                    std::string policy = "0";
                    if (item.operands.size() == 2)
                    {
                        const auto raw = temporary(), invalid = temporary(); policy = temporary();
                        output_ << "  " << raw << " = extractvalue " << llvmType(valueType(item.operands[1])) << ' ' << value(item.operands[1]) << ", 0\n"
                                << "  " << invalid << " = icmp ugt i32 " << raw << ", 1\n";
                        guard(invalid); output_ << "  " << policy << " = trunc i32 " << raw << " to i8\n";
                    }
                    const auto partial = temporary();
                    output_ << "  " << partial << " = insertvalue " << llvmType(item.type) << " zeroinitializer, " << payloadType << ' ' << input << ", 1\n"
                            << "  " << value(item.result) << " = insertvalue " << llvmType(item.type) << ' ' << partial << ", i8 " << policy << ", 2\n";
                    return;
                }
                if (operation->result == SyncResult::Reference || operation->result == SyncResult::MutableReference)
                {
                    const auto descriptor = temporary();
                    output_ << "  " << descriptor << " = load " << ownerType << ", ptr " << input << '\n'
                            << "  " << value(item.result) << " = extractvalue " << ownerType << ' ' << descriptor << ", 1\n";
                    return;
                }
                if (operation->result == SyncResult::Policy)
                {
                    const auto pointer = temporary(), stored = temporary(), expanded = temporary();
                    output_ << "  " << pointer << " = getelementptr " << ownerType << ", ptr " << input << ", i32 0, i32 2\n"
                            << "  " << stored << " = load i8, ptr " << pointer << '\n'
                            << "  " << expanded << " = zext i8 " << stored << " to i32\n"
                            << "  " << value(item.result) << " = insertvalue " << llvmType(item.type) << " zeroinitializer, i32 " << expanded << ", 0\n";
                    return;
                }
                usesSync_ = true;
                if (operation->result == SyncResult::Unit)
                {
                    auto descriptor = input;
                    if (operation->access != SyncAccess::Value)
                    { descriptor = temporary(); output_ << "  " << descriptor << " = load " << ownerType << ", ptr " << input << '\n'; }
                    const auto state = temporary(); output_ << "  " << state << " = extractvalue " << ownerType << ' ' << descriptor << ", 0\n";
                    if (method == "mark_poisoned")
                    {
                        const auto pointer = temporary();
                        output_ << "  " << pointer << " = getelementptr " << ownerType << ", ptr " << input << ", i32 0, i32 2\n  store i1 true, ptr " << pointer << '\n';
                    }
                    else
                    {
                        const auto status = temporary(); std::string extra;
                        if (operation->access == SyncAccess::Value && operation->kind != SyncKind::ReadGuard)
                        {
                            const auto pending = temporary(), wide = temporary();
                            output_ << "  " << pending << " = extractvalue " << ownerType << ' ' << descriptor << ", 2\n"
                                    << "  " << wide << " = zext i1 " << pending << " to i32\n";
                            extra = ", i32 " + wide;
                        }
                        output_ << "  " << status << " = call i32 @aevix_" << operation->native << "(ptr " << state << extra << ")\n";
                        const auto failed = temporary(); output_ << "  " << failed << " = icmp ne i32 " << status << ", 0\n"; guard(failed);
                        if (method == "clear_poison")
                        {
                            const auto pointer = temporary();
                            output_ << "  " << pointer << " = getelementptr " << ownerType << ", ptr " << input << ", i32 0, i32 2\n  store i1 false, ptr " << pointer << '\n';
                        }
                    }
                    output_ << "  " << value(item.result) << " = freeze {} zeroinitializer\n"; return;
                }
                auto state = input;
                if (operation->result == SyncResult::IntoInner)
                { state = base + ".owner"; output_ << "  store " << ownerType << ' ' << input << ", ptr " << state << '\n'; }
                const auto status = temporary();
                std::string timeout;
                if (operation->timed)
                { timeout = temporary(); output_ << "  " << timeout << " = extractvalue " << llvmType(valueType(item.operands[1])) << ' ' << value(item.operands[1]) << ", 0\n"; }
                output_ << "  " << status << " = call i32 @aevix_" << operation->native << "(ptr " << state
                        << (operation->timed ? ", i64 " + timeout : "") << ", ptr " << base << ".poison)\n";
                const auto poison = temporary(), poisoned = temporary();
                output_ << "  " << poison << " = load i32, ptr " << base << ".poison\n"
                        << "  " << poisoned << " = icmp ne i32 " << poison << ", 0\n";
                if (operation->result == SyncResult::Boolean)
                {
                    const auto failed = temporary(); output_ << "  " << failed << " = icmp ne i32 " << status << ", 0\n"; guard(failed);
                    output_ << "  " << value(item.result) << " = freeze i1 " << poisoned << '\n'; return;
                }
                const auto carrierEntry = typed_.carriers.find(item.type);
                if (carrierEntry == typed_.carriers.end()) { error(item.source, "Sync operation requires a core Result carrier."); return; }
                const auto& carrier = carrierEntry->second;
                const auto storage = "%enumtmp" + std::to_string(item.result);
                const bool acquisition = operation->result == SyncResult::Acquire || operation->result == SyncResult::OptionalAcquire;
                if (operation->result != SyncResult::IntoInner && (!namedVariant(carrier.error, "Poisoned")
                    || (acquisition && (!namedVariant(carrier.error, "ReaderOverflow") || !namedVariant(carrier.error, "InvalidTimeout")))))
                { error(item.source, "Sync operation requires the exact LockError capability carrier."); return; }
                const auto& output = typed_.types.get(carrier.value);
                (void)output;
                const auto successful = label(), failure = label(), finished = label();
                const auto ok = temporary();
                output_ << "  " << ok << " = icmp eq i32 " << status << ", 0\n  br i1 " << ok << ", label %" << successful << ", label %" << failure << '\n' << successful << ":\n";
                std::string held;
                TypeId heldType = carrier.value;
                if (operation->result == SyncResult::OptionalAcquire) heldType = typed_.carriers.at(carrier.value).value;
                if (operation->result == SyncResult::IntoInner)
                { held = temporary(); output_ << "  " << held << " = extractvalue " << ownerType << ' ' << input << ", 1\n"; }
                else
                {
                    const auto payload = temporary();
                    output_ << "  " << payload << " = getelementptr " << ownerType << ", ptr " << state << ", i32 0, i32 1\n";
                    held = payload;
                    if (acquisition)
                    {
                        const auto first = temporary(); held = temporary();
                        output_ << "  " << first << " = insertvalue " << llvmType(heldType) << " zeroinitializer, ptr " << state << ", 0\n"
                                << "  " << held << " = insertvalue " << llvmType(heldType) << ' ' << first << ", ptr " << payload << ", 1\n";
                    }
                }
                const auto clean = label(), dirty = label();
                output_ << "  br i1 " << poisoned << ", label %" << dirty << ", label %" << clean << '\n' << clean << ":\n";
                auto successValue = held;
                if (operation->result == SyncResult::OptionalAcquire)
                {
                    const auto& optional = typed_.carriers.at(carrier.value);
                    enumStore(base + ".optional", typed_.enumVariants.at(optional.success), {{heldType,held}});
                    successValue = temporary(); output_ << "  " << successValue << " = load " << llvmType(carrier.value) << ", ptr " << base << ".optional\n";
                }
                enumStore(storage, typed_.enumVariants.at(carrier.success), {{carrier.value,successValue}});
                output_ << "  br label %" << finished << '\n' << dirty << ":\n";
                if (operation->result == SyncResult::IntoInner)
                {
                    const auto errorValue = temporary();
                    output_ << "  " << errorValue << " = insertvalue " << llvmType(carrier.error) << " zeroinitializer, " << payloadType << ' ' << held << ", 0\n";
                    enumStore(storage, typed_.enumVariants.at(carrier.failure), {{carrier.error,errorValue}});
                }
                else
                {
                    enumStore(base + ".error", *namedVariant(carrier.error,"Poisoned"), {{heldType,held}});
                    const auto errorValue = temporary(); output_ << "  " << errorValue << " = load " << llvmType(carrier.error) << ", ptr " << base << ".error\n";
                    enumStore(storage, typed_.enumVariants.at(carrier.failure), {{carrier.error,errorValue}});
                }
                output_ << "  br label %" << finished << '\n' << failure << ":\n";
                if (!acquisition)
                { guard("true"); output_ << "  br label %" << finished << '\n'; }
                else
                {
                    const auto empty = label(), saturated = label(), invalid = label(), unexpected = label();
                    output_ << "  switch i32 " << status << ", label %" << unexpected << " [ i32 1, label %" << empty << " i32 2, label %" << empty
                            << " i32 5, label %" << saturated << " i32 3, label %" << invalid << " ]\n" << empty << ":\n";
                    if (operation->result == SyncResult::OptionalAcquire)
                    {
                        const auto& optional = typed_.carriers.at(carrier.value);
                        enumStore(base + ".optional", typed_.enumVariants.at(optional.failure), {});
                        const auto none = temporary(); output_ << "  " << none << " = load " << llvmType(carrier.value) << ", ptr " << base << ".optional\n";
                        enumStore(storage, typed_.enumVariants.at(carrier.success), {{carrier.value,none}});
                    }
                    else guard("true");
                    output_ << "  br label %" << finished << '\n' << saturated << ":\n";
                    if (operation->guard != SyncKind::ReadGuard) guard("true");
                    enumStore(base + ".error", *namedVariant(carrier.error,"ReaderOverflow"), {});
                    const auto exhausted = temporary(); output_ << "  " << exhausted << " = load " << llvmType(carrier.error) << ", ptr " << base << ".error\n";
                    enumStore(storage, typed_.enumVariants.at(carrier.failure), {{carrier.error,exhausted}});
                    output_ << "  br label %" << finished << '\n' << invalid << ":\n";
                    if (operation->timed)
                    { const auto finite = temporary(); output_ << "  " << finite << " = icmp ule i64 " << timeout << ", 9223372036854775807\n"; guard(finite); }
                    else guard("true");
                    enumStore(base + ".error", *namedVariant(carrier.error,"InvalidTimeout"), {});
                    const auto durationError = temporary(); output_ << "  " << durationError << " = load " << llvmType(carrier.error) << ", ptr " << base << ".error\n";
                    enumStore(storage, typed_.enumVariants.at(carrier.failure), {{carrier.error,durationError}});
                    output_ << "  br label %" << finished << '\n' << unexpected << ":\n";
                    guard("true"); output_ << "  br label %" << finished << '\n';
                }
                output_ << finished << ":\n  " << value(item.result) << " = load " << llvmType(item.type) << ", ptr " << storage << '\n';
            }
            void emitTaskReason(TypeId type, const std::string& storage, const std::string& status,
                                const std::string& code, const std::string& domain)
            {
                const auto native = label(), cancelled = label(), failed = label(), deadlock = label(), overflow = label(), timedout = label(), invalid = label(), done = label();
                output_ << "  switch i32 " << status << ", label %" << invalid << " [ i32 6, label %" << native << " i32 9, label %" << cancelled
                        << " i32 10, label %" << failed << " i32 7, label %" << deadlock << " i32 2, label %" << overflow << " i32 11, label %" << timedout << " ]\n";
                for (const auto& entry : {std::pair{native,"Native"},std::pair{cancelled,"Cancelled"},std::pair{failed,"Failed"},std::pair{deadlock,"WouldDeadlock"},std::pair{overflow,"Overflow"},std::pair{timedout,"TimedOut"}})
                {
                    output_ << entry.first << ":\n";
                    const auto* variant = namedVariant(type,entry.second);
                    if (!variant) return;
                    std::vector<std::pair<TypeId,std::string>> fields;
                    if (entry.second == std::string_view("Native")) fields = {{typed_.types.builtin("i32"),code},{typed_.types.builtin("i32"),domain}};
                    enumStore(storage,*variant,fields); output_ << "  br label %" << done << '\n';
                }
                output_ << invalid << ":\n"; guard("true"); output_ << "  br label %" << done << '\n' << done << ":\n";
            }
            void emitTask(const AIRInstruction& item)
            {
                const auto* operation = taskOperation(item.operation);
                if (!operation) { error(item.source,"Unknown closed Task operation."); return; }
                usesTask_ = true;
                const auto base = "%task." + std::to_string(item.result);
                const auto resultStorage = "%enumtmp" + std::to_string(item.result);
                const auto statusCall = [&](const std::string& call)
                { const auto status = temporary(); output_ << "  " << status << " = " << call << '\n'; return status; };
                const auto requireGood = [&](const std::string& status)
                { const auto bad = temporary(); output_ << "  " << bad << " = icmp ne i32 " << status << ", 0\n"; guard(bad); };
                const auto platformCode = [&]() { return statusCall("call i32 @aevix_task_last_error()"); };
                const auto platformDomain = [&]() { return statusCall("call i32 @aevix_task_error_domain()"); };
                // 진단 식별자는 관측 핸들이 소비되어도 남아 있으며 런타임 토큰을 다시 조회하지 않는다.
                // Diagnostic identity survives consumed observation and never queries a runtime token.
                if (operation->result == TaskResult::Identity)
                {
                    const auto descriptor = temporary(), serial = temporary(), invalid = temporary();
                    const auto owner = typed_.types.get(valueType(item.operands[0])).element;
                    output_ << "  " << descriptor << " = load " << llvmType(owner) << ", ptr " << value(item.operands[0]) << '\n'
                            << "  " << serial << " = extractvalue " << llvmType(owner) << ' ' << descriptor << ", 1\n"
                            << "  " << invalid << " = icmp eq i64 " << serial << ", 0\n";
                    guard(invalid);
                    output_ << "  " << value(item.result) << " = insertvalue " << llvmType(item.type) << " zeroinitializer, i64 " << serial << ", 0\n";
                    return;
                }
                std::string token, observation;
                if (operation->access != TaskAccess::None)
                {
                    token = value(item.operands[0]);
                    if (operation->access == TaskAccess::TaskValue)
                    {
                        const auto extracted = temporary();
                        output_ << "  " << extracted << " = extractvalue " << llvmType(valueType(item.operands[0])) << ' ' << token << ", 0\n";
                        token = extracted;
                    }
                    else if (operation->access != TaskAccess::ScopeValue)
                    {
                        if (operation->access == TaskAccess::TaskShared || operation->access == TaskAccess::TaskExclusive)
                        {
                            observation = temporary();
                            const auto owner = typed_.types.get(valueType(item.operands[0])).element;
                            output_ << "  " << observation << " = getelementptr " << llvmType(owner) << ", ptr " << token << ", i32 0, i32 0\n";
                            token = observation;
                        }
                        const auto loaded = temporary(); output_ << "  " << loaded << " = load i64, ptr " << token << '\n'; token = loaded;
                    }
                }
                if (operation->result == TaskResult::ActiveCount || operation->result == TaskResult::ScopeState)
                {
                    const auto count = operation->result == TaskResult::ActiveCount;
                    requireGood(statusCall("call i32 @aevix_task_scope_" + std::string(count ? "active_count" : "status")
                        + "(i64 " + token + ", ptr " + base + (count ? ".count)" : ".flag)")));
                    if (count)
                        output_ << "  " << value(item.result) << " = load i64, ptr " << base << ".count\n";
                    else
                    {
                        const auto state = temporary(), invalid = temporary();
                        output_ << "  " << state << " = load i32, ptr " << base << ".flag\n  " << invalid << " = icmp ugt i32 " << state << ", 1\n"; guard(invalid);
                        const auto* open = namedVariant(item.type,"Open"); if (!open) return;
                        enumStore(base + ".status",*open,{});
                        output_ << "  store i32 " << state << ", ptr " << base << ".status\n  " << value(item.result)
                                << " = load " << llvmType(item.type) << ", ptr " << base << ".status\n";
                    }
                    return;
                }
                if (operation->result == TaskResult::Executor)
                {
                    // 백엔드 사실은 결과를 가져간 뒤에도 동일하며 네이티브 식별자를 노출하지 않는다.
                    // The backend fact survives result consumption and exposes no native identity.
                    const auto* backend = namedVariant(item.type,"NativeThreads"); if (!backend) return;
                    enumStore(base + ".status",*backend,{});
                    output_ << "  " << value(item.result) << " = load " << llvmType(item.type) << ", ptr " << base << ".status\n";
                    return;
                }
                if (operation->result == TaskResult::Status || operation->result == TaskResult::Boolean)
                {
                    const auto* statusRecord = operation->result == TaskResult::Status ? record(item.type) : nullptr;
                    const auto phaseType = statusRecord ? statusRecord->fieldTypes[0] : InvalidTypeId;
                    const auto empty = temporary(), yes = label(), no = label(), done = label();
                    output_ << "  " << empty << " = icmp eq i64 " << token << ", 0\n  br i1 " << empty << ", label %" << yes << ", label %" << no << '\n';
                    const auto storeStatus = [&](const std::string& phase, const std::string& requested)
                    {
                        if (operation->result == TaskResult::Boolean)
                        {
                            if (item.operation == "is_cancel_requested")
                            {
                                output_ << "  store i1 " << requested << ", ptr " << base << ".status\n  br label %" << done << '\n';
                                return;
                            }
                            const auto terminal = temporary();
                            output_ << "  " << terminal << " = icmp uge i32 " << phase << ", 3\n  store i1 " << terminal << ", ptr " << base << ".status\n  br label %" << done << '\n';
                            return;
                        }
                        const auto p = temporary(), a = temporary(), b = temporary();
                        output_ << "  " << p << " = insertvalue " << llvmType(phaseType) << " zeroinitializer, i32 " << phase << ", 0\n"
                                << "  " << a << " = insertvalue " << llvmType(item.type) << " zeroinitializer, " << llvmType(phaseType) << ' ' << p << ", 0\n"
                                << "  " << b << " = insertvalue " << llvmType(item.type) << ' ' << a << ", i1 " << requested << ", 1\n"
                                << "  store " << llvmType(item.type) << ' ' << b << ", ptr " << base << ".status\n  br label %" << done << '\n';
                    };
                    output_ << yes << ":\n"; storeStatus("7","false"); output_ << no << ":\n";
                    requireGood(statusCall("call i32 @aevix_scoped_task_snapshot(i64 " + token + ", ptr " + base + ".snapshot)"));
                    const auto phase = temporary(), requestPointer = temporary(), requested = temporary(), bit = temporary(), invalid = temporary(), invalidFlag = temporary();
                    output_ << "  " << phase << " = load i32, ptr " << base << ".snapshot\n  " << invalid << " = icmp ugt i32 " << phase << ", 6\n"; guard(invalid);
                    output_ << "  " << requestPointer << " = getelementptr i32, ptr " << base << ".snapshot, i32 1\n  " << requested << " = load i32, ptr " << requestPointer << '\n'
                            << "  " << invalidFlag << " = icmp ugt i32 " << requested << ", 1\n"; guard(invalidFlag);
                    output_ << "  " << bit << " = icmp ne i32 " << requested << ", 0\n";
                    storeStatus(phase,bit);
                    output_ << done << ":\n  " << value(item.result) << " = load " << llvmType(item.type) << ", ptr " << base << ".status\n"; return;
                }
                const auto& carrier = typed_.carriers.at(item.type);
                const auto done = label();
                const auto ok = [&](TypeId type, const std::string& value)
                { enumStore(resultStorage,typed_.enumVariants.at(carrier.success),{{type,value}}); output_ << "  br label %" << done << '\n'; };
                const auto failure = [&]()
                { const auto loaded = temporary(); output_ << "  " << loaded << " = load " << llvmType(carrier.error) << ", ptr " << base << ".error\n"; enumStore(resultStorage,typed_.enumVariants.at(carrier.failure),{{carrier.error,loaded}}); output_ << "  br label %" << done << '\n'; };
                if (operation->result == TaskResult::Token)
                {
                    usesCancellation_ = true;
                    const auto empty = temporary(), consumed = label(), execute = label();
                    output_ << "  " << empty << " = icmp eq i64 " << token << ", 0\n  br i1 " << empty << ", label %" << consumed << ", label %" << execute << '\n'
                            << consumed << ":\n";
                    const auto* already = namedVariant(carrier.error,"AlreadyJoined"); if (!already) return;
                    enumStore(base + ".error",*already,{}); failure(); output_ << execute << ":\n";
                    const auto status = statusCall("call i32 @aevix_scoped_task_cancel_token(i64 " + token + ", ptr " + base + ".token)");
                    const auto good = label(), allocation = label(), overflow = label(), invalid = label();
                    output_ << "  switch i32 " << status << ", label %" << invalid << " [ i32 0, label %" << good
                            << " i32 1, label %" << allocation << " i32 2, label %" << overflow << " ]\n" << good << ":\n";
                    const auto owner = temporary(), absent = temporary();
                    output_ << "  " << owner << " = load i64, ptr " << base << ".token\n  " << absent << " = icmp eq i64 " << owner << ", 0\n";
                    guard(absent); ok(carrier.value,owner);
                    for (const auto& entry : {std::pair{allocation,"AllocationFailure"},std::pair{overflow,"Overflow"}})
                    {
                        output_ << entry.first << ":\n";
                        const auto* reason = namedVariant(carrier.error,entry.second); if (!reason) return;
                        enumStore(base + ".error",*reason,{}); failure();
                    }
                    output_ << invalid << ":\n"; guard("true"); output_ << "  br label %" << done << '\n';
                }
                else if (operation->result == TaskResult::CheckCancel)
                {
                    usesCancellation_ = true;
                    requireGood(statusCall("call i32 @aevix_cancel_token_requested(i64 " + token + ", ptr " + base + ".flag)"));
                    const auto flag = temporary(), invalid = temporary(), requested = temporary();
                    const auto clean = label(), cancelled = label();
                    output_ << "  " << flag << " = load i32, ptr " << base << ".flag\n  " << invalid << " = icmp ugt i32 " << flag << ", 1\n"; guard(invalid);
                    output_ << "  " << requested << " = icmp eq i32 " << flag << ", 1\n  br i1 " << requested << ", label %" << cancelled << ", label %" << clean << '\n'
                            << clean << ":\n";
                    ok(carrier.value,"zeroinitializer");
                    output_ << cancelled << ":\n";
                    const auto* checkpoint = namedVariant(carrier.error,"Requested"); if (!checkpoint) return;
                    enumStore(base + ".error",*checkpoint,{}); failure();
                }
                else if (operation->result == TaskResult::Scope || operation->result == TaskResult::Spawn)
                {
                    std::string status;
                    if (operation->worker)
                    {
                        taskThunks_.insert({item.taskCallable,item.taskElement,item.taskInvoke});
                        const auto capture = layout(item.taskCallable), result = layout(item.taskElement);
                        const auto identity = std::to_string(item.taskCallable) + "." + std::to_string(item.taskElement) + "." + std::to_string(item.taskInvoke);
                        output_ << "  store " << llvmType(item.taskCallable) << ' ' << value(item.operands[1]) << ", ptr " << base << ".capture\n";
                        status = statusCall("call i32 @aevix_task_scope_prepare_identified(i64 " + token + ", i64 " + std::to_string(capture.size) + ", i64 " + std::to_string(capture.alignment)
                            + ", i64 " + std::to_string(result.size) + ", i64 " + std::to_string(result.alignment) + ", ptr @aevix.task.invoke." + identity
                             + ", ptr @aevix.task.capture.drop." + identity + ", ptr @aevix.task.result.drop." + identity + ", ptr " + base + ".token, ptr " + base + ".pointer, ptr " + base + ".identity)");
                    }
                    else if (operation->preparation == TaskPreparation::Ready)
                    {
                        taskReadyDrops_.insert(item.taskElement);
                        const auto result = layout(item.taskElement);
                        output_ << "  store " << llvmType(item.taskElement) << ' ' << value(item.operands[1]) << ", ptr " << base << ".ready\n";
                        status = statusCall("call i32 @aevix_task_scope_prepare_ready(i64 " + token + ", i64 " + std::to_string(result.size)
                            + ", i64 " + std::to_string(result.alignment) + ", ptr @aevix.task.ready.drop." + std::to_string(item.taskElement)
                            + ", ptr " + base + ".token, ptr " + base + ".pointer, ptr " + base + ".identity)");
                    }
                    else status = statusCall("call i32 @aevix_task_scope_open(ptr " + base + ".token)");
                    const auto code = platformCode(), domain = platformDomain();
                    const auto good = label(), bad = label(); const auto valid = temporary();
                    output_ << "  " << valid << " = icmp eq i32 " << status << ", 0\n  br i1 " << valid << ", label %" << good << ", label %" << bad << '\n' << good << ":\n";
                    const auto owner = temporary(); output_ << "  " << owner << " = load i64, ptr " << base << ".token\n";
                    if (operation->worker)
                    {
                        const auto pointer = temporary(); output_ << "  " << pointer << " = load ptr, ptr " << base << ".pointer\n  store " << llvmType(item.taskCallable) << ' ' << value(item.operands[1]) << ", ptr " << pointer << '\n';
                        requireGood(statusCall("call i32 @aevix_scoped_task_commit(i64 " + owner + ")"));
                    }
                    if (operation->preparation == TaskPreparation::Ready)
                    {
                        const auto pointer = temporary();
                        output_ << "  " << pointer << " = load ptr, ptr " << base << ".pointer\n  store " << llvmType(item.taskElement) << ' '
                                << value(item.operands[1]) << ", ptr " << pointer << '\n';
                        requireGood(statusCall("call i32 @aevix_scoped_task_ready_commit(i64 " + owner + ")"));
                    }
                    if (operation->worker || operation->preparation == TaskPreparation::Ready)
                    {
                        const auto serial = temporary(), partial = temporary(), descriptor = temporary(), invalid = temporary();
                        output_ << "  " << serial << " = load i64, ptr " << base << ".identity\n"
                                << "  " << invalid << " = icmp eq i64 " << serial << ", 0\n";
                        guard(invalid);
                        output_ << "  " << partial << " = insertvalue " << llvmType(carrier.value) << " zeroinitializer, i64 " << owner << ", 0\n"
                                << "  " << descriptor << " = insertvalue " << llvmType(carrier.value) << ' ' << partial << ", i64 " << serial << ", 1\n";
                        ok(carrier.value,descriptor);
                    }
                    else ok(carrier.value,owner);
                    output_ << bad << ":\n";
                    if (operation->worker) emitDropCall(item.taskCallable,base + ".capture");
                    else if (operation->preparation == TaskPreparation::Ready) emitDropCall(item.taskElement,base + ".ready");
                    const auto allocation = label(), overflow = label(), closed = label(), thread = label(), invalid = label();
                    output_ << "  switch i32 " << status << ", label %" << invalid << " [ i32 1, label %" << allocation << " i32 2, label %" << overflow << " i32 5, label %" << closed << " i32 6, label %" << thread << " ]\n";
                    for (const auto& entry : {std::pair{allocation,"AllocationFailure"},std::pair{overflow,"Overflow"},std::pair{closed,"ScopeClosed"},std::pair{thread,"ThreadFailure"}})
                    {
                        output_ << entry.first << ":\n";
                        const auto* variant = namedVariant(carrier.error,entry.second); if (!variant) return;
                        std::vector<std::pair<TypeId,std::string>> fields;
                        if (entry.second == std::string_view("ThreadFailure")) fields = {{typed_.types.builtin("i32"),code},{typed_.types.builtin("i32"),domain}};
                        enumStore(base + ".error",*variant,fields); failure();
                    }
                    output_ << invalid << ":\n"; guard("true"); output_ << "  br label %" << done << '\n';
                }
                else if (operation->result == TaskResult::CloseAdmission)
                {
                    const auto* borrowed = namedVariant(carrier.error,"Failure");
                    if (!borrowed || borrowed->fieldTypes.size() != 1) { error(item.source,"Non-owning ScopeError.Failure reason is missing."); return; }
                    const auto reason = borrowed->fieldTypes[0];
                    const auto status = statusCall("call i32 @aevix_task_scope_close_admission(i64 " + token + ")");
                    const auto code = platformCode(), domain = platformDomain(), valid = temporary();
                    const auto good = label(), bad = label();
                    output_ << "  " << valid << " = icmp eq i32 " << status << ", 0\n  br i1 " << valid << ", label %" << good << ", label %" << bad << '\n' << good << ":\n";
                    ok(carrier.value,"zeroinitializer");
                    output_ << bad << ":\n";
                    emitTaskReason(reason,base + ".reason",status,code,domain);
                    const auto error = temporary(); output_ << "  " << error << " = load " << llvmType(reason) << ", ptr " << base << ".reason\n";
                    enumStore(base + ".error",*borrowed,{{reason,error}}); failure();
                }
                else if (operation->result == TaskResult::Close)
                {
                    const auto* retry = namedVariant(carrier.error,"Retry"); if (!retry) return;
                    const auto reason = retry->fieldTypes[1];
                    const auto status = statusCall("call i32 @aevix_task_scope_close(i64 " + token + ", ptr " + base + ".report)");
                    const auto code = platformCode(), domain = platformDomain(), valid = temporary(); const auto good = label(), bad = label();
                    output_ << "  " << valid << " = icmp eq i32 " << status << ", 0\n  br i1 " << valid << ", label %" << good << ", label %" << bad << '\n' << good << ":\n";
                    const auto report = temporary(); output_ << "  " << report << " = load " << llvmType(carrier.value) << ", ptr " << base << ".report\n"; ok(carrier.value,report);
                    output_ << bad << ":\n"; emitTaskReason(reason,base + ".reason",status,code,domain);
                    const auto error = temporary(); output_ << "  " << error << " = load " << llvmType(reason) << ", ptr " << base << ".reason\n";
                    enumStore(base + ".error",*retry,{{valueType(item.operands[0]),token},{reason,error}}); failure();
                }
                else
                {
                    const auto empty = temporary(), already = label(), execute = label();
                    output_ << "  " << empty << " = icmp eq i64 " << token << ", 0\n  br i1 " << empty << ", label %" << already << ", label %" << execute << '\n' << already << ":\n";
                    const auto* consumed = namedVariant(carrier.error,"AlreadyJoined"); if (!consumed) return;
                    enumStore(base + ".error",*consumed,{}); failure(); output_ << execute << ":\n";
                    std::string status;
                    if (operation->result == TaskResult::Wait)
                    {
                        const auto timeout = temporary(), tooLarge = temporary(), invalid = label(), finite = label();
                        output_ << "  " << timeout << " = extractvalue " << llvmType(valueType(item.operands[1])) << ' ' << value(item.operands[1]) << ", 0\n"
                                << "  " << tooLarge << " = icmp ugt i64 " << timeout << ", 9223372036854775807\n  br i1 " << tooLarge << ", label %" << invalid << ", label %" << finite << '\n' << invalid << ":\n";
                        enumStore(base + ".error",*namedVariant(carrier.error,"Overflow"),{}); failure(); output_ << finite << ":\n";
                        status = statusCall("call i32 @aevix_scoped_task_wait_timeout(i64 " + token + ", i64 " + timeout + ")");
                    }
                    else if (operation->result == TaskResult::Completion) status = statusCall("call i32 @aevix_scoped_task_wait(i64 " + token + ")");
                    else if (operation->result == TaskResult::Request) status = statusCall("call i32 @aevix_scoped_task_request_cancel(i64 " + token + ", ptr " + base + ".flag)");
                    else if (operation->result == TaskResult::Discard) status = statusCall("call i32 @aevix_scoped_task_discard(i64 " + token + ")");
                    else
                    {
                        if (item.operation == "cancel_and_join")
                            requireGood(statusCall("call i32 @aevix_scoped_task_request_cancel(i64 " + token + ", ptr " + base + ".flag)"));
                        status = statusCall("call i32 @aevix_scoped_task_" + std::string(operation->result == TaskResult::TryTake ? "try_take" : "join") + "(i64 " + token + ", ptr " + base + ".result, i64 " + std::to_string(layout(item.taskElement).size) + ")");
                    }
                    const auto code = platformCode(), domain = platformDomain();
                    const auto good = label(), bad = label(), notReady = label();
                    output_ << "  switch i32 " << status << ", label %" << bad << " [ i32 0, label %" << good;
                    if (operation->result == TaskResult::TryTake) output_ << " i32 8, label %" << notReady;
                    else if (operation->result == TaskResult::Wait) output_ << " i32 11, label %" << notReady;
                    output_ << " ]\n" << good << ":\n";
                    if (operation->result == TaskResult::Join || operation->result == TaskResult::TryTake)
                    {
                        const auto value = temporary(); output_ << "  " << value << " = load " << llvmType(item.taskElement) << ", ptr " << base << ".result\n";
                        if (operation->result == TaskResult::TryTake)
                        {
                            output_ << "  store i64 0, ptr " << observation << '\n';
                            const auto& optional = typed_.carriers.at(carrier.value);
                            enumStore(base + ".optional",typed_.enumVariants.at(optional.success),{{item.taskElement,value}});
                            const auto some = temporary(); output_ << "  " << some << " = load " << llvmType(carrier.value) << ", ptr " << base << ".optional\n"; ok(carrier.value,some);
                        }
                        else ok(carrier.value,value);
                    }
                    else if (operation->result == TaskResult::Discard || operation->result == TaskResult::Completion) ok(carrier.value,"zeroinitializer");
                    else
                    {
                        if (operation->result == TaskResult::Request)
                        {
                            const auto disposition = temporary(), invalid = temporary(), terminal = temporary(), repeated = temporary(), tag = temporary();
                            output_ << "  " << disposition << " = load i32, ptr " << base << ".flag\n  " << invalid << " = icmp ugt i32 " << disposition << ", 2\n"; guard(invalid);
                            output_ << "  " << terminal << " = icmp eq i32 " << disposition << ", 1\n  " << repeated << " = icmp eq i32 " << disposition << ", 2\n";
                            const auto normal = temporary(); output_ << "  " << normal << " = select i1 " << repeated << ", i32 1, i32 0\n  " << tag << " = select i1 " << terminal << ", i32 2, i32 " << normal << '\n';
                            enumStore(base + ".success",*namedVariant(carrier.value,"NewlyRequested"),{}); output_ << "  store i32 " << tag << ", ptr " << base << ".success\n";
                        }
                        else enumStore(base + ".success",*namedVariant(carrier.value,"Ready"),{});
                        const auto ready = temporary(); output_ << "  " << ready << " = load " << llvmType(carrier.value) << ", ptr " << base << ".success\n"; ok(carrier.value,ready);
                    }
                    if (operation->result == TaskResult::TryTake || operation->result == TaskResult::Wait)
                    {
                        output_ << notReady << ":\n";
                        if (operation->result == TaskResult::TryTake)
                        { const auto& optional = typed_.carriers.at(carrier.value); enumStore(base + ".optional",typed_.enumVariants.at(optional.failure),{}); }
                        else enumStore(base + ".success",*namedVariant(carrier.value,"TimedOut"),{});
                        const auto result = temporary(); output_ << "  " << result << " = load " << llvmType(carrier.value) << ", ptr " << base << (operation->result == TaskResult::TryTake ? ".optional" : ".success") << '\n'; ok(carrier.value,result);
                    }
                    output_ << bad << ":\n";
                    if (operation->result == TaskResult::Discard) { guard("true"); output_ << "  br label %" << done << '\n'; }
                    else
                    {
                        const auto cancelled = temporary(), failed = temporary(), consumed = temporary();
                        output_ << "  " << cancelled << " = icmp eq i32 " << status << ", 9\n  " << failed << " = icmp eq i32 " << status << ", 10\n  " << consumed << " = or i1 " << cancelled << ", " << failed << '\n';
                        if (operation->result == TaskResult::Join)
                        {
                            const auto fallback = label(), reason = label(); output_ << "  br i1 " << consumed << ", label %" << reason << ", label %" << fallback << '\n' << fallback << ":\n";
                            requireGood(statusCall("call i32 @aevix_scoped_task_discard(i64 " + token + ")")); output_ << "  br label %" << reason << '\n' << reason << ":\n";
                        }
                        else if (operation->result == TaskResult::TryTake)
                        {
                            const auto clear = label(), reason = label(); output_ << "  br i1 " << consumed << ", label %" << clear << ", label %" << reason << '\n' << clear << ":\n  store i64 0, ptr " << observation << "\n  br label %" << reason << '\n' << reason << ":\n";
                        }
                        emitTaskReason(carrier.error,base + ".error",status,code,domain); failure();
                    }
                }
                output_ << done << ":\n  " << value(item.result) << " = load " << llvmType(item.type) << ", ptr " << resultStorage << '\n';
            }
            void emitTaskThunks()
            {
                for (const auto& [capture,result,invoke] : taskThunks_)
                {
                    const auto identity = std::to_string(capture) + "." + std::to_string(result) + "." + std::to_string(invoke);
                    const auto* target = function(invoke);
                    if (!target || target->parameters.size() != 1) { error(nullptr,"Task thunk has no exact callable implementation."); continue; }
                    const auto self = target->locals[target->parameters[0]].type; const bool borrowed = typed_.types.get(self).kind == TypeKind::Reference;
                    temporary_ = 0; label_ = 0;
                    output_ << "define internal i32 @aevix.task.invoke." << identity << "(ptr %capture, ptr %result, ptr %context) {\nentry:\n";
                    if (!borrowed) output_ << "  %owner = load " << llvmType(capture) << ", ptr %capture\n";
                    output_ << "  %value = call " << llvmType(result) << " @aevix.fn." << invoke << '(' << (borrowed ? "ptr %capture" : llvmType(capture) + " %owner") << ")\n";
                    if (borrowed) emitDropCall(capture,"%capture");
                    output_ << "  store " << llvmType(result) << " %value, ptr %result\n  ret i32 0\n}\n";
                    output_ << "define internal void @aevix.task.capture.drop." << identity << "(ptr %capture) {\nentry:\n";
                    temporary_ = 0; label_ = 0; emitDropCall(capture,"%capture"); output_ << "  ret void\n}\n";
                    output_ << "define internal void @aevix.task.result.drop." << identity << "(ptr %result) {\nentry:\n";
                    temporary_ = 0; label_ = 0; emitDropCall(result,"%result"); output_ << "  ret void\n}\n";
                }
                for (const auto result : taskReadyDrops_)
                {
                    output_ << "define internal void @aevix.task.ready.drop." << result << "(ptr %result) {\nentry:\n";
                    temporary_ = 0; label_ = 0; emitDropCall(result,"%result"); output_ << "  ret void\n}\n";
                }
            }
            void emitCancellation(const AIRInstruction& item)
            {
                const auto* operation = cancellationOperation(item.operation);
                if (!operation) { error(item.source, "Unknown closed cancellation operation."); return; }
                usesCancellation_ = true;
                const auto base = "%cancel." + std::to_string(item.result);
                std::string token;
                if (operation->operands)
                {
                    token = temporary();
                    output_ << "  " << token << " = load i64, ptr " << value(item.operands[0]) << '\n';
                }
                const bool owner = operation->result == CancellationResult::Source || operation->result == CancellationResult::Token;
                const auto status = temporary();
                output_ << "  " << status << " = call i32 @" << operation->native << '(';
                if (operation->operands) output_ << "i64 " << token << ", ";
                output_ << "ptr " << base << (owner ? ".token" : ".flag") << ")\n";
                if (!operation->fallible)
                {
                    const auto bad = temporary(), observed = temporary(), invalidFlag = temporary();
                    output_ << "  " << bad << " = icmp ne i32 " << status << ", 0\n"; guard(bad);
                    output_ << "  " << observed << " = load i32, ptr " << base << ".flag\n"
                            << "  " << invalidFlag << " = icmp ugt i32 " << observed << ", 1\n"; guard(invalidFlag);
                    output_ << "  " << value(item.result) << " = icmp ne i32 " << observed << ", 0\n";
                    return;
                }
                const auto& carrier = typed_.carriers.at(item.type);
                const auto* allocationVariant = namedVariant(carrier.error,"AllocationFailure");
                const auto* overflowVariant = namedVariant(carrier.error,"Overflow");
                const auto* requestVariant = owner ? nullptr : namedVariant(carrier.value,"NewlyRequested");
                if (!allocationVariant || !overflowVariant || (!owner && !requestVariant)) return;
                const auto storage = "%enumtmp" + std::to_string(item.result);
                const auto good = label(), allocation = label(), overflow = label(), invalid = label(), done = label();
                output_ << "  switch i32 " << status << ", label %" << invalid << " [ i32 0, label %" << good
                        << " i32 1, label %" << allocation << " i32 2, label %" << overflow << " ]\n" << good << ":\n";
                const auto taken = temporary();
                if (owner)
                    output_ << "  " << taken << " = load i64, ptr " << base << ".token\n";
                else
                {
                    const auto flag = temporary(), bad = temporary();
                    output_ << "  " << flag << " = load i32, ptr " << base << ".flag\n"
                            << "  " << bad << " = icmp ugt i32 " << flag << ", 1\n"; guard(bad);
                    enumStore(base + ".request", *requestVariant, {});
                    output_ << "  store i32 " << flag << ", ptr " << base << ".request\n"
                            << "  " << taken << " = load " << llvmType(carrier.value) << ", ptr " << base << ".request\n";
                }
                enumStore(storage, typed_.enumVariants.at(carrier.success), {{carrier.value,taken}});
                output_ << "  br label %" << done << '\n';
                for (const auto& failure : {std::pair{allocation,"AllocationFailure"},std::pair{overflow,"Overflow"}})
                {
                    output_ << failure.first << ":\n";
                    enumStore(base + ".error", failure.second == std::string_view("AllocationFailure") ? *allocationVariant : *overflowVariant, {});
                    const auto errorValue = temporary();
                    output_ << "  " << errorValue << " = load " << llvmType(carrier.error) << ", ptr " << base << ".error\n";
                    enumStore(storage, typed_.enumVariants.at(carrier.failure), {{carrier.error,errorValue}});
                    output_ << "  br label %" << done << '\n';
                }
                output_ << invalid << ":\n"; guard("true"); output_ << "  br label %" << done << '\n'
                    << done << ":\n  " << value(item.result) << " = load " << llvmType(item.type) << ", ptr " << storage << '\n';
            }
            void emitAtomic(const AIRInstruction& item)
            {
                const auto* operation = atomicOperation(item.operation);
                if (!operation) { error(item.source, "Unknown closed Atomic operation."); return; }
                const auto& scalar = typed_.types.get(item.atomicElement);
                const auto kind = scalar.kind == TypeKind::Bool ? 8u : scalar.kind == TypeKind::Integer && scalar.bits <= 64
                    ? (scalar.bits == 8 ? 0u : scalar.bits == 16 ? 2u : scalar.bits == 32 ? 4u : 6u) + (scalar.isSigned ? 0u : 1u) : 999u;
                const auto input = [&](std::size_t index) { return value(item.operands[index]); };
                const auto scalarType = llvmType(item.atomicElement);
                if (item.operation == "new" || item.operation == "into_inner")
                { output_ << "  " << value(item.result) << " = freeze " << scalarType << ' ' << input(0) << '\n'; return; }
                if (item.operation == "get_mut")
                { output_ << "  " << value(item.result) << " = getelementptr i8, ptr " << input(0) << ", i64 0\n"; return; }
                if (item.operation == "is_supported")
                { output_ << "  " << value(item.result) << " = freeze i1 " << (kind < 9 ? "true" : "false") << '\n'; return; }
                usesAtomic_ = true;
                if (item.operation == "is_lock_free" || item.operation == "required_alignment")
                {
                    const auto observed = temporary();
                    output_ << "  " << observed << " = call i32 @aevix_atomic_" << (item.operation == "is_lock_free" ? "lock_free" : "alignment") << "(i32 " << kind << ")\n";
                    if (item.operation == "is_lock_free") output_ << "  " << value(item.result) << " = icmp ne i32 " << observed << ", 0\n";
                    else output_ << "  " << value(item.result) << " = zext i32 " << observed << " to i64\n";
                    return;
                }
                const auto base = "%atomic." + std::to_string(item.result);
                const auto encode = [&](std::size_t index)
                {
                    if (scalar.kind == TypeKind::Integer && scalar.bits == 64) return input(index);
                    const auto result = temporary(); output_ << "  " << result << " = zext " << scalarType << ' ' << input(index) << " to i64\n"; return result;
                };
                const auto order = [&](int index)
                {
                    if (index < 0) return std::string("0");
                    const auto result = temporary();
                    output_ << "  " << result << " = extractvalue " << llvmType(valueType(item.operands[index])) << ' ' << input(index) << ", 0\n"; return result;
                };
                const auto successOrder = order(operation->order), failureOrder = order(operation->failure);
                const auto status = temporary();
                if (item.operation == "fence" || item.operation == "compiler_fence")
                    output_ << "  " << status << " = call i32 @aevix_atomic_fence(i32 " << successOrder << ", i32 " << (item.operation == "compiler_fence" ? 1 : 0) << ")\n";
                else if (item.operation == "wait" || item.operation == "wait_timeout")
                {
                    const auto expected = encode(1); std::string timeout = "0";
                    if (item.operation == "wait_timeout")
                    { timeout = temporary(); output_ << "  " << timeout << " = extractvalue " << llvmType(valueType(item.operands[2])) << ' ' << input(2) << ", 0\n"; }
                    output_ << "  " << status << " = call i32 @aevix_atomic_wait(ptr " << input(0) << ", i32 " << kind << ", i64 " << expected
                            << ", i32 " << successOrder << ", i64 " << timeout << ", i32 " << (item.operation == "wait_timeout" ? 1 : 0)
                            << ", ptr " << base << ".observed, ptr " << base << ".changed)\n";
                }
                else
                {
                    const auto operand = operation->value >= 0 ? encode(operation->value) : std::string("0");
                    const auto expected = operation->result == AtomicResult::Cas ? encode(1) : std::string("0");
                    output_ << "  " << status << " = call i32 @aevix_atomic_operation(ptr " << input(0) << ", i32 " << kind << ", i32 " << operation->native
                            << ", i64 " << operand << ", i64 " << expected << ", i32 " << successOrder << ", i32 " << failureOrder
                            << ", ptr " << base << ".observed, ptr " << base << ".changed)\n";
                }
                if (!operation->fallible)
                {
                    const auto bad = temporary(); output_ << "  " << bad << " = icmp ne i32 " << status << ", 0\n"; guard(bad);
                    output_ << "  " << value(item.result) << " = freeze {} zeroinitializer\n"; return;
                }
                const auto& carrier = typed_.carriers.at(item.type);
                const auto storage = "%enumtmp" + std::to_string(item.result);
                const auto successful = label(), failure = label(), done = label(), ok = temporary();
                output_ << "  " << ok << " = icmp eq i32 " << status << ", 0\n  br i1 " << ok << ", label %" << successful << ", label %" << failure << '\n' << successful << ":\n";
                std::string payload = "zeroinitializer";
                if (operation->result != AtomicResult::Unit)
                {
                    const auto observed = temporary(); output_ << "  " << observed << " = load i64, ptr " << base << ".observed\n";
                    std::string decoded = observed;
                    if (!(scalar.kind == TypeKind::Integer && scalar.bits == 64))
                    { decoded = temporary(); output_ << "  " << decoded << " = trunc i64 " << observed << " to " << scalarType << '\n'; }
                    payload = decoded;
                    if (operation->result == AtomicResult::Cas || operation->result == AtomicResult::Wait)
                    {
                        const auto raw = temporary(), changed = temporary();
                        output_ << "  " << raw << " = load i32, ptr " << base << ".changed\n  " << changed << " = icmp ne i32 " << raw << ", 0\n";
                        const auto first = temporary(), second = temporary(), aggregate = llvmType(carrier.value);
                        if (operation->result == AtomicResult::Cas)
                            output_ << "  " << first << " = insertvalue " << aggregate << " zeroinitializer, i1 " << changed << ", 0\n"
                                    << "  " << second << " = insertvalue " << aggregate << ' ' << first << ", " << scalarType << ' ' << decoded << ", 1\n";
                        else
                        {
                            const auto timedOut = temporary(), middle = temporary();
                            output_ << "  " << first << " = insertvalue " << aggregate << " zeroinitializer, " << scalarType << ' ' << decoded << ", 0\n"
                                    << "  " << middle << " = insertvalue " << aggregate << ' ' << first << ", i1 " << changed << ", 1\n"
                                    << "  " << timedOut << " = xor i1 " << changed << ", true\n"
                                    << "  " << second << " = insertvalue " << aggregate << ' ' << middle << ", i1 " << timedOut << ", 2\n";
                        }
                        payload = second;
                    }
                }
                enumStore(storage, typed_.enumVariants.at(carrier.success), {{carrier.value, payload}});
                output_ << "  br label %" << done << '\n' << failure << ":\n";
                const auto invalid = label(), unsupported = label(), overflow = label(), joined = label();
                output_ << "  switch i32 " << status << ", label %" << overflow << " [ i32 1, label %" << invalid << " i32 2, label %" << unsupported << " ]\n"
                        << invalid << ":\n";
                enumStore(base + ".error", *namedVariant(carrier.error, "InvalidOrder"), {});
                output_ << "  br label %" << joined << '\n' << unsupported << ":\n";
                enumStore(base + ".error", *namedVariant(carrier.error, "UnsupportedType"), {});
                output_ << "  br label %" << joined << '\n' << overflow << ":\n";
                const auto unexpected = temporary(); output_ << "  " << unexpected << " = icmp ne i32 " << status << ", 3\n"; guard(unexpected);
                enumStore(base + ".error", *namedVariant(carrier.error, "Overflow"), {});
                output_ << "  br label %" << joined << '\n' << joined << ":\n";
                const auto errorValue = temporary(); output_ << "  " << errorValue << " = load " << llvmType(carrier.error) << ", ptr " << base << ".error\n";
                enumStore(storage, typed_.enumVariants.at(carrier.failure), {{carrier.error, errorValue}});
                output_ << "  br label %" << done << '\n' << done << ":\n  " << value(item.result) << " = load " << llvmType(item.type) << ", ptr " << storage << '\n';
            }
            void emitShared(const AIRInstruction& item)
            {
                usesShared_ = true;
                const bool weak = item.operation.starts_with("weak.");
                const auto method = item.operation.substr(item.operation.find('.') + 1);
                const auto suffix = std::to_string(item.result);
                const auto scratch = "%shared." + suffix;
                const auto outputType = llvmType(item.type);
                if (weak && method == "new")
                { output_ << "  " << value(item.result) << " = freeze i64 0\n"; return; }
                const auto inputType = valueType(item.operands[0]);
                const auto input = value(item.operands[0]);
                std::string token = input;
                if (method != "new" && method != "try_unwrap")
                { token = temporary(); output_ << "  " << token << " = load i64, ptr " << input << '\n'; }
                const auto requireStatus = [&](const std::string& status)
                { const auto bad = temporary(); output_ << "  " << bad << " = icmp ne i32 " << status << ", 0\n"; guard(bad); };
                const auto call = [&](const std::string& invocation, int emptyStatus = 0, const std::string& additionalEmpty = std::string{})
                {
                    const auto status = temporary();
                    if (weak)
                    {
                        const auto empty = temporary(), skip = label(), execute = label(), done = label();
                        output_ << "  " << empty << " = icmp eq i64 " << token << ", 0\n";
                        std::string condition = empty;
                        if (!additionalEmpty.empty()) { condition = temporary(); output_ << "  " << condition << " = or i1 " << empty << ", " << additionalEmpty << '\n'; }
                        output_ << "  br i1 " << condition << ", label %" << skip << ", label %" << execute << '\n'
                                << skip << ":\n  store i32 " << emptyStatus << ", ptr " << scratch << ".status\n  br label %" << done << '\n'
                                << execute << ":\n  " << status << " = " << invocation << '\n'
                                << "  store i32 " << status << ", ptr " << scratch << ".status\n  br label %" << done << '\n' << done << ":\n";
                        const auto merged = temporary(); output_ << "  " << merged << " = load i32, ptr " << scratch << ".status\n"; return merged;
                    }
                    output_ << "  " << status << " = " << invocation << '\n'; return status;
                };
                output_ << "  store i64 0, ptr " << scratch << ".token\n  store ptr null, ptr " << scratch << ".pointer\n"
                        << "  store i64 0, ptr " << scratch << ".strong\n  store i64 0, ptr " << scratch << ".weak\n  store i32 0, ptr " << scratch << ".equal\n";
                if (method == "is_empty")
                { output_ << "  " << value(item.result) << " = icmp eq i64 " << token << ", 0\n"; return; }
                if (method == "clear")
                {
                    requireStatus(call("call i32 @aevix_weak_release(i64 " + token + ")"));
                    output_ << "  store i64 0, ptr " << input << "\n  " << value(item.result) << " = freeze {} zeroinitializer\n"; return;
                }
                if (method == "strong_count" || method == "weak_count" || method == "is_expired")
                {
                    requireStatus(call("call i32 @aevix_shared_counts(i64 " + token + ", ptr " + scratch + ".strong, ptr " + scratch + ".weak)"));
                    if (method == "is_expired")
                    { const auto count = temporary(); output_ << "  " << count << " = load i64, ptr " << scratch << ".strong\n  " << value(item.result) << " = icmp eq i64 " << count << ", 0\n"; }
                    else output_ << "  " << value(item.result) << " = load i64, ptr " << scratch << (method == "strong_count" ? ".strong" : ".weak") << '\n';
                    return;
                }
                if (method == "ptr_equal")
                {
                    const auto second = temporary(); output_ << "  " << second << " = load i64, ptr " << value(item.operands[1]) << '\n';
                    std::string secondEmpty;
                    if (weak)
                    {
                        secondEmpty = temporary(); const auto firstEmpty = temporary(), both = temporary(), encoded = temporary();
                        output_ << "  " << secondEmpty << " = icmp eq i64 " << second << ", 0\n  " << firstEmpty << " = icmp eq i64 " << token << ", 0\n"
                                << "  " << both << " = and i1 " << firstEmpty << ", " << secondEmpty << "\n  " << encoded << " = zext i1 " << both << " to i32\n  store i32 " << encoded << ", ptr " << scratch << ".equal\n";
                    }
                    requireStatus(call("call i32 @aevix_shared_same_allocation(i64 " + token + ", i64 " + second + ", ptr " + scratch + ".equal)", 0, secondEmpty));
                    const auto equal = temporary(); output_ << "  " << equal << " = load i32, ptr " << scratch << ".equal\n  " << value(item.result) << " = icmp ne i32 " << equal << ", 0\n"; return;
                }
                if (method == "as_ref")
                {
                    requireStatus(call("call i32 @aevix_shared_payload(i64 " + token + ", ptr " + scratch + ".pointer)"));
                    output_ << "  " << value(item.result) << " = load ptr, ptr " << scratch << ".pointer\n"; return;
                }
                const auto& carrier = typed_.carriers.at(item.type);
                const auto storage = "%enumtmp" + suffix;
                const auto success = label(), failure = label(), done = label();
                std::string status;
                if (method == "new")
                {
                    const auto element = typed_.types.get(carrier.value).element;
                    const auto layout = TypeLayout(typed_.types, names_, typed_.symbolTypes).layout(element);
                    if (!layout.succeeded) { error(item.source, "Cannot lay out Shared payload."); return; }
                    sharedPayloadDropTypes_.insert(element);
                    status = call("call i32 @aevix_shared_prepare(i64 " + std::to_string(layout.layout.size) + ", i64 " + std::to_string(layout.layout.alignment)
                        + ", ptr @aevix.shared.payload.drop." + std::to_string(element) + ", ptr " + scratch + ".token, ptr " + scratch + ".pointer)");
                }
                else if (method == "try_unwrap")
                {
                    const auto layout = TypeLayout(typed_.types, names_, typed_.symbolTypes).layout(carrier.value);
                    if (!layout.succeeded) { error(item.source, "Cannot lay out Shared extraction."); return; }
                    status = call("call i32 @aevix_shared_take(i64 " + token + ", ptr " + scratch + ".taken, i64 " + std::to_string(layout.layout.size) + ")");
                }
                else if (method == "get_mut") status = call("call i32 @aevix_shared_unique_payload(i64 " + token + ", ptr " + scratch + ".pointer)");
                else status = call("call i32 @aevix_" + std::string(weak ? "weak_" : "shared_") + method + "(i64 " + token + ", ptr " + scratch + ".token)", method == "upgrade" ? 7 : 0);
                const auto ok = temporary(); output_ << "  " << ok << " = icmp eq i32 " << status << ", 0\n  br i1 " << ok << ", label %" << success << ", label %" << failure << '\n' << success << ":\n";
                std::string payload = temporary();
                if (method == "new")
                {
                    const auto pointer = temporary(); output_ << "  " << pointer << " = load ptr, ptr " << scratch << ".pointer\n"
                        << "  store " << llvmType(inputType) << ' ' << input << ", ptr " << pointer << '\n'
                        << "  " << payload << " = load i64, ptr " << scratch << ".token\n";
                    const auto committed = temporary(); output_ << "  " << committed << " = call i32 @aevix_shared_commit(i64 " << payload << ")\n"; requireStatus(committed);
                }
                else if (method == "try_unwrap") output_ << "  " << payload << " = load " << llvmType(carrier.value) << ", ptr " << scratch << ".taken\n";
                else if (method == "get_mut") output_ << "  " << payload << " = load ptr, ptr " << scratch << ".pointer\n";
                else
                {
                    output_ << "  " << payload << " = load i64, ptr " << scratch << ".token\n";
                    if (method == "upgrade")
                    {
                        const auto& optional = typed_.carriers.at(carrier.value);
                        enumStore(scratch + ".optional", typed_.enumVariants.at(optional.success), {{optional.value, payload}});
                        payload = temporary(); output_ << "  " << payload << " = load " << llvmType(carrier.value) << ", ptr " << scratch << ".optional\n";
                    }
                }
                enumStore(storage, typed_.enumVariants.at(carrier.success), {{carrier.value, payload}});
                output_ << "  br label %" << done << '\n' << failure << ":\n";
                if (method == "get_mut")
                {
                    const auto unexpected = temporary(); output_ << "  " << unexpected << " = icmp ne i32 " << status << ", 8\n"; guard(unexpected);
                    enumStore(storage, typed_.enumVariants.at(carrier.failure), {});
                }
                else if (method == "try_unwrap")
                {
                    const auto unexpected = temporary(); output_ << "  " << unexpected << " = icmp ne i32 " << status << ", 8\n"; guard(unexpected);
                    enumStore(storage, typed_.enumVariants.at(carrier.failure), {{carrier.error, token}});
                }
                else if (method == "new")
                {
                    const auto allocation = temporary(), allocationBlock = label(), sizeBlock = label(), joined = label();
                    output_ << "  " << allocation << " = icmp eq i32 " << status << ", 1\n  br i1 " << allocation << ", label %" << allocationBlock << ", label %" << sizeBlock << '\n' << allocationBlock << ":\n";
                    enumStore(scratch + ".error", *namedVariant(carrier.error, "AllocationFailure"), {{inputType, input}});
                    output_ << "  br label %" << joined << '\n' << sizeBlock << ":\n";
                    const auto unexpected = temporary(); output_ << "  " << unexpected << " = icmp ne i32 " << status << ", 2\n"; guard(unexpected);
                    enumStore(scratch + ".error", *namedVariant(carrier.error, "SizeOverflow"), {{inputType, input}});
                    output_ << "  br label %" << joined << '\n' << joined << ":\n";
                    const auto errorValue = temporary(); output_ << "  " << errorValue << " = load " << llvmType(carrier.error) << ", ptr " << scratch << ".error\n";
                    enumStore(storage, typed_.enumVariants.at(carrier.failure), {{carrier.error, errorValue}});
                }
                else
                {
                    const auto countBlock = label();
                    if (method == "upgrade")
                    {
                        const auto expired = temporary(), expiredBlock = label();
                        output_ << "  " << expired << " = icmp eq i32 " << status << ", 7\n  br i1 " << expired << ", label %" << expiredBlock << ", label %" << countBlock << '\n' << expiredBlock << ":\n";
                        const auto& optional = typed_.carriers.at(carrier.value);
                        enumStore(scratch + ".optional", typed_.enumVariants.at(optional.failure), {});
                        const auto empty = temporary(); output_ << "  " << empty << " = load " << llvmType(carrier.value) << ", ptr " << scratch << ".optional\n";
                        enumStore(storage, typed_.enumVariants.at(carrier.success), {{carrier.value, empty}});
                        output_ << "  br label %" << done << '\n' << countBlock << ":\n";
                    }
                    const auto unexpected = temporary(); output_ << "  " << unexpected << " = icmp ne i32 " << status << ", 6\n"; guard(unexpected);
                    enumStore(scratch + ".error", *namedVariant(carrier.error, "CountOverflow"), {});
                    const auto errorValue = temporary(); output_ << "  " << errorValue << " = load " << llvmType(carrier.error) << ", ptr " << scratch << ".error\n";
                    enumStore(storage, typed_.enumVariants.at(carrier.failure), {{carrier.error, errorValue}});
                }
                output_ << "  br label %" << done << '\n' << done << ":\n  " << value(item.result) << " = load " << outputType << ", ptr " << storage << '\n';
            }
            void emitInstruction(const AIRInstruction& item)
            {
                currentInstruction_ = &item;
                const auto type = item.type == InvalidTypeId ? std::string{} : llvmType(item.type);
                switch (item.opcode)
                {
                    case AIROpcode::TextIntrinsic: emitText(item); return;
                    case AIROpcode::ParseIntrinsic: emitParse(item); return;
                    case AIROpcode::SystemIntrinsic: emitSystem(item); return;
                    case AIROpcode::OsIntrinsic: emitOs(item); return;
                    case AIROpcode::SharedIntrinsic: emitShared(item); return;
                    case AIROpcode::AtomicIntrinsic: emitAtomic(item); return;
                    case AIROpcode::SyncIntrinsic: emitSync(item); return;
                    case AIROpcode::CancellationIntrinsic: emitCancellation(item); return;
                    case AIROpcode::TaskIntrinsic: emitTask(item); return;
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
                    case AIROpcode::RangeInspect: emitRangeInspect(item); return;
                    case AIROpcode::CharScalar: emitCharScalar(item); return;
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
                    case AIROpcode::VectorTake:
                    case AIROpcode::VectorInsert:
                    {
                        const auto [owner, vector] = address(item.place);
                        const auto element = typed_.types.get(vector).element;
                        const auto spelling = llvmType(vector), elementSpelling = llvmType(element);
                        const auto layout = TypeLayout(typed_.types, names_, typed_.symbolTypes).layout(element);
                        if (!layout.succeeded) { error(item.source, "Cannot determine Vector mutation element layout."); return; }
                        const auto descriptor = temporary(), pointer = temporary(), length = temporary(), outside = temporary();
                        const auto index = value(item.operands[0]);
                        const bool insert = item.opcode == AIROpcode::VectorInsert;
                        output_ << "  " << descriptor << " = load " << spelling << ", ptr " << owner << '\n'
                                << "  " << pointer << " = extractvalue " << spelling << ' ' << descriptor << ", 0\n"
                                << "  " << length << " = extractvalue " << spelling << ' ' << descriptor << ", 1\n"
                                << "  " << outside << " = icmp " << (insert ? "ugt" : "uge") << " i64 " << index << ", " << length << '\n';
                        guard(outside);
                        const auto destination = temporary(), last = temporary(), updated = temporary();
                        output_ << "  " << destination << " = getelementptr " << elementSpelling << ", ptr " << pointer << ", i64 " << index << '\n'
                                << "  " << last << " = " << (insert ? "add" : "sub") << " i64 " << length << ", 1\n";
                        if (insert)
                        {
                            const auto capacity = temporary(), full = temporary();
                            output_ << "  " << capacity << " = extractvalue " << spelling << ' ' << descriptor << ", 2\n"
                                    << "  " << full << " = icmp uge i64 " << length << ", " << capacity << '\n';
                            guard(full);
                            const auto next = temporary(), shifted = temporary(), count = temporary(), bytes = temporary();
                            output_ << "  " << next << " = add i64 " << index << ", 1\n"
                                    << "  " << shifted << " = getelementptr " << elementSpelling << ", ptr " << pointer << ", i64 " << next << '\n'
                                    << "  " << count << " = sub i64 " << length << ", " << index << '\n'
                                    << "  " << bytes << " = mul i64 " << count << ", " << layout.layout.size << '\n'
                                    << "  call void @llvm.memmove.p0.p0.i64(ptr " << shifted << ", ptr " << destination << ", i64 " << bytes << ", i1 false)\n"
                                    << "  store " << elementSpelling << ' ' << value(item.operands[1]) << ", ptr " << destination << '\n'
                                    << "  " << value(item.result) << " = freeze {} zeroinitializer\n";
                            usesMemmove_ = true;
                        }
                        else
                        {
                            if (item.operation == "pop")
                            {
                                const auto notLast = temporary();
                                output_ << "  " << notLast << " = icmp ne i64 " << index << ", " << last << '\n'; guard(notLast);
                            }
                            output_ << "  " << value(item.result) << " = load " << elementSpelling << ", ptr " << destination << '\n';
                            const auto finalSlot = temporary();
                            output_ << "  " << finalSlot << " = getelementptr " << elementSpelling << ", ptr " << pointer << ", i64 " << last << '\n';
                            if (item.operation == "swap_remove")
                            {
                                const auto moved = temporary();
                                output_ << "  " << moved << " = load " << elementSpelling << ", ptr " << finalSlot << '\n'
                                        << "  store " << elementSpelling << ' ' << moved << ", ptr " << destination << '\n';
                            }
                            else if (item.operation == "remove")
                            {
                                const auto next = temporary(), source = temporary(), count = temporary(), bytes = temporary();
                                output_ << "  " << next << " = add i64 " << index << ", 1\n"
                                        << "  " << source << " = getelementptr " << elementSpelling << ", ptr " << pointer << ", i64 " << next << '\n'
                                        << "  " << count << " = sub i64 " << last << ", " << index << '\n'
                                        << "  " << bytes << " = mul i64 " << count << ", " << layout.layout.size << '\n'
                                        << "  call void @llvm.memmove.p0.p0.i64(ptr " << destination << ", ptr " << source << ", i64 " << bytes << ", i1 false)\n";
                                usesMemmove_ = true;
                            }
                            output_ << "  store " << elementSpelling << " zeroinitializer, ptr " << finalSlot << '\n';
                        }
                        output_ << "  " << updated << " = insertvalue " << spelling << ' ' << descriptor << ", i64 " << last << ", 1\n"
                                << "  store " << spelling << ' ' << updated << ", ptr " << owner << '\n';
                        return;
                    }
                    case AIROpcode::VectorTruncate:
                    {
                        const auto [owner, vector] = address(item.place); truncateTypes_.insert(vector);
                        output_ << "  call void @aevix.vector.truncate." << vector << "(ptr " << owner << ", i64 " << (item.operation == "clear" ? std::string("0") : value(item.operands[0])) << ")\n"
                                << "  " << value(item.result) << " = freeze {} zeroinitializer\n";
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
                    case AIROpcode::BoxExtract:
                    {
                        if (!validBoxExtractionShape(*function_, item, typed_.types))
                        { error(item.source, "Box extraction owner/result metadata is invalid."); return; }
                        // 실제 T를 먼저 이동하고 바깥 할당만 해제한다. payload Drop은 호출하지 않는다.
                        // Move the exact T first, then release only outer storage without invoking payload Drop.
                        usesAllocation_ = true;
                        const auto [storage, ignored] = address(item.place); (void)ignored;
                        const auto pointer = temporary();
                        output_ << "  " << pointer << " = load ptr, ptr " << storage << '\n'
                                << "  " << value(item.result) << " = load " << type << ", ptr " << pointer << '\n'
                                << "  call void @free(ptr " << pointer << ")\n"
                                << "  store i1 false, ptr %initialized" << item.place.local << '\n';
                        return;
                    }
                    case AIROpcode::MemoryIntrinsic:
                    {
                        const auto element = typed_.types.get(valueType(item.operands[0])).element;
                        const auto storage = llvmType(element);
                        const auto left = value(item.operands[0]);
                        if (item.operation == "replace")
                        {
                            // 기존 값을 읽고 즉시 교체한다. 대여 저장소를 비우거나 Drop을 호출하지 않는다.
                            // Read the old value and replace it immediately without emptying storage or invoking Drop.
                            output_ << "  " << value(item.result) << " = load " << storage << ", ptr " << left << '\n'
                                    << "  store " << storage << ' ' << value(item.operands[1]) << ", ptr " << left << '\n';
                        }
                        else
                        {
                            const auto right = value(item.operands[1]);
                            const auto first = temporary(), second = temporary();
                            output_ << "  " << first << " = load " << storage << ", ptr " << left << '\n'
                                    << "  " << second << " = load " << storage << ", ptr " << right << '\n'
                                    << "  store " << storage << ' ' << second << ", ptr " << left << '\n'
                                    << "  store " << storage << ' ' << first << ", ptr " << right << '\n'
                                    << "  " << value(item.result) << " = freeze {} zeroinitializer\n";
                        }
                        return;
                    }
                    case AIROpcode::Constant:
                        output_ << "  " << value(item.result) << " = freeze " << type << ' ' << literal(item.operation, item.type) << '\n'; return;
                    case AIROpcode::ClosureConsume:
                    {
                        // 검증된 환경의 모든 필드를 옮긴 뒤 부모를 비활성화한다. 사용자 코드는 중간 상태를 볼 수 없다.
                        // Transfer every verified environment field, then deactivate its owner before any user-body instruction.
                        const auto environment = temporary();
                        output_ << "  " << environment << " = load " << type << ", ptr %l" << item.place.local << '\n';
                        for (std::size_t field = 0; field < item.closureLocals.size(); ++field)
                        {
                            const auto local = item.closureLocals[field]; const auto capture = temporary();
                            output_ << "  " << capture << " = extractvalue " << type << ' ' << environment << ", " << field << '\n'
                                    << "  store " << llvmType(function_->locals[local].type) << ' ' << capture << ", ptr %l" << local << '\n'
                                    << "  store i1 true, ptr %initialized" << local << '\n';
                        }
                        output_ << "  store i1 false, ptr %initialized" << item.place.local << '\n';
                        return;
                    }
                    case AIROpcode::Read: case AIROpcode::Move:
                    {
                        const auto [pointer, ignored] = address(item.place); (void)ignored;
                        output_ << "  " << value(item.result) << " = load " << type << ", ptr " << pointer << '\n';
                        // 이동한 필드는 비활성으로 표시한다. 0으로 채운 사용자 값도 유효한 Drop 값은 아니다.
                        // Mark moved fields inactive: zero-filled user values are not valid Drop values.
                        if (item.opcode == AIROpcode::Move && item.operation == "enum.extract")
                        {
                            const auto& field = item.place.projections.front();
                            output_ << "  store i1 false, ptr " << enumLive(item.place.local, field.variant, field.field) << '\n';
                        }
                        if (item.opcode == AIROpcode::Move && item.place.local != InvalidAIRId && item.place.projections.empty())
                            output_ << "  store i1 false, ptr %initialized" << item.place.local << '\n';
                        return;
                    }
                    case AIROpcode::Write:
                    {
                        const bool initialization = function_->initializesGlobal == item.place.global && item.place.projections.empty() && item.operation == "persist.initialize";
                        const auto [pointer, ignored] = address(item.place, initialization); (void)ignored;
                        output_ << "  store " << type << ' ' << value(item.operands[0]) << ", ptr " << pointer << '\n';
                        if (item.place.local != InvalidAIRId && item.place.projections.empty())
                        {
                            output_ << "  store i1 true, ptr %initialized" << item.place.local << '\n';
                            for (const auto& [variant, field] : extractedFields(item.place.local))
                                output_ << "  store i1 true, ptr " << enumLive(item.place.local, variant, field) << '\n';
                        }
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
                        emitLocalDrop(slot);
                        output_ << "  store " << llvmType(function_->locals[slot].type) << " zeroinitializer, ptr %l" << slot << '\n'
                                << "  store i1 false, ptr %initialized" << slot << "\n  br label %" << done << '\n' << done << ":\n";
                        return;
                    }
                }
            }
            bool needsDrop(TypeId id, std::set<TypeId>& active)
            {
                const auto& type = typed_.types.get(id);
                if (typed_.dropMethods.contains(id)) return true;
                if (taskOwner(type.kind) || cancellationOwner(type.kind) || syncGuard(type.kind) || type.kind == TypeKind::Vector || type.kind == TypeKind::Box || type.kind == TypeKind::Shared || type.kind == TypeKind::Weak || type.kind == TypeKind::String) return true;
                if (syncOwner(type.kind)) return needsDrop(type.element, active);
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
            std::string enumLive(AIRLocalId local, SymbolId variant, SymbolId field) const
            { return "%enumlive." + std::to_string(local) + "." + std::to_string(variant) + "." + std::to_string(field); }
            std::set<std::pair<SymbolId, SymbolId>> extractedFields(AIRLocalId local) const
            {
                std::set<std::pair<SymbolId, SymbolId>> result;
                for (const auto& block : function_->blocks)
                    for (const auto& item : block.instructions)
                        if (item.opcode == AIROpcode::Move && item.operation == "enum.extract" && item.place.local == local
                            && item.place.projections.size() == 1 && item.place.projections[0].kind == AIRProjectionKind::EnumField)
                            result.emplace(item.place.projections[0].variant, item.place.projections[0].field);
                return result;
            }
            void emitLocalDrop(AIRLocalId local)
            {
                const auto id = function_->locals[local].type;
                const auto pointer = "%l" + std::to_string(local);
                const auto fields = extractedFields(local);
                if (fields.empty()) { emitDropCall(id, pointer); return; }
                const auto* info = record(id);
                if (!info || !info->isEnum || typed_.dropMethods.contains(id))
                { error(function_->source, "Partial enum cleanup requires a non-Drop enum owner."); return; }
                const auto tag = temporary(), done = label();
                std::vector<std::string> branches;
                for (std::size_t i = 0; i < info->variants.size(); ++i) branches.push_back(label());
                output_ << "  " << tag << " = load i32, ptr " << pointer << "\n  switch i32 " << tag << ", label %" << done << " [\n";
                for (std::size_t i = 0; i < info->variants.size(); ++i)
                    output_ << "    i32 " << info->variants[i].tag << ", label %" << branches[i] << '\n';
                output_ << "  ]\n";
                for (std::size_t index = 0; index < info->variants.size(); ++index)
                {
                    const auto& variant = info->variants[index];
                    output_ << branches[index] << ":\n";
                    for (std::size_t i = variant.fields.size(); i > 0; --i)
                    {
                        const auto field = variant.fields[i - 1], type = variant.fieldTypes[i - 1];
                        std::set<TypeId> active;
                        if (!needsDrop(type, active)) continue;
                        std::string next;
                        if (fields.contains({variant.variant, i - 1}))
                        {
                            const auto live = temporary(), drop = label(); next = label();
                            output_ << "  " << live << " = load i1, ptr " << enumLive(local, variant.variant, i - 1) << '\n'
                                    << "  br i1 " << live << ", label %" << drop << ", label %" << next << '\n' << drop << ":\n";
                        }
                        const auto address = temporary();
                        output_ << "  " << address << " = getelementptr i8, ptr " << pointer << ", i64 " << enumOffset(id, field) << '\n';
                        emitDropCall(type, address);
                        if (!next.empty()) output_ << "  br label %" << next << '\n' << next << ":\n";
                    }
                    output_ << "  br label %" << done << '\n';
                }
                output_ << done << ":\n";
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
            void emitVectorTruncations()
            {
                for (const auto vector : truncateTypes_)
                {
                    temporary_ = 0; label_ = 0;
                    const auto element = typed_.types.get(vector).element;
                    const auto spelling = llvmType(vector);
                    output_ << "define internal void @aevix.vector.truncate." << vector << "(ptr %owner, i64 %wanted) {\nentry:\n"
                            << "  %descriptor = load " << spelling << ", ptr %owner\n"
                            << "  %length = extractvalue " << spelling << " %descriptor, 1\n"
                            << "  %shorter = icmp ult i64 %wanted, %length\n"
                            << "  br i1 %shorter, label %prepare, label %done\nprepare:\n"
                            << "  %data = extractvalue " << spelling << " %descriptor, 0\n"
                            << "  %updated = insertvalue " << spelling << " %descriptor, i64 %wanted, 1\n"
                            << "  store " << spelling << " %updated, ptr %owner\n"
                            << "  br label %loop\nloop:\n"
                            << "  %index = phi i64 [ %length, %prepare ], [ %previous, %body ]\n"
                            << "  %finished = icmp eq i64 %index, %wanted\n"
                            << "  br i1 %finished, label %done, label %body\nbody:\n"
                            << "  %previous = sub i64 %index, 1\n"
                            << "  %slot = getelementptr " << llvmType(element) << ", ptr %data, i64 %previous\n";
                    // 길이 밖 소유자는 한 번 정리하고 남은 바이트를 새 값처럼 해제하지 않는다.
                    // Drop each removed owner once; stale tail bytes never become another initialized value.
                    emitDropCall(element, "%slot");
                    output_ << "  store " << llvmType(element) << " zeroinitializer, ptr %slot\n"
                            << "  br label %loop\ndone:\n  ret void\n}\n";
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
            void emitSharedPayloadDrops()
            {
                for (const auto type : sharedPayloadDropTypes_)
                {
                    temporary_ = 0; label_ = 0;
                    output_ << "define internal void @aevix.shared.payload.drop." << type << "(ptr %storage) {\nentry:\n";
                    emitDropCall(type, "%storage"); output_ << "  ret void\n}\n";
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
                    // 사용자 정리를 먼저 호출하고, 반환한 뒤 남은 필드를 역순으로 해제한다.
                    // Run user cleanup first, then release remaining fields in reverse order.
                    if (const auto custom = typed_.dropMethods.find(id); custom != typed_.dropMethods.end())
                    {
                        const auto* cleanup = function(custom->second);
                        bool valid = cleanup && typed_.types.get(cleanup->result).kind == TypeKind::Void
                            && cleanup->parameters.size() == 1;
                        if (valid)
                        {
                            const auto parameter = cleanup->parameters[0];
                            valid = parameter < cleanup->locals.size();
                            if (valid)
                            {
                                const auto& receiver = typed_.types.get(cleanup->locals[parameter].type);
                                valid = receiver.kind == TypeKind::Reference && receiver.isMutable && receiver.element == id;
                            }
                        }
                        if (!valid) error(cleanup ? cleanup->source : nullptr, "Invalid compiler-managed Drop method signature.");
                        else output_ << "  call void @aevix.fn." << custom->second << "(ptr %object)\n";
                    }
                    if (syncGuard(type.kind))
                    {
                        usesSync_ = true;
                        output_ << "  %guard = load " << llvmType(id) << ", ptr %object\n  %state = extractvalue " << llvmType(id) << " %guard, 0\n"
                                << "  %empty = icmp eq ptr %state, null\n  br i1 %empty, label %done, label %release\nrelease:\n";
                        if (type.kind == TypeKind::RwLockReadGuard)
                            output_ << "  %status = call i32 @aevix_rwlock_read_unlock(ptr %state)\n";
                        else
                            output_ << "  %pending = extractvalue " << llvmType(id) << " %guard, 2\n  %mark = zext i1 %pending to i32\n"
                                    << "  %status = call i32 @aevix_" << (type.kind == TypeKind::MutexGuard ? "mutex" : "rwlock_write") << "_unlock(ptr %state, i32 %mark)\n";
                        output_ << "  %failed = icmp ne i32 %status, 0\n"; guard("%failed");
                        output_ << "  br label %done\ndone:\n";
                    }
                    else if (syncOwner(type.kind))
                    {
                        const auto payload = temporary();
                        output_ << "  " << payload << " = getelementptr " << llvmType(id) << ", ptr %object, i32 0, i32 1\n";
                        emitDropCall(type.element, payload);
                    }
                    else if (taskOwner(type.kind))
                    {
                        usesTask_ = true;
                        if (type.kind == TypeKind::Task)
                            output_ << "  %observation = getelementptr " << llvmType(id) << ", ptr %object, i32 0, i32 0\n";
                        output_ << "  %token = load i64, ptr " << (type.kind == TypeKind::Task ? "%observation" : "%object") << "\n  %empty = icmp eq i64 %token, 0\n  br i1 %empty, label %done, label %release\nrelease:\n"
                                << "  %status = call i32 @aevix_" << (type.kind == TypeKind::TaskScope ? "task_scope_cleanup" : "scoped_task_discard") << "(i64 %token)\n"
                                << "  %failed = icmp ne i32 %status, 0\n";
                        guard("%failed"); output_ << "  br label %done\ndone:\n";
                    }
                    else if (cancellationOwner(type.kind))
                    {
                        usesCancellation_ = true;
                        output_ << "  %token = load i64, ptr %object\n  %status = call i32 @aevix_cancel_"
                                << (type.kind == TypeKind::CancellationSource ? "source" : "token") << "_release(i64 %token)\n"
                                << "  %failed = icmp ne i32 %status, 0\n";
                        guard("%failed");
                    }
                    else if (type.kind == TypeKind::Shared || type.kind == TypeKind::Weak)
                    {
                        usesShared_ = true;
                        output_ << "  %token = load i64, ptr %object\n  %empty = icmp eq i64 %token, 0\n"
                                << "  br i1 %empty, label %done, label %release\nrelease:\n"
                                << "  %status = call i32 @aevix_" << (type.kind == TypeKind::Weak ? "weak" : "shared") << "_release(i64 %token)\n"
                                << "  %failed = icmp ne i32 %status, 0\n";
                        guard("%failed"); output_ << "  br label %done\ndone:\n";
                    }
                    else if (type.kind == TypeKind::Box)
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
                currentInstruction_ = nullptr;
                proofRanges_ = {};
                if (proofOptions_.enabled)
                {
                    const auto started = std::chrono::steady_clock::now();
                    proofRanges_ = analyzeProofRanges(function, typed_.types);
                    if (auto* statistics = proofOptions_.statistics)
                    {
                        statistics->analysisNanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
                        statistics->functions += proofRanges_.stats.functions;
                        statistics->skipped += proofRanges_.stats.skipped;
                        statistics->budgetFallbacks += proofRanges_.stats.budgetFallback ? 1 : 0;
                        statistics->arithmeticConsidered += proofRanges_.stats.arithmeticConsidered;
                        statistics->arithmeticProven += proofRanges_.stats.arithmeticProven;
                        statistics->fixedIndicesConsidered += proofRanges_.stats.fixedIndicesConsidered;
                        statistics->fixedIndicesProven += proofRanges_.stats.fixedIndicesProven;
                        statistics->cfgIterations += proofRanges_.stats.cfgIterations;
                    }
                }
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
                        {
                            output_ << "  %l" << index << " = alloca " << llvmType(function.locals[index].type) << ", align " << layout(function.locals[index].type).alignment << "\n"
                                    << "  %initialized" << index << " = alloca i1\n  store i1 false, ptr %initialized" << index << '\n';
                            for (const auto& [variant, field] : extractedFields(index))
                                output_ << "  " << enumLive(index, variant, field) << " = alloca i1\n  store i1 true, ptr " << enumLive(index, variant, field) << '\n';
                        }
                        for (const auto& basicBlock : function.blocks)
                            for (const auto& instruction : basicBlock.instructions)
                            {
                                if (instruction.opcode == AIROpcode::OsIntrinsic && typed_.types.get(instruction.type).kind == TypeKind::Tuple)
                                {
                                    output_ << "  %osout" << instruction.result << " = alloca "
                                            << llvmType(typed_.types.get(instruction.type).parameters[1]) << '\n';
                                    if (instruction.operation == "env_get")
                                        output_ << "  %ospresent" << instruction.result << " = alloca i32\n  store i32 0, ptr %ospresent" << instruction.result << '\n';
                                }
                                if (instruction.opcode == AIROpcode::ComplexIntrinsic && instruction.operation == "divide_components")
                                    output_ << "  %complex.real." << instruction.result << " = alloca double\n  %complex.imag." << instruction.result << " = alloca double\n";
                                if (instruction.opcode == AIROpcode::ArrayRepeat)
                                    output_ << "  %repeattmp" << instruction.result << " = alloca " << llvmType(instruction.type) << '\n';
                                if (instruction.opcode == AIROpcode::TaskIntrinsic)
                                {
                                    const auto base = "%task." + std::to_string(instruction.result);
                                    output_ << "  " << base << ".token = alloca i64\n  " << base << ".pointer = alloca ptr\n  " << base << ".flag = alloca i32\n  " << base << ".snapshot = alloca [3 x i32]\n";
                                    const auto* operation = taskOperation(instruction.operation);
                                    if (operation && operation->result == TaskResult::ActiveCount)
                                        output_ << "  " << base << ".count = alloca i64\n";
                                    if (operation && operation->worker)
                                        output_ << "  " << base << ".identity = alloca i64\n  " << base << ".capture = alloca " << llvmType(instruction.taskCallable) << ", align " << layout(instruction.taskCallable).alignment << '\n';
                                    else if (operation && operation->preparation == TaskPreparation::Ready)
                                        output_ << "  " << base << ".identity = alloca i64\n  " << base << ".ready = alloca " << llvmType(instruction.taskElement) << ", align " << layout(instruction.taskElement).alignment << '\n';
                                    if (operation && (operation->result == TaskResult::Join || operation->result == TaskResult::TryTake))
                                        output_ << "  " << base << ".result = alloca " << llvmType(instruction.taskElement) << ", align " << layout(instruction.taskElement).alignment << '\n';
                                    if (const auto carrier = typed_.carriers.find(instruction.type); carrier != typed_.carriers.end())
                                    {
                                        output_ << "  %enumtmp" << instruction.result << " = alloca " << llvmType(instruction.type) << ", align " << layout(instruction.type).alignment << '\n'
                                                << "  " << base << ".error = alloca " << llvmType(carrier->second.error) << ", align " << layout(carrier->second.error).alignment << '\n'
                                                << "  " << base << ".success = alloca " << llvmType(carrier->second.value) << ", align " << layout(carrier->second.value).alignment << '\n';
                                        if (operation && operation->result == TaskResult::TryTake)
                                            output_ << "  " << base << ".optional = alloca " << llvmType(carrier->second.value) << ", align " << layout(carrier->second.value).alignment << '\n';
                                        if (operation && (operation->result == TaskResult::Close || operation->result == TaskResult::CloseAdmission))
                                        {
                                            const bool closing = operation->result == TaskResult::Close;
                                            const auto* variant = namedVariant(carrier->second.error,closing ? "Retry" : "Failure");
                                            if (variant && variant->fieldTypes.size() == (closing ? 2u : 1u))
                                                output_ << "  " << base << ".reason = alloca " << llvmType(variant->fieldTypes[closing ? 1 : 0]) << '\n';
                                            if (closing) output_ << "  " << base << ".report = alloca " << llvmType(carrier->second.value) << '\n';
                                        }
                                    }
                                    else output_ << "  " << base << ".status = alloca " << llvmType(instruction.type) << '\n';
                                }
                                if (instruction.opcode == AIROpcode::CancellationIntrinsic)
                                {
                                    const auto base = "%cancel." + std::to_string(instruction.result);
                                    output_ << "  " << base << ".token = alloca i64\n  " << base << ".flag = alloca i32\n";
                                    if (const auto carrier = typed_.carriers.find(instruction.type); carrier != typed_.carriers.end())
                                    {
                                        output_ << "  %enumtmp" << instruction.result << " = alloca " << llvmType(instruction.type) << '\n'
                                                << "  " << base << ".error = alloca " << llvmType(carrier->second.error) << '\n';
                                        if (const auto* operation = cancellationOperation(instruction.operation); operation && operation->result == CancellationResult::Request)
                                            output_ << "  " << base << ".request = alloca " << llvmType(carrier->second.value) << '\n';
                                    }
                                }
                                if (instruction.opcode == AIROpcode::SyncIntrinsic)
                                {
                                    const auto base = "%sync." + std::to_string(instruction.result);
                                    output_ << "  " << base << ".poison = alloca i32\n  store i32 0, ptr " << base << ".poison\n";
                                    if (instruction.operation.ends_with(".into_inner") && !instruction.operation.starts_with("poisoned."))
                                        output_ << "  " << base << ".owner = alloca " << llvmType(valueType(instruction.operands[0])) << '\n';
                                    if (const auto carrier = typed_.carriers.find(instruction.type); carrier != typed_.carriers.end())
                                    {
                                        output_ << "  %enumtmp" << instruction.result << " = alloca " << llvmType(instruction.type) << '\n'
                                                << "  " << base << ".error = alloca " << llvmType(carrier->second.error) << '\n';
                                        const auto* operation = syncOperation(instruction.operation);
                                        if (operation && operation->result == SyncResult::OptionalAcquire)
                                            output_ << "  " << base << ".optional = alloca " << llvmType(carrier->second.value) << '\n';
                                    }
                                }
                                if (instruction.opcode == AIROpcode::AtomicIntrinsic)
                                {
                                    const auto base = "%atomic." + std::to_string(instruction.result);
                                    output_ << "  " << base << ".observed = alloca i64\n  " << base << ".changed = alloca i32\n";
                                    if (const auto carrier = typed_.carriers.find(instruction.type); carrier != typed_.carriers.end())
                                        output_ << "  %enumtmp" << instruction.result << " = alloca " << llvmType(instruction.type) << '\n'
                                                << "  " << base << ".error = alloca " << llvmType(carrier->second.error) << '\n';
                                }
                                if (instruction.opcode == AIROpcode::SharedIntrinsic)
                                {
                                    const auto base = "%shared." + std::to_string(instruction.result);
                                    output_ << "  " << base << ".token = alloca i64\n  " << base << ".pointer = alloca ptr\n"
                                            << "  " << base << ".strong = alloca i64\n  " << base << ".weak = alloca i64\n"
                                            << "  " << base << ".equal = alloca i32\n  " << base << ".status = alloca i32\n";
                                    if (const auto carrier = typed_.carriers.find(instruction.type); carrier != typed_.carriers.end())
                                    {
                                        output_ << "  %enumtmp" << instruction.result << " = alloca " << llvmType(instruction.type) << '\n';
                                        if (!carrier->second.optional) output_ << "  " << base << ".error = alloca " << llvmType(carrier->second.error) << '\n';
                                        if (instruction.operation == "weak.upgrade") output_ << "  " << base << ".optional = alloca " << llvmType(carrier->second.value) << '\n';
                                        if (instruction.operation == "shared.try_unwrap") output_ << "  " << base << ".taken = alloca " << llvmType(carrier->second.value) << '\n';
                                    }
                                }
                                if (instruction.opcode == AIROpcode::EnumConstruct || instruction.operation == "box.new"
                                    || instruction.opcode == AIROpcode::ParseIntrinsic
                                    || (instruction.opcode == AIROpcode::CharScalar && instruction.operation == "Char.from_u32")
                                    || instruction.opcode == AIROpcode::SystemIntrinsic
                                    || (instruction.opcode == AIROpcode::TextIntrinsic && typed_.carriers.contains(instruction.type)))
                                {
                                    output_ << "  %enumtmp" << instruction.result << " = alloca " << llvmType(instruction.type) << '\n';
                                    if (instruction.operation == "box.new") output_ << "  %boxerrtmp" << instruction.result << " = alloca " << llvmType(typed_.carriers.at(instruction.type).error) << '\n';
                                    if (instruction.opcode == AIROpcode::CharScalar)
                                        output_ << "  %charerror" << instruction.result << " = alloca " << llvmType(typed_.carriers.at(instruction.type).error) << '\n';
                                    if (instruction.opcode == AIROpcode::SystemIntrinsic)
                                    {
                                        const auto& carrier = typed_.carriers.at(instruction.type);
                                        const auto suffix = std::to_string(instruction.result);
                                        output_ << "  %syserror" << suffix << " = alloca " << llvmType(carrier.error) << '\n';
                                        if (instruction.operation == "input")
                                        {
                                            const auto& optional = typed_.carriers.at(carrier.value);
                                            const auto* io = namedVariant(carrier.error, "Io"), *parse = namedVariant(carrier.error, "Parse");
                                            if (io && parse)
                                                output_ << "  %sysdata" << suffix << " = alloca ptr\n  %syslen" << suffix << " = alloca i64\n  %syscap" << suffix << " = alloca i64\n"
                                                        << "  %sysoptional" << suffix << " = alloca " << llvmType(carrier.value) << '\n'
                                                        << "  %sysio" << suffix << " = alloca " << llvmType(io->fieldTypes[0]) << '\n'
                                                        << "  %sysparseerror" << suffix << " = alloca " << llvmType(parse->fieldTypes[0]) << '\n'
                                                        << "  %syspayload" << suffix << " = alloca " << llvmType(optional.value) << '\n';
                                        }
                                        else
                                        {
                                            const auto begin = instruction.operation == "log" ? 1u : 0u;
                                            const auto end = instruction.operands.size() - (instruction.operation == "log" ? 1u : 0u);
                                            output_ << "  %sysparts" << suffix << " = alloca [" << end - begin << " x { ptr, i64, i32, i32 }]\n";
                                            for (std::size_t index = begin; index < end; ++index)
                                                output_ << "  %syspart" << suffix << '.' << index << " = alloca "
                                                        << (typed_.types.get(valueType(instruction.operands[index])).kind == TypeKind::Bool ? "i8" : llvmType(valueType(instruction.operands[index]))) << '\n';
                                        }
                                    }
                                    if (instruction.opcode == AIROpcode::ParseIntrinsic)
                                    {
                                        const auto& carrier = typed_.carriers.at(instruction.type);
                                        output_ << "  %parsepayload" << instruction.result << " = alloca " << llvmType(carrier.value) << '\n'
                                                << "  %parseerror" << instruction.result << " = alloca " << llvmType(carrier.error) << '\n';
                                    }
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
                                      const NameResolution& names, Diagnostics& diagnostics, std::string targetTriple, ProofOptimizationOptions proof)
    {
        if (proof.statistics) *proof.statistics = {};
        if (!diagnostics.empty() || !verifyAIR(module, types.types, diagnostics) || !validateTaskWorkers(module,types,diagnostics)) return std::nullopt;
        return Emitter(module, types, names, diagnostics, std::move(targetTriple), proof).run();
    }
}
