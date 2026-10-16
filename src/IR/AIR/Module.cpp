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
#include <sstream>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
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
                    output << " scope=" << item.scope << '\n';
                }
                const auto& end = function.blocks[block].terminator;
                if (end.kind == AIRTerminatorKind::Jump) output << "  jump b" << end.target;
                else if (end.kind == AIRTerminatorKind::Branch) output << "  branch %" << end.value << " b" << end.target << " b" << end.alternative;
                else if (end.kind == AIRTerminatorKind::Return) { output << "  return"; if (end.value != InvalidAIRId) output << " %" << end.value; }
                else output << "  unreachable";
                output << '\n';
            }
            output << "}\n";
        }
        return output.str();
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
                        || item.opcode == AIROpcode::VectorAppend || item.opcode == AIROpcode::VectorReserve;
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
                                if (record && projection.kind == AIRProjectionKind::Field && !record->isEnum && projection.field < record->fieldTypes.size()) projected = record->fieldTypes[projection.field];
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
                    if (item.opcode == AIROpcode::Aggregate)
                    {
                        std::vector<TypeId> fields;
                        const auto& aggregate = types.get(item.type);
                        if (aggregate.kind == TypeKind::Array) fields.assign(aggregate.extent, aggregate.element);
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
                if (end.kind == AIRTerminatorKind::Return)
                {
                    const auto returnsVoid = types.get(function.result).kind == TypeKind::Void;
                    if ((!returnsVoid && end.value == InvalidAIRId) || (end.value != InvalidAIRId && typeOf(end.value) != function.result)) fail(end.range, "return type differs from function result.");
                }
                for (const auto scope : end.exitedScopes) if (scope >= function.scopes.size()) fail(end.range, "terminator exits an invalid scope.");
            }
        }
        return diagnostics.size() == before;
    }
}
