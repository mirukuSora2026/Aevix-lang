// ========================
//
// Created By : miruku_sora2026
//
// File name : Module.cpp
//
// Description :
//     AIR 덤프와 기본 구조·타입 불변식 검증을 제공한다.
//     Provide AIR dumps and validation of structural and typing invariants.
//
// ========================
#include "aevix/IR/AIR/Module.hpp"
#include "aevix/Native/AtomicOperation.hpp"
#include "aevix/Native/TaskOperation.hpp"
#include "aevix/Native/RangeOperation.hpp"
#include "aevix/Native/SyncOperation.hpp"
#include "aevix/Native/OsOperation.hpp"
#include <sstream>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <array>
#include <functional>

namespace aevix
{
    namespace
    {
        const char* name(AIROpcode opcode)
        {
            switch (opcode)
            {
                case AIROpcode::Constant: return "constant";
                case AIROpcode::Read: return "read";
                case AIROpcode::Move: return "move";
                case AIROpcode::Write: return "write";
                case AIROpcode::BorrowShared: return "borrow";
                case AIROpcode::BorrowMut: return "borrow_mut";
                case AIROpcode::ClosureConsume: return "closure_consume";
                case AIROpcode::BoxExtract: return "box_extract";
                case AIROpcode::Unary: return "unary";
                case AIROpcode::Binary: return "binary";
                case AIROpcode::Call: return "call";
                case AIROpcode::Aggregate: return "aggregate";
                case AIROpcode::EnumConstruct: return "enum_construct";
                case AIROpcode::EnumTag: return "enum_tag";
                case AIROpcode::EnsurePersist: return "persist_ensure";
                case AIROpcode::ScopeBegin: return "scope_begin";
                case AIROpcode::ScopeEnd: return "scope_end";
                case AIROpcode::Drop: return "drop";
                case AIROpcode::RangeInspect: return "range_inspect";
                case AIROpcode::CharScalar: return "char_scalar";
                case AIROpcode::RangeConstruct: return "range_construct";
                case AIROpcode::RangeWithStep: return "range_with_step";
                case AIROpcode::RangeCount: return "range_count";
                case AIROpcode::RangeCountFits: return "range_count_fits";
                case AIROpcode::RangeTest: return "range_test";
                case AIROpcode::RangeNext: return "range_next";
                case AIROpcode::RangeValue: return "range_value";
                case AIROpcode::SliceConstruct: return "slice_construct";
                case AIROpcode::SliceLength: return "slice_length";
                case AIROpcode::VectorAllocate: return "vector_allocate";
                case AIROpcode::VectorAvailable: return "vector_available";
                case AIROpcode::VectorAppend: return "vector_append";
                case AIROpcode::VectorLength: return "vector_length";
                case AIROpcode::VectorReserve: return "vector_reserve";
                case AIROpcode::VectorTake: return "vector_take";
                case AIROpcode::VectorTruncate: return "vector_truncate";
                case AIROpcode::VectorInsert: return "vector_insert";
                case AIROpcode::ArrayRepeat: return "array_repeat";
                case AIROpcode::TextIntrinsic: return "text_intrinsic";
                case AIROpcode::OsIntrinsic: return "os_intrinsic";
                case AIROpcode::ParseIntrinsic: return "parse_intrinsic";
                case AIROpcode::SystemIntrinsic: return "system_intrinsic";
                case AIROpcode::MemoryIntrinsic: return "memory_intrinsic";
                case AIROpcode::SharedIntrinsic: return "shared_intrinsic";
                case AIROpcode::AtomicIntrinsic: return "atomic_intrinsic";
                case AIROpcode::SyncIntrinsic: return "sync_intrinsic";
                case AIROpcode::CancellationIntrinsic: return "cancellation_intrinsic";
                case AIROpcode::TaskIntrinsic: return "task_intrinsic";
                case AIROpcode::ParallelIntrinsic: return "parallel_intrinsic";
                case AIROpcode::SliceSubview: return "slice_subview";
                case AIROpcode::NumericStatus: return "numeric_status";
                case AIROpcode::NumericConvert: return "numeric_convert";
                case AIROpcode::Simd: return "simd";
                case AIROpcode::ComplexIntrinsic: return "complex";
            }
            return "invalid";
        }
    }
    std::string dumpAIR(const AIRModule& module, const TypeContext& types)
    {
        std::ostringstream output;
        for (const auto& external : module.externals)
        {
            output << "extern " << external.callingConvention << " @" << external.symbol << ' ' << external.name << " -> " << types.display(external.result) << '\n';
        }
        for (const auto& global : module.globals) output << "global @" << global.symbol << ' ' << global.name << ": " << types.display(global.type) << '\n';
        for (const auto& function : module.functions)
        {
            output << "function @" << function.symbol << ' ' << function.name << " -> " << types.display(function.result) << " {\n";
            for (std::size_t index = 0; index < function.locals.size(); ++index)
                output << "  local $" << index << ' ' << function.locals[index].name << ": " << types.display(function.locals[index].type) << " scope=" << function.locals[index].scope << '\n';
            for (std::size_t block = 0; block < function.blocks.size(); ++block)
            {
                output << "b" << block << " (" << function.blocks[block].name << "):\n";
                for (const auto& item : function.blocks[block].instructions)
                {
                    output << "  ";
                    if (item.result != InvalidAIRId) output << '%' << item.result << " = ";
                    output << name(item.opcode);
                    if (!item.operation.empty()) output << ' ' << item.operation;
                    if (item.type != InvalidTypeId) output << " : " << types.display(item.type);
                    if (item.place.local != InvalidAIRId) output << " $" << item.place.local;
                    if (item.place.global != InvalidSymbolId) output << " @" << item.place.global;
                    if (item.place.pointer != InvalidAIRId) output << " *%" << item.place.pointer;
                    for (const auto& projection : item.place.projections)
                        if (projection.kind == AIRProjectionKind::Field) output << ".field" << projection.field;
                        else if (projection.kind == AIRProjectionKind::EnumField) output << ".variant" << projection.variant << ".field" << projection.field;
                        else if (projection.kind == AIRProjectionKind::Index) output << "[%" << projection.index << ']';
                    if (item.callee != InvalidSymbolId) output << " @" << item.callee;
                    for (const auto operand : item.operands) output << " %" << operand;
                    for (const auto destination : item.closureLocals) output << " ->$" << destination;
                    output << " scope=" << item.scope << '\n';
                }
                const auto& end = function.blocks[block].terminator;
                if (end.kind == AIRTerminatorKind::Jump) output << "  jump b" << end.target;
                else if (end.kind == AIRTerminatorKind::Branch) output << "  branch %" << end.value << " b" << end.target << " b" << end.alternative;
                else if (end.kind == AIRTerminatorKind::Return) { output << "  return"; if (end.value != InvalidAIRId) output << " %" << end.value; }
                else if (end.kind == AIRTerminatorKind::Trap) output << "  trap";
                else if (end.kind == AIRTerminatorKind::NoReturn) output << "  noreturn";
                else output << "  unreachable";
                output << '\n';
            }
            output << "}\n";
        }
        return output.str();
    }
    bool validBoxExtractionShape(const AIRFunction& function, const AIRInstruction& item, const TypeContext& types)
    {
        const auto& owner = types.get(item.boxOwner);
        const auto kind = types.get(item.boxElement).kind;
        if (item.opcode != AIROpcode::BoxExtract || owner.kind != TypeKind::Box || owner.element != item.boxElement
            || item.type != item.boxElement || kind == TypeKind::Error || kind == TypeKind::Void || kind == TypeKind::Never || kind == TypeKind::Dynamic
            || item.result >= function.valueTypes.size() || function.valueTypes[item.result] != item.type
            || item.operation != "into_inner" || !item.operands.empty() || item.place.type != item.boxOwner
            || item.place.local >= function.locals.size() || item.place.global != InvalidSymbolId || item.place.pointer != InvalidAIRId
            || !item.place.projections.empty() || item.callee != InvalidSymbolId || !item.dynamicTargets.empty()
            || item.dynamicSignature != InvalidTypeId || item.atomicElement != InvalidTypeId || item.syncElement != InvalidTypeId
            || item.taskElement != InvalidTypeId || item.taskCallable != InvalidTypeId || item.taskInvoke != InvalidSymbolId
            || !item.closureLocals.empty()) return false;
        const auto& storage = function.locals[item.place.local];
        return storage.name == "$box.extract" && storage.type == item.boxOwner && !storage.isParameter && !storage.isMutable
            && storage.symbol == InvalidSymbolId && !storage.source && storage.scope == item.scope && storage.scope < function.scopes.size();
    }
    bool verifyAIR(const AIRModule& module, const TypeContext& types, Diagnostics& diagnostics)
    {
        const auto before = diagnostics.size();
        for (const auto& function : module.functions)
        {
            auto fail = [&](SourceRange range, std::string message) { diagnostics.push_back({range, "Invalid AIR: " + std::move(message)}); };
            if (function.blocks.empty()) { fail({}, "function has no entry block."); continue; }
            if (types.get(function.result).kind == TypeKind::Error) fail({}, "function has an invalid result type.");
            std::unordered_set<AIRLocalId> parameterSlots;
            for (const auto slot : function.parameters)
                if (slot >= function.locals.size() || !parameterSlots.insert(slot).second) fail({}, "invalid or duplicate parameter local.");
            for (const auto& local : function.locals)
                if (local.scope >= function.scopes.size() || types.get(local.type).kind == TypeKind::Error) fail({}, "local has an invalid scope or type.");
            const auto blocks = function.blocks.size();
            std::vector<std::vector<AIRBlockId>> predecessors(blocks);
            std::vector<bool> reachable(blocks, false);
            std::vector<AIRBlockId> worklist{0};
            while (!worklist.empty())
            {
                const auto block = worklist.back(); worklist.pop_back();
                if (block >= blocks || reachable[block]) continue;
                reachable[block] = true;
                const auto& end = function.blocks[block].terminator;
                if (end.kind == AIRTerminatorKind::Jump || end.kind == AIRTerminatorKind::Branch) worklist.push_back(end.target);
                if (end.kind == AIRTerminatorKind::Branch) worklist.push_back(end.alternative);
            }
            for (AIRBlockId block = 0; block < blocks; ++block)
            {
                const auto& end = function.blocks[block].terminator;
                if (end.kind == AIRTerminatorKind::Jump || end.kind == AIRTerminatorKind::Branch)
                {
                    if (end.target < blocks) predecessors[end.target].push_back(block);
                    else fail(end.range, "branch target is out of range.");
                }
                if (end.kind == AIRTerminatorKind::Branch)
                {
                    if (end.alternative < blocks) predecessors[end.alternative].push_back(block);
                    else fail(end.range, "alternative branch target is out of range.");
                }
            }
            // 도달 가능한 선행 블록들의 교집합으로 지배 집합을 계산한다.
            // Compute dominators by intersecting reachable predecessor dominator sets.
            std::vector<std::vector<bool>> dominators(blocks, std::vector<bool>(blocks, true));
            dominators[0].assign(blocks, false); dominators[0][0] = true;
            bool changed = true;
            while (changed)
            {
                changed = false;
                for (AIRBlockId block = 1; block < blocks; ++block)
                {
                    if (!reachable[block]) continue;
                    std::vector<bool> next(blocks, true);
                    for (const auto predecessor : predecessors[block])
                        if (reachable[predecessor])
                            for (std::size_t index = 0; index < blocks; ++index) next[index] = next[index] && dominators[predecessor][index];
                    next[block] = true;
                    if (next != dominators[block]) { dominators[block] = std::move(next); changed = true; }
                }
            }
            std::unordered_map<AIRValueId, std::pair<AIRBlockId, std::size_t>> definitions;
            for (AIRBlockId block = 0; block < blocks; ++block)
                for (std::size_t index = 0; index < function.blocks[block].instructions.size(); ++index)
                {
                    const auto& item = function.blocks[block].instructions[index];
                    if (item.result != InvalidAIRId)
                    {
                        if (item.result >= function.valueTypes.size() || !definitions.emplace(item.result, std::pair{block, index}).second) fail(item.range, "invalid or duplicate value definition.");
                        else if (function.valueTypes[item.result] != item.type) fail(item.range, "value result type disagrees with its instruction.");
                    }
                }
            if (definitions.size() != function.valueTypes.size()) fail({}, "value table contains an undefined value.");
            auto use = [&](AIRValueId value, AIRBlockId block, std::size_t index, SourceRange range)
            {
                const auto definition = definitions.find(value);
                if (value >= function.valueTypes.size() || definition == definitions.end()) { fail(range, "operand has no valid definition."); return; }
                if (!reachable[block]) return;
                const auto [owner, order] = definition->second;
                if ((owner == block && order >= index) || (owner != block && !dominators[block][owner])) fail(range, "operand definition does not dominate its use.");
            };
            auto typeOf = [&](AIRValueId value) { return value < function.valueTypes.size() ? function.valueTypes[value] : InvalidTypeId; };
            for (AIRBlockId block = 0; block < blocks; ++block)
            {
                const auto& current = function.blocks[block];
                for (std::size_t index = 0; index < current.instructions.size(); ++index)
                {
                    const auto& item = current.instructions[index];
                    for (const auto value : item.operands) use(value, block, index, item.range);
                    if (item.opcode != AIROpcode::BoxExtract && (item.boxOwner != InvalidTypeId || item.boxElement != InvalidTypeId))
                        fail(item.range, "Box extraction metadata belongs only to a sealed BoxExtract operation.");
                    if (item.opcode == AIROpcode::BoxExtract)
                    {
                        const auto local = item.place.local;
                        bool valid = validBoxExtractionShape(function, item, types);
                        if (local < function.locals.size())
                        {
                            unsigned initializations = 0, extractions = 0, cleanupEntries = 0;
                            for (const auto& scope : function.scopes)
                                cleanupEntries += std::count(scope.locals.begin(), scope.locals.end(), local);
                            for (AIRBlockId b = 0; b < blocks; ++b)
                                for (std::size_t i = 0; i < function.blocks[b].instructions.size(); ++i)
                                {
                                    const auto& access = function.blocks[b].instructions[i];
                                    if (access.place.local != local) continue;
                                    if (access.opcode == AIROpcode::BoxExtract) ++extractions;
                                    else if (access.opcode == AIROpcode::Write)
                                    {
                                        ++initializations;
                                        valid = valid && b == block && i + 1 == index && access.place.type == item.boxOwner
                                            && access.place.global == InvalidSymbolId && access.place.pointer == InvalidAIRId && access.place.projections.empty()
                                            && access.operands.size() == 1 && typeOf(access.operands[0]) == item.boxOwner;
                                        if (access.operands.size() == 1)
                                            if (const auto producer = definitions.find(access.operands[0]); producer != definitions.end())
                                            {
                                                const auto& origin = function.blocks[producer->second.first].instructions[producer->second.second];
                                                valid = valid && origin.opcode != AIROpcode::Read && origin.opcode != AIROpcode::Constant;
                                            }
                                    }
                                    else if (access.opcode != AIROpcode::Drop) valid = false;
                                }
                            valid = valid && initializations == 1 && extractions == 1 && cleanupEntries == 1;
                        }
                        if (!valid) fail(item.range, "BoxExtract requires one initialized complete concrete Box owner and the exact sized payload result.");
                    }
                    if (item.opcode != AIROpcode::ClosureConsume && !item.closureLocals.empty())
                        fail(item.range, "capture destinations belong only to a sealed closure consume operation.");
                    if (item.opcode == AIROpcode::ClosureConsume)
                    {
                        const AIRRecord* environment = nullptr;
                        for (const auto& candidate : module.records) if (candidate.type == item.place.type) environment = &candidate;
                        const auto receiver = function.parameters.empty() ? InvalidAIRId : function.parameters.front();
                        const Node* marker = nullptr;
                        unsigned markers = 0, invocations = 0;
                        bool customDrop = false;
                        if (environment && environment->source)
                            for (const auto& member : environment->source->children)
                            {
                                if (member->kind == "CompilerClosure") { marker = member.get(); ++markers; }
                                if (member->kind == "Function" && member->value == "__invoke")
                                { if (member.get() == function.source) ++invocations; else invocations += 2; }
                                if (member->kind == "Implements")
                                    for (const auto& contract : member->children) if (contract->value == "Drop") customDrop = true;
                            }
                        bool valid = block == 0 && index == 1 && current.instructions.front().opcode == AIROpcode::ScopeBegin
                            && current.instructions.front().scope == 0 && item.scope == 0
                            && environment && !environment->isEnum && environment->source && environment->source->kind == "Struct"
                            && markers == 1 && marker && marker->value == "FnOnce" && !customDrop && invocations == 1
                            && function.source && function.source->kind == "Function" && function.source->value == "__invoke"
                            && function.name == "__invoke" && item.source == function.source
                            && receiver < function.locals.size() && item.place.local == receiver
                            && function.locals[receiver].type == item.place.type && function.locals[receiver].name == "self"
                            && function.locals[receiver].isParameter && function.locals[receiver].scope == 0
                            && types.get(item.place.type).kind == TypeKind::Nominal && item.type == item.place.type
                            && item.result == InvalidAIRId && item.operation == "consume" && item.operands.empty()
                            && item.place.pointer == InvalidAIRId && item.place.global == InvalidSymbolId && item.place.projections.empty()
                            && item.callee == InvalidSymbolId && item.dynamicTargets.empty() && item.dynamicSignature == InvalidTypeId
                            && item.atomicElement == InvalidTypeId && item.syncElement == InvalidTypeId
                            && item.taskElement == InvalidTypeId && item.taskCallable == InvalidTypeId && item.taskInvoke == InvalidSymbolId
                            && !function.scopes.empty() && function.scopes[0].parent == InvalidAIRId
                            && environment->fields.size() == environment->fieldTypes.size()
                            && item.closureLocals.size() == environment->fieldTypes.size();
                        std::unordered_set<AIRLocalId> destinations;
                        if (valid)
                        {
                            const auto& cleanup = function.scopes[0].locals;
                            valid = cleanup.size() > item.closureLocals.size() && cleanup.front() == receiver;
                            for (std::size_t field = 0; field < item.closureLocals.size(); ++field)
                            {
                                const auto destination = item.closureLocals[field];
                                valid = valid && destination < function.locals.size() && destination != receiver
                                    && destinations.insert(destination).second && !parameterSlots.contains(destination)
                                    && field + 1 < cleanup.size() && cleanup[field + 1] == destination;
                                if (destination >= function.locals.size()) continue;
                                const auto& local = function.locals[destination];
                                valid = valid && local.type == environment->fieldTypes[field] && !local.isParameter && local.scope == 0
                                    && local.isMutable == function.locals[receiver].isMutable
                                    && local.name == "$capturelocal." + std::to_string(field);
                                std::size_t occurrences = 0;
                                for (const auto& scope : function.scopes)
                                    occurrences += std::count(scope.locals.begin(), scope.locals.end(), destination);
                                valid = valid && occurrences == 1;
                            }
                        }
                        if (!valid) fail(item.range, "closure consume requires the exact complete owned FnOnce invocation environment and fresh capture locals.");
                    }
                    if (item.opcode == AIROpcode::MemoryIntrinsic)
                    {
                        if ((item.operation != "replace" && item.operation != "swap") || item.operands.size() != 2
                            || item.result == InvalidAIRId || item.place.local != InvalidAIRId
                            || item.place.global != InvalidSymbolId || item.place.pointer != InvalidAIRId
                            || !item.place.projections.empty() || item.callee != InvalidSymbolId)
                            fail(item.range, "invalid sealed memory operation shape.");
                        else
                        {
                            const auto& target = types.get(typeOf(item.operands[0]));
                            if (target.kind != TypeKind::Reference || !target.isMutable || target.element == InvalidTypeId)
                                fail(item.range, "memory target requires an exclusive reference.");
                            const auto elementKind = types.get(target.element).kind;
                            if (elementKind == TypeKind::Error || elementKind == TypeKind::Void || elementKind == TypeKind::Never
                                || elementKind == TypeKind::Dynamic || elementKind == TypeKind::Function)
                                fail(item.range, "memory target element must be concrete storable data.");
                            if (item.operation == "replace")
                            {
                                if (typeOf(item.operands[1]) != target.element || item.type != target.element)
                                    fail(item.range, "memory replacement and result must match the target element type.");
                            }
                            else if (typeOf(item.operands[1]) != typeOf(item.operands[0]) || types.get(item.type).kind != TypeKind::Unit)
                                fail(item.range, "memory swap requires equal exclusive target types and a unit result.");
                        }
                    }
                    if (item.place.local != InvalidAIRId && item.place.local >= function.locals.size()) fail(item.range, "local place is out of range.");
                    if (item.place.pointer != InvalidAIRId)
                    {
                        use(item.place.pointer, block, index, item.range);
                        if (types.get(typeOf(item.place.pointer)).kind != TypeKind::Reference) fail(item.range, "pointer place base is not a reference.");
                    }
                    for (const auto& projection : item.place.projections)
                        if (projection.kind == AIRProjectionKind::Index) use(projection.index, block, index, item.range);
                    const bool hasPlace = item.opcode == AIROpcode::Read || item.opcode == AIROpcode::Move
                        || item.opcode == AIROpcode::Write || item.opcode == AIROpcode::BorrowShared
                        || item.opcode == AIROpcode::BorrowMut || item.opcode == AIROpcode::Drop || item.opcode == AIROpcode::EnumTag || item.opcode == AIROpcode::SliceConstruct
                        || item.opcode == AIROpcode::VectorAppend || item.opcode == AIROpcode::VectorReserve
                        || item.opcode == AIROpcode::VectorTake || item.opcode == AIROpcode::VectorTruncate || item.opcode == AIROpcode::VectorInsert
                        || item.opcode == AIROpcode::ClosureConsume || item.opcode == AIROpcode::BoxExtract;
                    if (hasPlace)
                    {
                        const unsigned bases = (item.place.local != InvalidAIRId) + (item.place.global != InvalidSymbolId) + (item.place.pointer != InvalidAIRId);
                        if (bases != 1) fail(item.range, "place must have exactly one base.");
                        TypeId currentType = InvalidTypeId;
                        if (item.place.local < function.locals.size()) currentType = function.locals[item.place.local].type;
                        else if (item.place.pointer != InvalidAIRId) currentType = types.get(typeOf(item.place.pointer)).element;
                        else if (item.place.global != InvalidSymbolId)
                            for (const auto& global : module.globals) if (global.symbol == item.place.global) currentType = global.type;
                        if (currentType == InvalidTypeId) fail(item.range, "place base has no storage type.");
                        for (const auto& projection : item.place.projections)
                        {
                            TypeId projected = InvalidTypeId;
                            if (projection.kind == AIRProjectionKind::Index)
                            {
                                const auto& array = types.get(currentType);
                                if ((array.kind != TypeKind::Array && array.kind != TypeKind::Vector && array.kind != TypeKind::Slice) || array.extent != projection.extent
                                    || types.get(typeOf(projection.index)).kind != TypeKind::Integer) fail(item.range, "invalid array projection.");
                                else projected = array.element;
                            }
                            else if (projection.kind == AIRProjectionKind::Dereference)
                            {
                                if (types.get(currentType).kind != TypeKind::Reference) fail(item.range, "dereference projection requires a reference.");
                                else projected = types.get(currentType).element;
                            }
                            else
                            {
                                const AIRRecord* record = nullptr;
                                for (const auto& candidate : module.records) if (candidate.type == currentType) record = &candidate;
                                if (types.get(currentType).kind == TypeKind::Tuple && projection.kind == AIRProjectionKind::Field
                                    && projection.field < types.get(currentType).parameters.size()) projected = types.get(currentType).parameters[projection.field];
                                else if (record && projection.kind == AIRProjectionKind::Field && !record->isEnum && projection.field < record->fieldTypes.size()) projected = record->fieldTypes[projection.field];
                                else if (record && projection.kind == AIRProjectionKind::EnumField && record->isEnum)
                                    for (const auto& variant : record->variants)
                                        if (variant.variant == projection.variant && projection.field < variant.fieldTypes.size()) projected = variant.fieldTypes[projection.field];
                                if (projected == InvalidTypeId) fail(item.range, "invalid record projection.");
                            }
                            if (projected != projection.type) fail(item.range, "projection type differs from storage field.");
                            currentType = projected;
                        }
                        if (currentType != item.place.type) fail(item.range, "place type differs from projected storage.");
                    }
                    if (item.opcode == AIROpcode::VectorTake || item.opcode == AIROpcode::VectorTruncate || item.opcode == AIROpcode::VectorInsert)
                    {
                        const auto& vector = types.get(item.place.type);
                        const auto& receiver = types.get(typeOf(item.place.pointer));
                        const auto count = item.opcode == AIROpcode::VectorInsert ? 2u : 1u;
                        const bool operation = item.opcode == AIROpcode::VectorTake
                            ? (item.operation == "pop" || item.operation == "remove" || item.operation == "swap_remove")
                            : item.opcode == AIROpcode::VectorTruncate ? (item.operation == "clear" || item.operation == "truncate") : item.operation == "insert";
                        if (!operation || item.callee != InvalidSymbolId || vector.kind != TypeKind::Vector
                            || receiver.kind != TypeKind::Reference || !receiver.isMutable || receiver.element != item.place.type
                            || item.place.local != InvalidAIRId || item.place.global != InvalidSymbolId || !item.place.projections.empty()
                            || item.operands.size() != count || item.result == InvalidAIRId)
                            fail(item.range, "Vector mutation requires one exclusive vector, a known operation, and exact operands.");
                        else
                        {
                            if (typeOf(item.operands[0]) != types.builtin("usize")) fail(item.range, "Vector mutation index or length must be usize.");
                            if (item.opcode == AIROpcode::VectorTake ? item.type != vector.element : types.get(item.type).kind != TypeKind::Unit)
                                fail(item.range, "Vector mutation result does not match its operation.");
                            if (item.opcode == AIROpcode::VectorInsert && typeOf(item.operands[1]) != vector.element)
                                fail(item.range, "Vector insertion input must match its element type.");
                        }
                    }
                    if (item.operation == "enum.extract")
                    {
                        if (item.opcode != AIROpcode::Move || item.place.pointer != InvalidAIRId
                            || item.place.local >= function.locals.size()
                            || function.locals[item.place.local].name != "$enumselector"
                            || item.place.projections.size() != 1
                            || item.place.projections[0].kind != AIRProjectionKind::EnumField)
                            fail(item.range, "enum extraction requires one local consuming-selector payload projection.");
                    }
                    if (item.opcode == AIROpcode::Aggregate)
                    {
                        std::vector<TypeId> fields;
                        const auto& aggregate = types.get(item.type);
                        if (aggregate.kind == TypeKind::Array) fields.assign(aggregate.extent, aggregate.element);
                        else if (aggregate.kind == TypeKind::Tuple) fields = aggregate.parameters;
                        else for (const auto& record : module.records) if (record.type == item.type && !record.isEnum) fields = record.fieldTypes;
                        if (fields.size() != item.operands.size()) fail(item.range, "aggregate field count differs from its type.");
                        else for (std::size_t field = 0; field < fields.size(); ++field)
                            if (fields[field] != typeOf(item.operands[field])) fail(item.range, "aggregate field type mismatch.");
                    }
                    if (item.opcode == AIROpcode::EnumConstruct)
                    {
                        const EnumVariantInfo* variant = nullptr;
                        for (const auto& record : module.records) for (const auto& candidate : record.variants) if (candidate.variant == item.callee) variant = &candidate;
                        if (!variant || variant->type != item.type || variant->fieldTypes.size() != item.operands.size()) fail(item.range, "enum construction differs from variant storage.");
                        else for (std::size_t field = 0; field < item.operands.size(); ++field)
                            if (variant->fieldTypes[field] != typeOf(item.operands[field])) fail(item.range, "enum payload type mismatch.");
                    }
                    if (item.opcode == AIROpcode::CharScalar)
                    {
                        const bool from = item.operation == "Char.from_u32";
                        const auto input = item.operands.size() == 1 ? typeOf(item.operands[0]) : InvalidTypeId;
                        bool valid = (from || item.operation == "Char.to_u32") && item.result != InvalidAIRId
                            && item.operands.size() == 1 && input == types.builtin(from ? "u32" : "char")
                            && item.callee == InvalidSymbolId && item.dynamicSignature == InvalidTypeId && item.dynamicTargets.empty()
                            && item.place.local == InvalidAIRId && item.place.global == InvalidSymbolId && item.place.pointer == InvalidAIRId
                            && item.place.type == InvalidTypeId && item.place.projections.empty()
                            && item.atomicElement == InvalidTypeId && item.syncElement == InvalidTypeId
                            && item.taskElement == InvalidTypeId && item.taskCallable == InvalidTypeId && item.taskInvoke == InvalidSymbolId;
                        if (!from) valid = valid && item.type == types.builtin("u32");
                        else
                        {
                            const auto recordOf = [&](TypeId type) -> const AIRRecord*
                            { for (const auto& record : module.records) if (record.type == type) return &record; return nullptr; };
                            const auto* result = recordOf(item.type);
                            const auto variantAt = [](const AIRRecord* record,std::size_t tag) -> const EnumVariantInfo*
                            {
                                if (!record) return nullptr;
                                const EnumVariantInfo* found = nullptr;
                                for (const auto& variant : record->variants) if (variant.tag == tag)
                                { if (found) return nullptr; found = &variant; }
                                return found;
                            };
                            const auto* ok = variantAt(result,0), *failure = variantAt(result,1);
                            const auto errorType = failure && failure->fieldTypes.size() == 1 ? failure->fieldTypes[0] : InvalidTypeId;
                            const auto* errors = recordOf(errorType);
                            const auto* rejected = variantAt(errors,0);
                            valid = valid && types.get(item.type).kind == TypeKind::Nominal && result && result->isEnum
                                && result->fieldTypes.empty() && result->variants.size() == 2
                                && ok && ok->type == item.type && ok->fields.size() == 1 && ok->fieldTypes == std::vector<TypeId>{types.builtin("char")}
                                && failure && failure->type == item.type && failure->fields.size() == 1 && failure->fieldTypes.size() == 1
                                && types.get(errorType).kind == TypeKind::Nominal && types.get(errorType).name == "CharError"
                                && errors && errors->isEnum && errors->fieldTypes.empty() && errors->variants.size() == 1
                                && rejected && rejected->type == errorType && rejected->fields.size() == 1
                                && rejected->fieldTypes == std::vector<TypeId>{types.builtin("u32")};
                        }
                        if (!valid) fail(item.range,"Char scalar conversion requires a closed exact-domain operation and canonical char/u32 Result error shape without unrelated metadata.");
                    }
                    if (item.opcode == AIROpcode::RangeInspect)
                    {
                        const auto* operation = rangeInspectOperation(item.operation);
                        const auto input = item.operands.size() == 1 ? typeOf(item.operands[0]) : InvalidTypeId;
                        const auto& reference = types.get(input);
                        const auto& descriptor = types.get(reference.element);
                        const auto& element = types.get(descriptor.element);
                        const auto expected = !operation ? InvalidTypeId : operation->result == RangeInspectResult::Element ? descriptor.element
                            : operation->result == RangeInspectResult::Magnitude ? types.builtin(element.name == "isize" || element.name == "usize" ? "usize" : "u" + std::to_string(element.bits))
                            : types.builtin("bool");
                        if (!operation || item.operands.size() != 1 || item.result == InvalidAIRId || reference.kind != TypeKind::Reference
                            || descriptor.kind != TypeKind::Range || element.kind != TypeKind::Integer
                            || (element.bits != 8 && element.bits != 16 && element.bits != 32 && element.bits != 64 && element.bits != 128)
                            || item.type != expected || item.callee != InvalidSymbolId || item.dynamicSignature != InvalidTypeId || !item.dynamicTargets.empty()
                            || item.place.local != InvalidAIRId || item.place.global != InvalidSymbolId || item.place.pointer != InvalidAIRId
                            || item.place.type != InvalidTypeId || !item.place.projections.empty()
                            || item.atomicElement != InvalidTypeId || item.syncElement != InvalidTypeId || item.taskElement != InvalidTypeId
                            || item.taskCallable != InvalidTypeId || item.taskInvoke != InvalidSymbolId)
                            fail(item.range,"Range inspection requires a known pure descriptor operation, explicit integer Range borrow, exact scalar result and no unrelated metadata.");
                    }
                    if (item.opcode == AIROpcode::NumericStatus || item.opcode == AIROpcode::NumericConvert)
                    {
                        if (item.operation == "Float.to_bits" || item.operation == "Float.from_bits")
                        {
                            const bool toBits = item.operation == "Float.to_bits";
                            const auto input = item.operands.size() == 1 ? typeOf(item.operands[0]) : InvalidTypeId;
                            const auto& format = types.get(toBits ? input : item.type);
                            const auto integer = toBits ? item.type : input;
                            const auto counterpart = format.kind == TypeKind::Float && (format.bits == 16 || format.bits == 32 || format.bits == 64)
                                ? types.builtin("u" + std::to_string(format.bits)) : InvalidTypeId;
                            if (item.opcode != AIROpcode::NumericConvert || item.result == InvalidAIRId || item.operands.size() != 1
                                || format.kind != TypeKind::Float || counterpart == InvalidTypeId || integer != counterpart
                                || item.callee != InvalidSymbolId || item.dynamicSignature != InvalidTypeId || !item.dynamicTargets.empty()
                                || item.place.local != InvalidAIRId || item.place.global != InvalidSymbolId || item.place.pointer != InvalidAIRId
                                || item.place.type != InvalidTypeId || !item.place.projections.empty()
                                || item.atomicElement != InvalidTypeId || item.syncElement != InvalidTypeId
                                || item.taskElement != InvalidTypeId || item.taskCallable != InvalidTypeId || item.taskInvoke != InvalidSymbolId)
                                fail(item.range,"Floating-point bits require a pure NumericConvert with one exact float/fixed-unsigned counterpart and no status, callback or storage metadata.");
                        }
                        else
                        {
                            const bool status = item.opcode == AIROpcode::NumericStatus;
                            const auto destination = status ? item.dynamicSignature : item.type;
                            const auto source = item.operands.size() == 1 ? typeOf(item.operands[0]) : InvalidTypeId;
                            const auto sourceKind = types.get(source).kind, destinationKind = types.get(destination).kind;
                            const bool integerSource = sourceKind == TypeKind::Integer, integerDestination = destinationKind == TypeKind::Integer;
                            const bool floatSource = sourceKind == TypeKind::Float, floatDestination = destinationKind == TypeKind::Float;
                            const bool domain = item.operation == "Integer.convert" ? integerSource && integerDestination
                                : item.operation == "Float.convert" || item.operation == "Float.convert_exact" ? (integerSource || floatSource) && floatDestination
                                : item.operation == "Float.to_integer_exact" ? floatSource && integerDestination
                                : item.operation == "return.bool_to_integer" ? !status && sourceKind == TypeKind::Bool && integerDestination : false;
                            if (!domain || item.operands.size() != 1 || item.result == InvalidAIRId
                                || (status ? item.type != types.builtin("i32") : item.dynamicSignature != InvalidTypeId)
                                || item.callee != InvalidSymbolId || !item.dynamicTargets.empty()
                                || item.place.local != InvalidAIRId || item.place.global != InvalidSymbolId || item.place.pointer != InvalidAIRId
                                || item.place.type != InvalidTypeId || !item.place.projections.empty()
                                || item.atomicElement != InvalidTypeId || item.syncElement != InvalidTypeId || item.taskElement != InvalidTypeId
                                || item.taskCallable != InvalidTypeId || item.taskInvoke != InvalidSymbolId)
                                fail(item.range,"Numeric conversion requires a known closed operation, exact numeric domains and status/value metadata.");
                        }
                    }
                    if (item.opcode == AIROpcode::SliceSubview)
                    {
                        if (item.operands.size() != 3 || types.get(item.type).kind != TypeKind::Slice)
                            fail(item.range, "slice subview requires one view and two usize bounds.");
                        else if (typeOf(item.operands[0]) != item.type || typeOf(item.operands[1]) != types.builtin("usize")
                            || typeOf(item.operands[2]) != types.builtin("usize")) fail(item.range, "slice subview operand type mismatch.");
                    }
                    if (item.opcode == AIROpcode::OsIntrinsic)
                    {
                        const auto* operation = osOperation(item.operation);
                        if (!operation || item.callee == InvalidSymbolId || item.operands.size() != operation->parameterCount)
                            fail(item.range, "OS operation is absent from the closed native registry.");
                        else
                        {
                            const auto matches = [&](TypeId id, OsValueKind wanted)
                            {
                                const auto& type = types.get(id);
                                switch (wanted)
                                {
                                    case OsValueKind::U64: return id == types.builtin("u64");
                                    case OsValueKind::U32: return id == types.builtin("u32");
                                    case OsValueKind::I64: return id == types.builtin("i64");
                                    case OsValueKind::Usize: return id == types.builtin("usize");
                                    case OsValueKind::Text: return type.kind == TypeKind::StringView;
                                    case OsValueKind::Arguments: return type.kind == TypeKind::Slice && !type.isMutable
                                        && type.element == types.builtin("string_view");
                                    case OsValueKind::Bytes: return type.kind == TypeKind::Slice && !type.isMutable
                                        && type.element == types.builtin("byte");
                                    case OsValueKind::MutableBytesLoan:
                                    {
                                        const auto& view = types.get(type.element);
                                        return type.kind == TypeKind::Reference && type.isMutable && view.kind == TypeKind::Slice
                                            && view.isMutable && view.element == types.builtin("byte");
                                    }
                                }
                                return false;
                            };
                            for (std::size_t index = 0; index < item.operands.size(); ++index)
                                if (!matches(typeOf(item.operands[index]), operation->parameters[index]))
                                    fail(item.range, "OS operation requires its exact typed handle and bounded byte views.");
                            const auto& returned = types.get(item.type);
                            if (operation->unitResult)
                            { if (returned.kind != TypeKind::Unit) fail(item.range, "OS cleanup must return unit."); }
                            else
                            {
                                bool payload = returned.kind == TypeKind::Tuple && returned.parameters.size() == 4;
                                if (payload)
                                {
                                    payload = returned.parameters[0] == types.builtin("i32")
                                        && returned.parameters[2] == types.builtin("i32") && returned.parameters[3] == types.builtin("i32");
                                    const auto id = returned.parameters[1];
                                    const auto& value = types.get(id);
                                    if (operation->tokenResult) payload = payload && id == types.builtin("u64");
                                    else switch (operation->payload)
                                    {
                                        case OsResultKind::Progress: payload = payload && id == types.builtin("usize"); break;
                                        case OsResultKind::Text: payload = payload && value.kind == TypeKind::String; break;
                                        case OsResultKind::SignedTime: payload = payload && id == types.builtin("i64"); break;
                                        case OsResultKind::Exit: payload = payload && value.kind == TypeKind::Tuple
                                            && value.parameters == std::vector<TypeId>{types.builtin("i32"), types.builtin("i32")}; break;
                                        case OsResultKind::Metadata: payload = payload && value.kind == TypeKind::Tuple
                                            && value.parameters == std::vector<TypeId>{types.builtin("usize"), types.builtin("i64"),
                                                types.builtin("u32"), types.builtin("u32")}; break;
                                    }
                                }
                                if (!payload) fail(item.range, "OS status, owning payload and native-error tuple shape mismatch.");
                            }
                        }
                    }
                    if (item.opcode == AIROpcode::ParallelIntrinsic)
                    {
                        if (item.operation == "worker_count")
                        {
                            if (!item.operands.empty() || item.type != types.builtin("usize")) fail(item.range, "parallel worker_count signature mismatch.");
                        }
                        else if (item.operation != "for_each" || item.operands.size() != 1 || item.type != types.builtin("unit"))
                            fail(item.range, "parallel for_each signature mismatch.");
                        else
                        {
                            const auto& slice = types.get(typeOf(item.operands[0]));
                            const AIRFunction* worker = nullptr;
                            for (const auto& candidate : module.functions) if (candidate.symbol == item.callee) worker = &candidate;
                            if (slice.kind != TypeKind::Slice || !slice.isMutable || !worker || worker->parameters.size() != 1
                                || worker->result != types.builtin("void")) fail(item.range, "parallel requires an exclusive slice and a static void worker.");
                            else
                            {
                                const auto& reference = types.get(worker->locals[worker->parameters[0]].type);
                                if (reference.kind != TypeKind::Reference || !reference.isMutable || reference.element != slice.element)
                                    fail(item.range, "parallel worker element borrow type mismatch.");
                            }
                        }
                    }
                    if (item.opcode == AIROpcode::TaskIntrinsic)
                    {
                        const auto* operation = taskOperation(item.operation);
                        const auto recordOf = [&](TypeId type) -> const AIRRecord*
                        { for (const auto& record : module.records) if (record.type == type) return &record; return nullptr; };
                        const auto structure = [&](TypeId type, std::string_view name, const std::vector<TypeId>& fields)
                        { const auto* record = recordOf(type); return record && !record->isEnum && types.get(type).kind == TypeKind::Nominal && types.get(type).name == name && record->fieldTypes == fields; };
                        const auto enumeration = [&](TypeId type, std::string_view name, const std::vector<std::vector<TypeId>>& fields)
                        {
                            const auto* record = recordOf(type);
                            if (!record || !record->isEnum || types.get(type).kind != TypeKind::Nominal || types.get(type).name != name || record->variants.size() != fields.size()) return false;
                            std::vector<bool> seen(fields.size());
                            for (const auto& variant : record->variants)
                            { if (variant.tag >= fields.size() || seen[variant.tag] || variant.fieldTypes != fields[variant.tag]) return false; seen[variant.tag] = true; }
                            return true;
                        };
                        const auto carrier = [&](TypeId type, bool optional) -> std::pair<TypeId,TypeId>
                        {
                            const auto* record = recordOf(type); TypeId success = InvalidTypeId, failure = InvalidTypeId;
                            if (!record || !record->isEnum || record->variants.size() != 2) return {success,failure};
                            for (const auto& variant : record->variants)
                                if (variant.tag == 0 && variant.fieldTypes.size() == 1 && success == InvalidTypeId) success = variant.fieldTypes[0];
                                else if (variant.tag == 1 && variant.fieldTypes.size() == (optional ? 0u : 1u) && failure == InvalidTypeId) failure = optional ? types.builtin("unit") : variant.fieldTypes[0];
                                else return {InvalidTypeId,InvalidTypeId};
                            return {success,failure};
                        };
                        const auto taskError = [&](TypeId type)
                        { return enumeration(type,"TaskError",{{},{},{},{types.builtin("i32"),types.builtin("i32")},{types.builtin("i32")},{},{},{},{},{},{}}); };
                        const auto payload = item.taskElement, callable = item.taskCallable;
                        const auto& element = types.get(payload);
                        bool valid = operation && item.operands.size() == operation->operands && item.result != InvalidAIRId
                            && payload != InvalidTypeId && callable != InvalidTypeId && element.kind != TypeKind::Error && element.kind != TypeKind::Void
                            && element.kind != TypeKind::Never && element.kind != TypeKind::Dynamic
                            && item.atomicElement == InvalidTypeId && item.syncElement == InvalidTypeId && item.callee == InvalidSymbolId
                            && item.place.local == InvalidAIRId && item.place.pointer == InvalidAIRId && item.place.global == InvalidSymbolId
                            && item.place.type == InvalidTypeId && item.place.projections.empty() && item.dynamicTargets.empty() && item.dynamicSignature == InvalidTypeId;
                        if (operation && item.operands.size() == operation->operands)
                        {
                            const bool scopeAction = !operation->worker && operation->preparation == TaskPreparation::None && (operation->access == TaskAccess::None || operation->access == TaskAccess::ScopeValue || operation->access == TaskAccess::ScopeShared);
                            if (scopeAction || operation->access == TaskAccess::CancellationShared) valid = valid && payload == types.builtin("unit");
                            if (!operation->worker) valid = valid && callable == types.builtin("unit") && item.taskInvoke == InvalidSymbolId;
                            for (std::size_t i = 0; i < item.operands.size(); ++i)
                            {
                                const auto actual = typeOf(item.operands[i]); const auto& reference = types.get(actual);
                                if (i == 0 && operation->access != TaskAccess::None)
                                {
                                    const bool value = operation->access == TaskAccess::TaskValue || operation->access == TaskAccess::ScopeValue;
                                    const auto& owner = types.get(value ? actual : reference.element);
                                    const bool scope = operation->access == TaskAccess::ScopeValue || operation->access == TaskAccess::ScopeShared;
                                    valid = valid && (value || (reference.kind == TypeKind::Reference && (operation->access != TaskAccess::TaskExclusive || reference.isMutable)))
                                        && owner.kind == (operation->access == TaskAccess::CancellationShared ? TypeKind::CancellationToken : scope ? TypeKind::TaskScope : TypeKind::Task)
                                        && (scope || operation->access == TaskAccess::CancellationShared || owner.element == payload);
                                }
                                else if (operation->worker) valid = valid && actual == callable;
                                else if (operation->preparation == TaskPreparation::Ready) valid = valid && actual == payload;
                                else if (operation->result == TaskResult::Scope)
                                    valid = valid && (i == 0 ? reference.kind == TypeKind::Reference && structure(reference.element,"Executor",{types.builtin("u8")}) : structure(actual,"ScopeOptions",{types.builtin("u8")}));
                                else valid = valid && structure(actual,"Duration",{types.builtin("u64")});
                            }
                            auto output = item.type;
                            TypeId failure = InvalidTypeId;
                            if (taskFallible(operation->result))
                            {
                                const auto result = carrier(output,false); output = result.first; failure = result.second;
                                if (operation->result == TaskResult::Scope || operation->result == TaskResult::Spawn)
                                    valid = valid && enumeration(failure,"SpawnError",{{},{},{},{types.builtin("i32"),types.builtin("i32")}});
                                else if (operation->result == TaskResult::CheckCancel) valid = valid && enumeration(failure,"Cancelled",{{}});
                                else if (operation->result != TaskResult::Close && operation->result != TaskResult::CloseAdmission) valid = valid && taskError(failure);
                            }
                            const auto& value = types.get(output);
                            switch (operation->result)
                            {
                                case TaskResult::Scope: valid = valid && value.kind == TypeKind::TaskScope; break;
                                case TaskResult::Spawn: valid = valid && value.kind == TypeKind::Task && value.element == payload; break;
                                case TaskResult::Join: valid = valid && output == payload; break;
                                case TaskResult::TryTake: { const auto optional = carrier(output,true); valid = valid && optional.first == payload && optional.second == types.builtin("unit"); break; }
                                case TaskResult::Discard: valid = valid && output == types.builtin("unit"); break;
                                case TaskResult::Status:
                                {
                                    const auto* record = recordOf(output);
                                    valid = valid && record && record->fieldTypes.size() == 2;
                                    if (record && record->fieldTypes.size() == 2) valid = valid && structure(output,"TaskStatus",record->fieldTypes)
                                        && enumeration(record->fieldTypes[0],"TaskPhase",{{},{},{},{},{},{},{},{}}) && record->fieldTypes[1] == types.builtin("bool");
                                    break;
                                }
                                case TaskResult::Boolean: valid = valid && output == types.builtin("bool"); break;
                                case TaskResult::Token: valid = valid && value.kind == TypeKind::CancellationToken && value.name == "CancellationToken"; break;
                                case TaskResult::Executor: valid = valid && enumeration(output,"ExecutorId",{{}}); break;
                                case TaskResult::Identity:
                                {
                                    const auto* identity = recordOf(output);
                                    const Node* serial = nullptr;
                                    unsigned markers = 0, fields = 0;
                                    bool markerValid = true;
                                    if (identity && identity->source) for (const auto& member : identity->source->children)
                                    {
                                        if (member->kind == "CompilerTaskId") { ++markers; markerValid = markerValid && member->value.empty() && member->children.empty(); }
                                        if (member->kind == "Variable") { serial = member.get(); ++fields; }
                                    }
                                    bool source = serial && serial->value == "serial" && serial->children.size() == 2
                                        && serial->children[0]->kind == "Type" && serial->children[0]->value == "u64" && serial->children[0]->children.empty()
                                        && serial->children[1]->kind == "ArraySuffixes" && serial->children[1]->children.empty();
                                    if (serial) for (const auto& member : serial->children)
                                        if (member->kind == "Modifier" && (member->value == "public" || member->value == "mut" || member->value == "persist")) source = false;
                                    valid = valid && structure(output,"TaskId",{types.builtin("u64")})
                                        && identity && identity->source && identity->source->kind == "Struct" && identity->source->value == "TaskId"
                                        && markers == 1 && markerValid && fields == 1 && source && identity->fields.size() == 1
                                        && identity->fields[0] != InvalidSymbolId && identity->variants.empty();
                                    break;
                                }
                                case TaskResult::ActiveCount: valid = valid && output == types.builtin("usize"); break;
                                case TaskResult::ScopeState: valid = valid && enumeration(output,"ScopeStatus",{{},{}}); break;
                                case TaskResult::Completion: case TaskResult::CheckCancel: valid = valid && output == types.builtin("unit"); break;
                                case TaskResult::Wait: valid = valid && enumeration(output,"WaitStatus",{{},{}}); break;
                                case TaskResult::Request: valid = valid && enumeration(output,"TaskCancelRequest",{{},{},{}}); break;
                                case TaskResult::Close:
                                case TaskResult::CloseAdmission:
                                {
                                    const auto* error = recordOf(failure);
                                    const EnumVariantInfo* retry = nullptr;
                                    const EnumVariantInfo* borrowed = nullptr;
                                    if (error) for (const auto& variant : error->variants)
                                    { if (variant.tag == 0) retry = &variant; else if (variant.tag == 1) borrowed = &variant; }
                                    const auto actual = typeOf(item.operands[0]);
                                    const auto scope = operation->result == TaskResult::Close ? actual : types.get(actual).element;
                                    valid = valid && (operation->result == TaskResult::Close
                                        ? structure(output,"ScopeReport",std::vector<TypeId>(7,types.builtin("u64"))) : output == types.builtin("unit"));
                                    valid = valid && retry && borrowed && retry->type == failure && borrowed->type == failure
                                        && retry->fields.size() == 2 && borrowed->fields.size() == 1
                                        && retry->fieldTypes.size() == 2 && types.get(scope).kind == TypeKind::TaskScope;
                                    if (retry && borrowed)
                                    {
                                        std::unordered_set<SymbolId> fieldIds;
                                        for (const auto* variant : {retry,borrowed}) for (const auto field : variant->fields)
                                            valid = valid && field != InvalidSymbolId && fieldIds.insert(field).second;
                                    }
                                    if (retry && retry->fieldTypes.size() == 2)
                                    {
                                        const auto reason = retry->fieldTypes[1];
                                        valid = valid && retry->fieldTypes[0] == scope && taskError(reason)
                                            && enumeration(failure,"ScopeError",{{scope,reason},{reason}});
                                    }
                                    break;
                                }
                            }
                            if (operation->worker)
                            {
                                const auto* record = recordOf(callable); const AIRFunction* invoke = nullptr;
                                for (const auto& candidate : module.functions) if (candidate.symbol == item.taskInvoke) invoke = &candidate;
                                const Node* closure = nullptr;
                                if (record && record->source) for (const auto& marker : record->source->children) if (marker->kind == "CompilerClosure") closure = marker.get();
                                valid = valid && record && !record->isEnum && closure && invoke && invoke->source && invoke->source->value == "__invoke"
                                    && invoke->result == payload && invoke->parameters.size() == 1 && types.get(callable).kind == TypeKind::Nominal;
                                if (closure && invoke && invoke->source && record && record->source && invoke->parameters.size() == 1)
                                {
                                    valid = valid && std::any_of(record->source->children.begin(),record->source->children.end(),[&](const auto& member){return member.get() == invoke->source;});
                                    const auto parameter = invoke->parameters[0];
                                    if (parameter >= invoke->locals.size()) valid = false;
                                    else
                                    {
                                        const auto self = invoke->locals[parameter].type; const auto& borrow = types.get(self);
                                        valid = valid && (closure->value == "FnOnce" ? self == callable : (closure->value == "Fn" || closure->value == "FnMut")
                                            && borrow.kind == TypeKind::Reference && borrow.element == callable && borrow.isMutable == (closure->value == "FnMut"));
                                    }
                                }
                            }
                        }
                        if (!valid) fail(item.range,"Task operation, sealed scope/owner, callable identity or exact lifecycle carrier shape mismatch.");
                    }
                    if (item.opcode == AIROpcode::CancellationIntrinsic)
                    {
                        const auto* operation = cancellationOperation(item.operation);
                        const auto recordOf = [&](TypeId type) -> const AIRRecord*
                        { for (const auto& record : module.records) if (record.type == type) return &record; return nullptr; };
                        const auto twoEmptyVariants = [&](TypeId type, std::string_view name)
                        {
                            const auto* record = recordOf(type);
                            if (!record || !record->isEnum || types.get(type).kind != TypeKind::Nominal || types.get(type).name != name || record->variants.size() != 2) return false;
                            bool zero = false, one = false;
                            for (const auto& variant : record->variants)
                            {
                                if (!variant.fieldTypes.empty()) return false;
                                if (variant.tag == 0 && !zero) zero = true;
                                else if (variant.tag == 1 && !one) one = true;
                                else return false;
                            }
                            return zero && one;
                        };
                        bool valid = operation && item.operands.size() == operation->operands && item.result != InvalidAIRId
                            && item.atomicElement == InvalidTypeId && item.syncElement == InvalidTypeId
                            && item.callee == InvalidSymbolId && item.place.local == InvalidAIRId && item.place.pointer == InvalidAIRId
                            && item.place.global == InvalidSymbolId && item.place.type == InvalidTypeId && item.place.projections.empty()
                            && item.dynamicTargets.empty() && item.dynamicSignature == InvalidTypeId;
                        if (operation && item.operands.size() == operation->operands)
                        {
                            if (operation->operands == 1)
                            {
                                const auto& input = types.get(typeOf(item.operands[0]));
                                valid = valid && input.kind == TypeKind::Reference && !input.isMutable && types.get(input.element).kind == operation->input;
                            }
                            auto output = item.type;
                            if (operation->fallible)
                            {
                                const auto* carrier = recordOf(output); TypeId failure = InvalidTypeId, success = InvalidTypeId;
                                valid = valid && carrier && carrier->isEnum && carrier->variants.size() == 2;
                                if (carrier) for (const auto& variant : carrier->variants)
                                {
                                    if (variant.tag == 0 && variant.fieldTypes.size() == 1 && success == InvalidTypeId) success = variant.fieldTypes[0];
                                    else if (variant.tag == 1 && variant.fieldTypes.size() == 1 && failure == InvalidTypeId) failure = variant.fieldTypes[0];
                                    else valid = false;
                                }
                                output = success;
                                valid = valid && twoEmptyVariants(failure,"CancellationError");
                            }
                            const auto kind = types.get(output).kind;
                            valid = valid && (operation->result == CancellationResult::Source ? kind == TypeKind::CancellationSource
                                : operation->result == CancellationResult::Token ? kind == TypeKind::CancellationToken
                                : operation->result == CancellationResult::Boolean ? kind == TypeKind::Bool
                                : twoEmptyVariants(output,"CancelRequest"));
                        }
                        if (!valid) fail(item.range, "Cancellation operation, sealed owner role or exact Result shape mismatch.");
                    }
                    if (item.opcode == AIROpcode::SyncIntrinsic)
                    {
                        const auto* operation = syncOperation(item.operation);
                        const auto payload = item.syncElement;
                        const auto& element = types.get(payload);
                        const auto recordOf = [&](TypeId type) -> const AIRRecord*
                        { for (const auto& record : module.records) if (record.type == type) return &record; return nullptr; };
                        const auto shape = [&](TypeId type, const std::vector<std::vector<TypeId>>& fields)
                        {
                            const auto* record = recordOf(type);
                            if (!record || types.get(type).kind != TypeKind::Nominal || !record->isEnum || record->variants.size() != fields.size()) return false;
                            std::vector<bool> seen(fields.size(), false);
                            for (const auto& variant : record->variants)
                            {
                                if (variant.tag >= fields.size() || seen[variant.tag] || variant.fieldTypes != fields[variant.tag]) return false;
                                seen[variant.tag] = true;
                            }
                            return true;
                        };
                        const auto carrier = [&](TypeId type, bool optional) -> std::pair<TypeId, TypeId>
                        {
                            const auto* record = recordOf(type);
                            if (!record || !record->isEnum || record->variants.size() != 2) return {InvalidTypeId,InvalidTypeId};
                            TypeId value = InvalidTypeId, error = InvalidTypeId;
                            for (const auto& variant : record->variants)
                                if (variant.tag == 0 && variant.fieldTypes.size() == 1) value = variant.fieldTypes[0];
                                else if (variant.tag == 1 && variant.fieldTypes.size() == (optional ? 0u : 1u)) error = optional ? types.builtin("unit") : variant.fieldTypes[0];
                            return {value,error};
                        };
                        bool valid = operation && item.operands.size() == operation->operands && item.result != InvalidAIRId
                            && payload != InvalidTypeId && element.kind != TypeKind::Error && element.kind != TypeKind::Void
                            && element.kind != TypeKind::Never && element.kind != TypeKind::Dynamic && item.atomicElement == InvalidTypeId
                            && item.callee == InvalidSymbolId && item.place.local == InvalidAIRId && item.place.pointer == InvalidAIRId
                            && item.place.global == InvalidSymbolId && item.place.type == InvalidTypeId && item.place.projections.empty()
                            && item.dynamicTargets.empty() && item.dynamicSignature == InvalidTypeId;
                        if (operation)
                        {
                            for (std::size_t index = 0; index < item.operands.size(); ++index)
                            {
                                const auto actual = typeOf(item.operands[index]);
                                if (index == 0 && operation->access != SyncAccess::None)
                                {
                                    const auto& argument = types.get(actual);
                                    const auto owner = operation->access == SyncAccess::Value ? actual : argument.element;
                                    const auto& owning = types.get(owner);
                                    valid = valid && (operation->access == SyncAccess::Value || (argument.kind == TypeKind::Reference
                                        && (operation->access != SyncAccess::Exclusive || argument.isMutable)));
                                    if (operation->kind == SyncKind::Poisoned)
                                    { const auto* record = recordOf(owner); valid = valid && record && !record->isEnum && record->fieldTypes == std::vector<TypeId>{payload}; }
                                    else valid = valid && owning.kind == syncTypeKind(operation->kind) && owning.element == payload;
                                }
                                else if (index == 0) valid = valid && actual == payload;
                                else if (operation->timed)
                                { const auto* record = recordOf(actual); valid = valid && types.get(actual).kind == TypeKind::Nominal && types.get(actual).name == "Duration" && record && !record->isEnum && record->fieldTypes == std::vector<TypeId>{types.builtin("u64")}; }
                                else valid = valid && shape(actual, {{},{}}) && types.get(actual).name == "PoisonPolicy";
                            }
                            auto output = item.type;
                            if (operation->result == SyncResult::Acquire || operation->result == SyncResult::OptionalAcquire || operation->result == SyncResult::UniqueReference || operation->result == SyncResult::IntoInner)
                            {
                                const auto [value,error] = carrier(output,false); output = value;
                                auto capability = value;
                                if (operation->result == SyncResult::OptionalAcquire)
                                { const auto optional = carrier(output,true); capability = optional.first; valid = valid && optional.second == types.builtin("unit"); }
                                if (operation->result == SyncResult::IntoInner)
                                { const auto* record = recordOf(error); valid = valid && record && !record->isEnum && record->fieldTypes == std::vector<TypeId>{payload}; }
                                else valid = valid && shape(error, {{capability},{},{}});
                                if (operation->result == SyncResult::UniqueReference)
                                    valid = valid && types.get(value).kind == TypeKind::Reference && types.get(value).isMutable && types.get(value).element == payload;
                                else if (operation->result == SyncResult::IntoInner) valid = valid && value == payload;
                                else valid = valid && types.get(capability).kind == syncTypeKind(operation->guard) && types.get(capability).element == payload;
                            }
                            else if (operation->result == SyncResult::Owner) valid = valid && types.get(output).kind == syncTypeKind(operation->kind) && types.get(output).element == payload;
                            else if (operation->result == SyncResult::Boolean) valid = valid && output == types.builtin("bool");
                            else if (operation->result == SyncResult::Policy) valid = valid && shape(output, {{},{}}) && types.get(output).name == "PoisonPolicy";
                            else if (operation->result == SyncResult::Reference || operation->result == SyncResult::MutableReference)
                                valid = valid && types.get(output).kind == TypeKind::Reference && types.get(output).element == payload
                                    && types.get(output).isMutable == (operation->result == SyncResult::MutableReference);
                            else if (operation->result == SyncResult::Recovered) valid = valid && output == payload;
                            else valid = valid && output == types.builtin("unit");
                        }
                        if (!valid) fail(item.range, "Sync operation, sealed capability, payload or error carrier shape mismatch.");
                    }
                    if (item.opcode == AIROpcode::AtomicIntrinsic)
                    {
                        const auto* operation = atomicOperation(item.operation);
                        const auto scalar = item.atomicElement;
                        const auto& element = types.get(scalar);
                        const bool supported = element.kind == TypeKind::Bool || (element.kind == TypeKind::Integer
                            && (element.bits == 8 || element.bits == 16 || element.bits == 32 || element.bits == 64));
                        const auto recordOf = [&](TypeId type) -> const AIRRecord*
                        { for (const auto& record : module.records) if (record.type == type) return &record; return nullptr; };
                        const auto emptyEnum = [&](TypeId type, std::string_view name, std::size_t count)
                        {
                            const auto* record = recordOf(type);
                            if (types.get(type).kind != TypeKind::Nominal || types.get(type).name != name || !record || !record->isEnum || record->variants.size() != count) return false;
                            for (std::size_t index = 0; index < count; ++index)
                                if (record->variants[index].tag != index || !record->variants[index].fieldTypes.empty()) return false;
                            return true;
                        };
                        bool valid = operation && item.operands.size() == operation->operands && item.result != InvalidAIRId
                            && scalar != InvalidTypeId && element.kind != TypeKind::Error && item.callee == InvalidSymbolId
                            && item.place.local == InvalidAIRId && item.place.pointer == InvalidAIRId && item.place.global == InvalidSymbolId
                            && item.place.type == InvalidTypeId && item.place.projections.empty() && item.dynamicTargets.empty() && item.dynamicSignature == InvalidTypeId;
                        if (operation)
                        {
                            const bool query = operation->result == AtomicResult::Boolean || operation->result == AtomicResult::Alignment;
                            valid = valid && (query || item.operation == "fence" || item.operation == "compiler_fence" ? true : supported)
                                && (!operation->integerOnly || element.kind == TypeKind::Integer);
                            for (std::size_t index = 0; index < item.operands.size(); ++index)
                            {
                                const auto actual = typeOf(item.operands[index]);
                                const auto& argument = types.get(actual);
                                if (operation->cell && index == 0)
                                {
                                    const auto owning = item.operation == "into_inner" ? actual : argument.element;
                                    valid = valid && types.get(owning).kind == TypeKind::Atomic && types.get(owning).element == scalar
                                        && (item.operation == "into_inner" || (argument.kind == TypeKind::Reference && (item.operation != "get_mut" || argument.isMutable)));
                                }
                                else if (static_cast<int>(index) == operation->order || static_cast<int>(index) == operation->failure)
                                    valid = valid && emptyEnum(actual, "MemoryOrder", 5);
                                else if (item.operation == "wait_timeout" && index == 2)
                                { const auto* record = recordOf(actual); valid = valid && argument.kind == TypeKind::Nominal && argument.name == "Duration" && record && !record->isEnum && record->fieldTypes == std::vector<TypeId>{types.builtin("u64")}; }
                                else valid = valid && actual == scalar;
                            }
                            auto output = item.type;
                            if (operation->fallible)
                            {
                                const auto* carrier = recordOf(output);
                                valid = valid && carrier && carrier->isEnum && carrier->variants.size() == 2;
                                if (carrier && carrier->variants.size() == 2)
                                {
                                    valid = valid && carrier->variants[0].tag == 0 && carrier->variants[1].tag == 1
                                        && carrier->variants[0].fieldTypes.size() == 1 && carrier->variants[1].fieldTypes.size() == 1;
                                    if (carrier->variants[0].fieldTypes.size() == 1 && carrier->variants[1].fieldTypes.size() == 1)
                                    { output = carrier->variants[0].fieldTypes[0]; valid = valid && emptyEnum(carrier->variants[1].fieldTypes[0], "AtomicError", 3); }
                                }
                            }
                            const auto& returned = types.get(output);
                            const auto* record = recordOf(output);
                            switch (operation->result)
                            {
                                case AtomicResult::Cell: valid = valid && returned.kind == TypeKind::Atomic && returned.element == scalar; break;
                                case AtomicResult::Scalar: valid = valid && output == scalar; break;
                                case AtomicResult::Unit: valid = valid && returned.kind == TypeKind::Unit; break;
                                case AtomicResult::MutableReference: valid = valid && returned.kind == TypeKind::Reference && returned.isMutable && returned.element == scalar; break;
                                case AtomicResult::Cas: valid = valid && record && !record->isEnum && record->fieldTypes == std::vector<TypeId>{types.builtin("bool"), scalar}; break;
                                case AtomicResult::Wait: valid = valid && record && !record->isEnum && record->fieldTypes == std::vector<TypeId>{scalar, types.builtin("bool"), types.builtin("bool")}; break;
                                case AtomicResult::Boolean: valid = valid && returned.kind == TypeKind::Bool; break;
                                case AtomicResult::Alignment: valid = valid && output == types.builtin("usize"); break;
                            }
                        }
                        if (!valid) fail(item.range, "Atomic operation, cell, order, scalar or result shape mismatch.");
                    }
                    if (item.opcode == AIROpcode::SharedIntrinsic)
                    {
                        const auto recordOf = [&](TypeId type) -> const AIRRecord*
                        { for (const auto& record : module.records) if (record.type == type && record.isEnum) return &record; return nullptr; };
                        const auto carrier = [&](TypeId type, bool optional) -> std::pair<TypeId, TypeId>
                        {
                            const auto* record = recordOf(type);
                            if (!record || record->variants.size() != 2 || record->variants[0].fieldTypes.size() != 1
                                || record->variants[1].fieldTypes.size() != (optional ? 0u : 1u)) return {InvalidTypeId, InvalidTypeId};
                            return {record->variants[0].fieldTypes[0], optional ? InvalidTypeId : record->variants[1].fieldTypes[0]};
                        };
                        const bool weak = item.operation.starts_with("weak.");
                        const auto method = item.operation.substr(item.operation.find('.') + 1);
                        const std::set<std::string> strong = {"new","clone","as_ref","downgrade","get_mut","try_unwrap","strong_count","weak_count","ptr_equal"};
                        const std::set<std::string> weakMethods = {"new","clone","upgrade","strong_count","weak_count","ptr_equal","is_empty","is_expired","clear"};
                        const auto count = weak && method == "new" ? 0u : method == "ptr_equal" ? 2u : 1u;
                        bool valid = (weak ? weakMethods : strong).contains(method)
                            && (weak || item.operation.starts_with("shared.")) && item.operands.size() == count
                            && item.callee == InvalidSymbolId && item.result != InvalidAIRId && item.place.local == InvalidAIRId
                            && item.place.pointer == InvalidAIRId && item.place.global == InvalidSymbolId && item.place.projections.empty()
                            && item.place.type == InvalidTypeId && item.dynamicTargets.empty() && item.dynamicSignature == InvalidSymbolId;
                        const auto first = count && !item.operands.empty() ? typeOf(item.operands[0]) : InvalidTypeId;
                        const auto& input = types.get(first);
                        const auto owner = method == "try_unwrap" ? first : input.element;
                        const auto& owning = types.get(owner);
                        if (weak && method == "new") valid = valid && types.get(item.type).kind == TypeKind::Weak;
                        else if (!weak && method == "new")
                        {
                            const auto [value, error] = carrier(item.type, false);
                            const auto* failures = recordOf(error);
                            valid = valid && types.get(value).kind == TypeKind::Shared && types.get(value).element == first
                                && failures && failures->variants.size() == 3;
                            if (failures) for (const auto& variant : failures->variants) valid = valid && variant.fieldTypes == std::vector<TypeId>{first};
                        }
                        else
                        {
                            valid = valid && owning.kind == (weak ? TypeKind::Weak : TypeKind::Shared)
                                && (method == "try_unwrap" || (input.kind == TypeKind::Reference
                                    && ((method != "get_mut" && method != "clear") || input.isMutable)));
                            if (method == "strong_count" || method == "weak_count") valid = valid && item.type == types.builtin("usize");
                            else if (method == "is_empty" || method == "is_expired" || method == "ptr_equal")
                                valid = valid && item.type == types.builtin("bool") && (method != "ptr_equal" || (item.operands.size() == 2
                                    && types.get(typeOf(item.operands[1])).kind == TypeKind::Reference && types.get(typeOf(item.operands[1])).element == owner));
                            else if (method == "clear") valid = valid && item.type == types.builtin("unit");
                            else if (method == "as_ref") valid = valid && types.get(item.type).kind == TypeKind::Reference
                                && !types.get(item.type).isMutable && types.get(item.type).element == owning.element;
                            else if (method == "get_mut")
                            {
                                const auto [value, error] = carrier(item.type, true); (void)error;
                                valid = valid && types.get(value).kind == TypeKind::Reference && types.get(value).isMutable && types.get(value).element == owning.element;
                            }
                            else
                            {
                                auto [value, error] = carrier(item.type, false);
                                if (method == "try_unwrap") valid = valid && value == owning.element && error == owner;
                                else
                                {
                                    const auto* failures = recordOf(error);
                                    valid = valid && failures && failures->variants.size() == 1 && failures->variants[0].fieldTypes.empty();
                                    if (method == "upgrade") value = carrier(value, true).first;
                                    const auto expected = method == "downgrade" || (weak && method == "clone") ? TypeKind::Weak : TypeKind::Shared;
                                    valid = valid && types.get(value).kind == expected && types.get(value).element == owning.element;
                                }
                            }
                        }
                        if (!valid) fail(item.range, "Shared/Weak operation, ownership, payload or error carrier shape mismatch.");
                    }
                    if (item.opcode == AIROpcode::SystemIntrinsic)
                    {
                        const auto recordOf = [&](TypeId type) -> const AIRRecord*
                        { for (const auto& record : module.records) if (record.type == type) return &record; return nullptr; };
                        const auto shape = [&](const AIRRecord* record, const std::vector<std::vector<TypeId>>& fields)
                        {
                            if (!record || !record->isEnum || record->variants.size() != fields.size()) return false;
                            std::vector<bool> seen(fields.size(), false);
                            for (const auto& variant : record->variants)
                            {
                                if (variant.tag >= fields.size() || seen[variant.tag] || variant.fieldTypes != fields[variant.tag]) return false;
                                seen[variant.tag] = true;
                            }
                            return true;
                        };
                        const auto ioShape = [&](TypeId type)
                        {
                            const auto i32 = types.builtin("i32"), usize = types.builtin("usize");
                            return types.get(type).kind == TypeKind::Nominal && types.get(type).name == "IoError"
                                && shape(recordOf(type), {{i32,usize},{i32,usize},{i32},{},{},{},{},{},{},{},{}});
                        };
                        const auto parseShape = [&](TypeId type)
                        { return types.get(type).kind == TypeKind::Nominal && types.get(type).name == "ParseError" && shape(recordOf(type), {{},{},{},{},{}}); };
                        const bool input = item.operation == "input", flush = item.operation == "flush", log = item.operation == "log";
                        if (!(input || flush || log || item.operation == "print" || item.operation == "println" || item.operation == "printf")
                            || item.result == InvalidAIRId || ((input || flush) && !item.operands.empty())
                            || (log && item.operands.size() < 3) || ((item.operation == "print" || item.operation == "println") && item.operands.empty()))
                            fail(item.range, "system instruction has an unknown operation or invalid argument count.");
                        const auto supported = [&](TypeId id, bool allowString)
                        {
                            const auto& type = types.get(id);
                            return type.kind == TypeKind::Integer || type.kind == TypeKind::Bool || type.kind == TypeKind::Char || type.kind == TypeKind::Byte
                                || (type.kind == TypeKind::Float && (type.bits == 32 || type.bits == 64))
                                || (allowString && type.kind == TypeKind::String);
                        };
                        const auto text = [&](TypeId id)
                        {
                            const auto& type = types.get(id);
                            return type.kind == TypeKind::StringView || (type.kind == TypeKind::Reference
                                && (types.get(type.element).kind == TypeKind::String || types.get(type.element).kind == TypeKind::StringView));
                        };
                        for (std::size_t argument = 0; argument < item.operands.size(); ++argument)
                        {
                            const auto type = typeOf(item.operands[argument]);
                            if (log && argument == 0)
                            {
                                if (types.get(type).kind != TypeKind::Nominal || types.get(type).name != "LogLevel" || !shape(recordOf(type), {{},{},{}}))
                                    fail(item.range, "system.log requires a three-tag LogLevel without payloads.");
                            }
                            else if (log && argument + 1 == item.operands.size())
                            { if (!text(type)) fail(item.range, "system.log code must be text."); }
                            else if (!text(type) && !supported(type, false)) fail(item.range, "system output argument has an unsupported type.");
                        }
                        if (input && !supported(item.dynamicSignature, true)) fail(item.range, "system.input has an unsupported target type.");
                        const AIRRecord* result = nullptr;
                        for (const auto& record : module.records) if (record.type == item.type) result = &record;
                        if (!result || !result->isEnum || result->variants.size() != 2) fail(item.range, "system output must use a Result carrier.");
                        else
                        {
                            std::array<bool, 2> tags{};
                            for (const auto& variant : result->variants)
                            {
                            if (variant.tag >= tags.size() || tags[variant.tag]) { fail(item.range, "system Result tags must be unique zero and one."); continue; }
                            tags[variant.tag] = true;
                            if (variant.fieldTypes.size() != 1) { fail(item.range, "system Result variants must each have one payload."); continue; }
                            if (variant.tag == 1)
                            {
                                const auto errorType = variant.fieldTypes[0];
                                if (!input)
                                { if (!ioShape(errorType)) fail(item.range, "system output requires the complete IoError payload shape."); }
                                else
                                {
                                    const auto* errors = recordOf(errorType);
                                    TypeId io = InvalidTypeId, parse = InvalidTypeId;
                                    if (errors) for (const auto& cause : errors->variants)
                                    {
                                        if (cause.fieldTypes.size() != 1) continue;
                                        if (cause.tag == 0) io = cause.fieldTypes[0];
                                        if (cause.tag == 1) parse = cause.fieldTypes[0];
                                    }
                                    if (types.get(errorType).kind != TypeKind::Nominal || types.get(errorType).name != "ReadError"
                                        || !shape(errors, {{io},{parse}}) || !ioShape(io) || !parseShape(parse))
                                        fail(item.range, "system.input requires ReadError Io/Parse payloads and complete nested error shapes.");
                                }
                            }
                            if (variant.tag == 0 && !input && variant.fieldTypes[0] != types.builtin("unit")) fail(item.range, "system output Result must contain unit.");
                            if (variant.tag == 0 && input)
                            {
                                const AIRRecord* optional = nullptr;
                                for (const auto& record : module.records) if (record.type == variant.fieldTypes[0]) optional = &record;
                                if (!shape(optional, {{item.dynamicSignature},{}})) fail(item.range, "system.input Optional payload or tags differ from target type.");
                            }
                            }
                        }
                    }
                    if (item.opcode == AIROpcode::ParseIntrinsic)
                    {
                        const auto& target = types.get(item.dynamicSignature);
                        const bool scalar = target.kind == TypeKind::Integer || target.kind == TypeKind::Bool
                            || target.kind == TypeKind::Char || target.kind == TypeKind::Byte
                            || (target.kind == TypeKind::Float && (target.bits == 32 || target.bits == 64));
                        if (item.operation != "scalar.parse" || !scalar || item.operands.size() != 1 || item.result == InvalidAIRId)
                            fail(item.range, "parse requires one text input, a supported scalar target, and a result.");
                        else
                        {
                            auto input = typeOf(item.operands[0]);
                            const auto& original = types.get(input);
                            if (original.kind == TypeKind::Reference) input = original.element;
                            if (!(types.get(input).kind == TypeKind::StringView
                                || (original.kind == TypeKind::Reference && types.get(input).kind == TypeKind::String)))
                                fail(item.range, "parse input must be string_view or an explicit text borrow.");
                        }
                        const AIRRecord* result = nullptr;
                        for (const auto& record : module.records) if (record.type == item.type) result = &record;
                        TypeId errorType = InvalidTypeId;
                        if (!result || !result->isEnum || result->variants.size() != 2)
                            fail(item.range, "parse output must be a two-variant Result.");
                        else for (const auto& variant : result->variants)
                        {
                            if (variant.fieldTypes.size() != 1 || (variant.tag == 0 && variant.fieldTypes[0] != item.dynamicSignature))
                                fail(item.range, "parse Result payload differs from the scalar target.");
                            if (variant.tag == 1 && variant.fieldTypes.size() == 1) errorType = variant.fieldTypes[0];
                        }
                        const AIRRecord* errors = nullptr;
                        for (const auto& record : module.records) if (record.type == errorType) errors = &record;
                        if (!errors || !errors->isEnum || types.get(errorType).name != "ParseError" || errors->variants.size() != 5)
                            fail(item.range, "parse Result must carry the canonical ParseError shape.");
                        else for (const auto& variant : errors->variants)
                            if (!variant.fieldTypes.empty()) fail(item.range, "ParseError variants cannot carry payloads.");
                    }
                    if (item.opcode == AIROpcode::ComplexIntrinsic)
                    {
                        const AIRRecord* record = nullptr;
                        for (const auto& candidate : module.records) if (candidate.type == item.dynamicSignature) record = &candidate;
                        const bool magnitude = item.operation == "magnitude";
                        if (!record || record->isEnum || record->fieldTypes.size() != 2
                            || record->fieldTypes[0] != record->fieldTypes[1] || types.get(record->fieldTypes[0]).kind != TypeKind::Float)
                            fail(item.range, "complex intrinsic requires two matching floating-point components.");
                        else if ((item.operation != "divide_components" && !magnitude) || item.operands.size() != (magnitude ? 1u : 2u)
                            || item.type != (magnitude ? record->fieldTypes[0] : item.dynamicSignature))
                            fail(item.range, "complex intrinsic signature mismatch.");
                        for (const auto operand : item.operands)
                            if (typeOf(operand) != item.dynamicSignature) fail(item.range, "complex intrinsic operand type mismatch.");
                    }
                    if (item.opcode == AIROpcode::Simd)
                    {
                        const auto& vector = types.get(item.dynamicSignature);
                        const bool mask = item.operation.starts_with("simd.mask.");
                        const auto method = item.operation.substr(mask ? 10 : 5);
                        if ((mask ? vector.kind != TypeKind::SimdMask : vector.kind != TypeKind::Simd)
                            || !item.operation.starts_with("simd.")) fail(item.range, "SIMD instruction has no valid vector type.");
                        const bool unary = method == "splat" || method == "from_array" || method == "to_array"
                            || method == "sum" || method == "sqrt" || method == "lanes" || method == "any" || method == "all" || method == "not";
                        const bool ternary = method == "store" || method == "select";
                        const auto count = ternary ? 3u : unary ? 1u : 2u;
                        if (item.operands.size() != count) fail(item.range, "SIMD instruction argument count mismatch.");
                        if (item.result == InvalidAIRId) fail(item.range, "SIMD instruction must define its result value.");
                        if (item.operands.size() == count)
                        {
                            if (method == "splat" && typeOf(item.operands[0]) != vector.element) fail(item.range, "SIMD splat element mismatch.");
                            if (method == "extract" && (typeOf(item.operands[0]) != item.dynamicSignature
                                || types.get(typeOf(item.operands[1])).kind != TypeKind::Integer)) fail(item.range, "SIMD extraction input mismatch.");
                            if (method == "load" || method == "store" || method == "from_array")
                            {
                                const auto& reference = types.get(typeOf(item.operands[0]));
                                const auto& array = types.get(reference.element);
                                if (reference.kind != TypeKind::Reference || array.kind != TypeKind::Array || array.element != vector.element
                                    || (method == "store" && !reference.isMutable)) fail(item.range, "SIMD array access requires the correct borrowed storage.");
                            }
                        }
                    }
                    if (item.opcode == AIROpcode::Write)
                    {
                        if (item.operands.size() != 1) fail(item.range, "write requires one value.");
                        else if (typeOf(item.operands[0]) != item.place.type) fail(item.range, "write value and place types differ.");
                    }
                    if ((item.opcode == AIROpcode::Read || item.opcode == AIROpcode::Move) && item.type != item.place.type) fail(item.range, "read value and place types differ.");
                    if (item.opcode == AIROpcode::BorrowShared || item.opcode == AIROpcode::BorrowMut)
                    {
                        const auto& borrowed = types.get(item.type);
                        if (borrowed.kind != TypeKind::Reference || (borrowed.element != item.place.type && !item.operation.starts_with("dyn.view.") && item.operation != "box.borrow")
                            || borrowed.isMutable != (item.opcode == AIROpcode::BorrowMut)) fail(item.range, "borrow result and place types differ.");
                    }
                    if ((item.opcode == AIROpcode::Binary && item.operands.size() != 2) || (item.opcode == AIROpcode::Unary && item.operands.size() != 1)) fail(item.range, "operator has an invalid number of operands.");
                    if (item.opcode == AIROpcode::Call)
                    {
                        if (item.operation.starts_with("dyn.call."))
                        {
                            const auto& contract = types.get(item.dynamicSignature);
                            if (contract.kind != TypeKind::Function || contract.result != item.type || contract.parameters.size() != item.operands.size())
                                fail(item.range, "dynamic call signature mismatch.");
                            else for (std::size_t argument = 0; argument < item.operands.size(); ++argument)
                                if (contract.parameters[argument] != typeOf(item.operands[argument])) fail(item.range, "dynamic call argument type mismatch.");
                            continue;
                        }
                        const AIRFunction* target = nullptr;
                        for (const auto& candidate : module.functions) if (candidate.symbol == item.callee) target = &candidate;
                        const AIRExternal* foreign = nullptr;
                        if (!target) for (const auto& candidate : module.externals) if (candidate.symbol == item.callee) foreign = &candidate;
                        if (!target && !foreign) fail(item.range, "call target has no AIR function or external declaration.");
                        else if (foreign)
                        {
                            if (foreign->variadic || foreign->callingConvention != "C" || foreign->parameters.size() != item.operands.size() || foreign->result != item.type) fail(item.range, "external call signature mismatch.");
                            else for (std::size_t argument = 0; argument < item.operands.size(); ++argument)
                                if (foreign->parameters[argument] != typeOf(item.operands[argument])) fail(item.range, "external call argument has the wrong type.");
                        }
                        else if (target->parameters.size() != item.operands.size() || target->result != item.type) fail(item.range, "call signature differs from selected function.");
                        else for (std::size_t argument = 0; argument < item.operands.size(); ++argument)
                            if (target->parameters[argument] >= target->locals.size() || target->locals[target->parameters[argument]].type != typeOf(item.operands[argument])) fail(item.range, "call argument has the wrong type.");
                    }
                }
                const auto& end = current.terminator;
                if (end.value != InvalidAIRId) use(end.value, block, current.instructions.size(), end.range);
                if (end.kind == AIRTerminatorKind::Branch && types.get(typeOf(end.value)).kind != TypeKind::Bool) fail(end.range, "conditional branch requires a bool.");
                if (end.kind == AIRTerminatorKind::NoReturn)
                {
                    if (current.instructions.empty() || current.instructions.back().opcode != AIROpcode::Call
                        || types.get(current.instructions.back().type).kind != TypeKind::Never
                        || current.instructions.back().result != InvalidAIRId)
                        fail(end.range, "noreturn terminator requires a final never-returning call.");
                }
                if (end.kind == AIRTerminatorKind::Return)
                {
                    if (types.get(function.result).kind == TypeKind::Never)
                        fail(end.range, "never-returning function cannot contain a return terminator.");
                    const auto returnsVoid = types.get(function.result).kind == TypeKind::Void;
                    if ((!returnsVoid && end.value == InvalidAIRId) || (end.value != InvalidAIRId && typeOf(end.value) != function.result)) fail(end.range, "return type differs from function result.");
                }
                for (const auto scope : end.exitedScopes) if (scope >= function.scopes.size()) fail(end.range, "terminator exits an invalid scope.");
            }
        }
        return diagnostics.size() == before;
    }
}
