// ========================
//
// Created By : miruku_sora2026
//
// File name : BorrowChecker.cpp
//
// Description :
//     AIR의 장소와 제어 흐름을 분석하여 소유권, 대여, 반환 수명 및 정리를 검증한다.
//     Analyze AIR places and control flow to verify ownership, loans, returned lifetimes, and cleanup.
//
// ========================

#include "aevix/Frontend/Ownership/BorrowChecker.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <sstream>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace aevix
{
    namespace
    {
        using IdSet = std::set<std::size_t>;

        enum class OriginKind { Local, Global, Parameter, ParameterElements, VariadicStorage };
        struct Target final
        {
            OriginKind kind = OriginKind::Local;
            std::size_t id = InvalidAIRId;
            // 필드 번호는 서로 구별하지만 실행 중 인덱스는 보수적으로 겹친다고 본다.
            // Field indices are distinct, while runtime indices conservatively overlap.
            std::vector<std::pair<AIRProjectionKind, std::size_t>> projections;
            auto operator<=>(const Target&) const = default;
        };
        struct Loan final
        {
            bool exclusive = false;
            std::set<Target> targets;
            IdSet parents;
            bool sliceStorage = false;
            bool sliceContents = false;
            bool operator==(const Loan&) const = default;
        };
        struct State final
        {
            // 1은 살아 있을 가능성, 2는 이동되었을 가능성이며 병합 시 두 비트를 합친다.
            // Bit 1 means possibly live and bit 2 possibly moved; joins combine both bits.
            std::vector<unsigned char> live;
            std::vector<IdSet> locals;
            std::vector<IdSet> values;
            std::map<std::size_t, Loan> loans;
            std::set<std::tuple<AIRLocalId, SymbolId, std::size_t>> extractedFields;
            IdSet initializedPersist;
            bool operator==(const State&) const = default;
        };
        struct PlaceView final
        {
            std::set<Target> targets;
            IdSet permission;
        };
        struct FunctionFlow final
        {
            std::vector<std::optional<State>> incoming;
            ReferenceReturnSummary summary;
        };

        template<class T> void unite(std::set<T>& target, const std::set<T>& source)
        {
            target.insert(source.begin(), source.end());
        }

        State merge(State result, const State& other)
        {
            for (std::size_t i = 0; i < result.live.size(); ++i)
            {
                result.live[i] |= other.live[i];
                unite(result.locals[i], other.locals[i]);
            }
            for (std::size_t i = 0; i < result.values.size(); ++i)
                unite(result.values[i], other.values[i]);
            for (const auto& [id, loan] : other.loans)
            {
                auto& merged = result.loans[id];
                merged.exclusive = merged.exclusive || loan.exclusive;
                merged.sliceStorage = merged.sliceStorage || loan.sliceStorage;
                merged.sliceContents = merged.sliceContents || loan.sliceContents;
                unite(merged.targets, loan.targets);
                unite(merged.parents, loan.parents);
            }
            unite(result.extractedFields, other.extractedFields);
            IdSet initialized;
            std::set_intersection(result.initializedPersist.begin(), result.initializedPersist.end(), other.initializedPersist.begin(), other.initializedPersist.end(), std::inserter(initialized, initialized.end()));
            result.initializedPersist = std::move(initialized);
            return result;
        }

        bool overlaps(const Target& left, const Target& right)
        {
            if (left.kind != right.kind || left.id != right.id) return false;
            const auto count = std::min(left.projections.size(), right.projections.size());
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto [lk, li] = left.projections[i];
                const auto [rk, ri] = right.projections[i];
                if ((lk == AIRProjectionKind::Field || lk == AIRProjectionKind::EnumField)
                    && lk == rk && li != ri)
                    return false;
            }
            return true;
        }

        std::vector<AIRBlockId> successors(const AIRBlock& block)
        {
            if (block.terminator.kind == AIRTerminatorKind::Jump)
                return {block.terminator.target};
            if (block.terminator.kind == AIRTerminatorKind::Branch)
                return {block.terminator.target, block.terminator.alternative};
            return {};
        }

        void instructionUses(const AIRInstruction& instruction, IdSet& values)
        {
            for (const auto value : instruction.operands)
                if (value != InvalidAIRId) values.insert(value);
            if (instruction.place.pointer != InvalidAIRId)
                values.insert(instruction.place.pointer);
            for (const auto& projection : instruction.place.projections)
                if (projection.index != InvalidAIRId) values.insert(projection.index);
        }

        class Checker final
        {
        public:
            Checker(AIRModule& module, const NameResolution& names,
                    const TypeCheckResult& types, Diagnostics& diagnostics)
                : module_(module), names_(names), types_(types), diagnostics_(diagnostics)
            {
                for (const auto& record : module_.records) records_[record.type] = &record;
                for (const auto& symbol : names_.symbols)
                    if (symbol.declaration) declarations_[symbol.declaration] = symbol.id;
            }

            OwnershipResult run()
            {
                const auto before = diagnostics_.size();
                // 재검사할 때 지난 정리 계획을 먼저 제거하여 분석 입력을 동일하게 유지한다.
                // Remove prior cleanup plans so repeated checks analyze identical ownership events.
                for (auto& function : module_.functions)
                    for (auto& block : function.blocks)
                        std::erase_if(block.instructions, [](const auto& instruction)
                        {
                            return instruction.opcode == AIROpcode::Drop
                                && instruction.operation == "ownership.cleanup";
                        });
                for (const auto& function : module_.functions) summaries_[function.symbol] = {};
                for (const auto& function : module_.functions) for (const auto& block : function.blocks) for (const auto& instruction : block.instructions)
                    if (instruction.opcode == AIROpcode::Call && instruction.operation.starts_with("dyn.call.")) summaries_[instruction.callee] = {};
                collectGlobalEffects();
                // 반환 원천 요약은 호출 그래프의 순환과 무관하게 유한 집합의 고정점으로 구한다.
                // Compute returned-origin summaries as a finite-set fixed point, including recursion.
                bool changed = true;
                while (changed)
                {
                    changed = false;
                    for (const auto& function : module_.functions) for (const auto& block : function.blocks) for (const auto& instruction : block.instructions)
                        if (instruction.opcode == AIROpcode::Call && instruction.operation.starts_with("dyn.call."))
                        {
                            auto& summary = summaries_[instruction.callee]; const auto previous = summary;
                            for (const auto target : instruction.dynamicTargets)
                            {
                                unite(summary.parameters, summaries_[target].parameters);
                                unite(summary.elementParameters, summaries_[target].elementParameters);
                                unite(summary.globals, summaries_[target].globals);
                            }
                            changed = changed || summary != previous;
                        }
                    for (auto& function : module_.functions)
                    {
                        prepare(function);
                        const auto flow = analyze(function, false);
                        auto& summary = summaries_[function.symbol];
                        const auto prior = summary;
                        unite(summary.parameters, flow.summary.parameters);
                        unite(summary.elementParameters, flow.summary.elementParameters);
                        unite(summary.globals, flow.summary.globals);
                        summary.global = summary.global || flow.summary.global;
                        changed = changed || summary != prior;
                    }
                }
                std::vector<FunctionFlow> flows;
                for (auto& function : module_.functions)
                {
                    prepare(function);
                    flows.push_back(analyze(function, true));
                }
                OwnershipResult result;
                result.returns = summaries_;
                result.succeeded = diagnostics_.size() == before;
                if (result.succeeded)
                    for (std::size_t i = 0; i < module_.functions.size(); ++i)
                    {
                        prepare(module_.functions[i]);
                        result.plannedDrops += planDrops(module_.functions[i], flows[i]);
                    }
                return result;
            }

        private:
            AIRModule& module_;
            const NameResolution& names_;
            const TypeCheckResult& types_;
            Diagnostics& diagnostics_;
            std::unordered_map<TypeId, const AIRRecord*> records_;
            std::unordered_map<const Node*, SymbolId> declarations_;
            std::unordered_map<SymbolId, ReferenceReturnSummary> summaries_;
            std::unordered_map<SymbolId, std::map<SymbolId, bool>> globalEffects_;
            std::unordered_map<SymbolId, IdSet> persistEnsures_;
            std::unordered_map<SymbolId, std::map<SymbolId, bool>> initializerEffects_;
            std::unordered_map<SymbolId, bool> unknownEffects_;
            std::unordered_map<SymbolId, bool> unknownInitializerEffects_;
            std::map<std::tuple<SourceId, std::size_t, std::string>, bool> emitted_;
            std::vector<std::vector<IdSet>> liveAfter_;
            std::vector<std::vector<IdSet>> localLiveAfter_;
            std::vector<std::vector<std::size_t>> loanIds_;
            std::vector<std::vector<std::size_t>> sliceContentLoanIds_;
            std::size_t parameterLoanBase_ = 0;
            bool reporting_ = false;

            void collectGlobalEffects()
            {
                bool changed = true;
                while (changed)
                {
                    changed = false;
                    for (const auto& function : module_.functions)
                    {
                        auto effects = globalEffects_[function.symbol];
                        auto ensures = persistEnsures_[function.symbol];
                        bool unknown = unknownEffects_[function.symbol];
                        for (const auto& block : function.blocks)
                            for (const auto& instruction : block.instructions)
                            {
                                if (instruction.opcode == AIROpcode::EnsurePersist) ensures.insert(instruction.place.global);
                                if (instruction.place.global != InvalidSymbolId
                                    && instruction.opcode != AIROpcode::Drop && instruction.opcode != AIROpcode::EnsurePersist)
                                {
                                    const bool write = instruction.opcode == AIROpcode::Write
                                        || instruction.opcode == AIROpcode::Move
                                        || instruction.opcode == AIROpcode::BorrowMut;
                                    auto& effect = effects[instruction.place.global];
                                    effect = effect || write;
                                }
                                if (instruction.opcode == AIROpcode::Call)
                                {
                                    auto targets = instruction.dynamicTargets;
                                    if (targets.empty() && !instruction.operation.starts_with("dyn.call.")) targets.push_back(instruction.callee);
                                    for (const auto target : targets)
                                    {
                                        unknown = unknown || !summaries_.contains(target) || unknownEffects_[target];
                                        for (const auto& [global, write] : globalEffects_[target])
                                        { auto& effect = effects[global]; effect = effect || write; }
                                        unite(ensures, persistEnsures_[target]);
                                    }
                                    if (instruction.operation.starts_with("dyn.call."))
                                    {
                                        auto& combined = globalEffects_[instruction.callee];
                                        for (const auto target : targets)
                                            for (const auto& [global, write] : globalEffects_[target]) combined[global] = combined[global] || write;
                                        for (const auto target : targets) unite(persistEnsures_[instruction.callee], persistEnsures_[target]);
                                        for (const auto target : targets) unknownEffects_[instruction.callee] = unknownEffects_[instruction.callee] || unknownEffects_[target];
                                    }
                                }
                            }
                        if (effects != globalEffects_[function.symbol])
                        {
                            globalEffects_[function.symbol] = std::move(effects);
                            changed = true;
                        }
                        if (unknown != unknownEffects_[function.symbol]) { unknownEffects_[function.symbol] = unknown; changed = true; }
                        if (ensures != persistEnsures_[function.symbol])
                        {
                            persistEnsures_[function.symbol] = std::move(ensures);
                            changed = true;
                        }
                    }
                }
                changed = true;
                while (changed)
                {
                    changed = false;
                    for (const auto& storage : module_.globals)
                    {
                        if (!storage.isPersist) continue;
                        auto effects = globalEffects_[storage.initializerFunction];
                        bool unknown = unknownEffects_[storage.initializerFunction];
                        // 자기 최종 저장은 기존 대여가 존재하면 이미 끝난 초기화이므로 충돌 효과에서 제외한다.
                        // Its final store cannot conflict with an existing loan because that loan already required initialization.
                        effects.erase(storage.symbol);
                        for (const auto nested : persistEnsures_[storage.initializerFunction])
                            if (nested != storage.symbol)
                            {
                                unknown = unknown || unknownInitializerEffects_[nested];
                                for (const auto& [global, write] : initializerEffects_[nested]) effects[global] = effects[global] || write;
                            }
                        if (unknown != unknownInitializerEffects_[storage.symbol]) { unknownInitializerEffects_[storage.symbol] = unknown; changed = true; }
                        if (effects != initializerEffects_[storage.symbol]) { initializerEffects_[storage.symbol] = std::move(effects); changed = true; }
                    }
                }
            }

            void error(SourceRange range, std::string message)
            {
                if (!reporting_) return;
                const auto key = std::make_tuple(range.begin.source, range.begin.offset, message);
                if (emitted_.emplace(key, true).second)
                    diagnostics_.push_back({range, std::move(message)});
            }

            bool copy(TypeId id, std::unordered_set<TypeId>& visiting) const
            {
                const auto& type = types_.types.get(id);
                if (type.kind == TypeKind::String || type.kind == TypeKind::Array || type.kind == TypeKind::Vector || type.kind == TypeKind::Box
                    || type.kind == TypeKind::Void || type.kind == TypeKind::Error) return false;
                if (type.kind == TypeKind::Reference || type.kind == TypeKind::Slice) return !type.isMutable;
                if (type.kind != TypeKind::Nominal) return true;
                if (!visiting.insert(id).second || type.declaration >= names_.symbols.size()) return false;
                const auto* declaration = names_.symbols[type.declaration].declaration;
                bool marker = false;
                if (declaration)
                    for (const auto& group : declaration->children)
                        if (group->kind == "Implements")
                            for (const auto& item : group->children) marker = marker || item->value == "Copy";
                const auto found = records_.find(id);
                if (marker && found != records_.end())
                {
                    for (const auto field : found->second->fieldTypes)
                        if (!copy(field, visiting)) marker = false;
                    for (const auto& variant : found->second->variants)
                        for (const auto field : variant.fieldTypes)
                            if (!copy(field, visiting)) marker = false;
                }
                visiting.erase(id);
                return marker;
            }

            bool copy(TypeId id) const
            {
                std::unordered_set<TypeId> visiting;
                return copy(id, visiting);
            }

            bool owner(TypeId id) const
            {
                const auto kind = types_.types.get(id).kind;
                return !copy(id) && kind != TypeKind::Reference && kind != TypeKind::Void
                    && kind != TypeKind::Error && kind != TypeKind::Never;
            }

            bool containsReference(TypeId id, std::unordered_set<TypeId>& visiting) const
            {
                const auto& type = types_.types.get(id);
                if (type.kind == TypeKind::Reference || type.kind == TypeKind::Slice) return true;
                if (!visiting.insert(id).second) return false;
                bool answer = (type.kind == TypeKind::Array || type.kind == TypeKind::Vector || type.kind == TypeKind::Box) && containsReference(type.element, visiting);
                if (type.kind == TypeKind::Box && types_.types.get(type.element).kind == TypeKind::Dynamic) answer = true;
                if (const auto found = records_.find(id); found != records_.end())
                {
                    for (const auto field : found->second->fieldTypes)
                        answer = answer || containsReference(field, visiting);
                    for (const auto& variant : found->second->variants)
                        for (const auto field : variant.fieldTypes)
                            answer = answer || containsReference(field, visiting);
                }
                visiting.erase(id);
                return answer;
            }

            bool containsReference(TypeId id) const
            {
                std::unordered_set<TypeId> visiting;
                return containsReference(id, visiting);
            }

            void prepare(const AIRFunction& function)
            {
                loanIds_.assign(function.blocks.size(), {});
                std::size_t next = 0;
                for (std::size_t b = 0; b < function.blocks.size(); ++b)
                {
                    loanIds_[b].resize(function.blocks[b].instructions.size());
                    for (auto& id : loanIds_[b]) id = next++;
                }
                sliceContentLoanIds_.assign(function.blocks.size(), {});
                for (std::size_t block = 0; block < function.blocks.size(); ++block)
                {
                    sliceContentLoanIds_[block].resize(function.blocks[block].instructions.size(), InvalidAIRId);
                    for (std::size_t index = 0; index < function.blocks[block].instructions.size(); ++index)
                        if (function.blocks[block].instructions[index].opcode == AIROpcode::SliceConstruct) sliceContentLoanIds_[block][index] = next++;
                }
                parameterLoanBase_ = next;
                // SSA 임시값의 뒤쪽 사용을 구하여 호출용 임시 대여가 블록 끝까지 남지 않게 한다.
                // Compute SSA value liveness so call-only temporary loans do not last to block end.
                std::vector<IdSet> incoming(function.blocks.size()), outgoing(function.blocks.size());
                bool changed = true;
                while (changed)
                {
                    changed = false;
                    for (std::size_t reverse = function.blocks.size(); reverse > 0; --reverse)
                    {
                        const auto b = reverse - 1;
                        IdSet out;
                        for (const auto nextBlock : successors(function.blocks[b]))
                            if (nextBlock < incoming.size()) unite(out, incoming[nextBlock]);
                        auto in = out;
                        const auto& block = function.blocks[b];
                        if (block.terminator.value != InvalidAIRId) in.insert(block.terminator.value);
                        for (auto instruction = block.instructions.rbegin(); instruction != block.instructions.rend(); ++instruction)
                        {
                            if (instruction->result != InvalidAIRId) in.erase(instruction->result);
                            instructionUses(*instruction, in);
                        }
                        if (in != incoming[b] || out != outgoing[b])
                        {
                            incoming[b] = std::move(in);
                            outgoing[b] = std::move(out);
                            changed = true;
                        }
                    }
                }
                liveAfter_.assign(function.blocks.size(), {});
                for (std::size_t b = 0; b < function.blocks.size(); ++b)
                {
                    const auto& block = function.blocks[b];
                    auto live = outgoing[b];
                    if (block.terminator.value != InvalidAIRId) live.insert(block.terminator.value);
                    liveAfter_[b].resize(block.instructions.size());
                    for (std::size_t reverse = block.instructions.size(); reverse > 0; --reverse)
                    {
                        const auto i = reverse - 1;
                        liveAfter_[b][i] = live;
                        if (block.instructions[i].result != InvalidAIRId) live.erase(block.instructions[i].result);
                        instructionUses(block.instructions[i], live);
                    }
                }
                // 참조를 담은 지역값의 마지막 사용도 계산하여 쓸모가 끝난 대여를 해제한다.
                // Track last uses of local reference holders as well, ending loans after their final use.
                std::vector<IdSet> localIn(function.blocks.size()), localOut(function.blocks.size());
                auto localTransfer = [&](const AIRInstruction& instruction, IdSet& live)
                {
                    if (instruction.opcode == AIROpcode::ScopeEnd && instruction.scope < function.scopes.size())
                        for (const auto local : function.scopes[instruction.scope].locals) live.erase(local);
                    const auto local = instruction.place.local;
                    if (local == InvalidAIRId) return;
                    if (instruction.opcode == AIROpcode::Write && instruction.place.projections.empty()) live.erase(local);
                    else if (instruction.opcode != AIROpcode::Drop) live.insert(local);
                };
                changed = true;
                while (changed)
                {
                    changed = false;
                    for (std::size_t reverse = function.blocks.size(); reverse > 0; --reverse)
                    {
                        const auto b = reverse - 1;
                        IdSet out;
                        for (const auto next : successors(function.blocks[b]))
                            if (next < localIn.size()) unite(out, localIn[next]);
                        auto in = out;
                        for (const auto scope : function.blocks[b].terminator.exitedScopes)
                            if (scope < function.scopes.size())
                                for (const auto local : function.scopes[scope].locals) in.erase(local);
                        for (auto instruction = function.blocks[b].instructions.rbegin();
                             instruction != function.blocks[b].instructions.rend(); ++instruction)
                            localTransfer(*instruction, in);
                        if (in != localIn[b] || out != localOut[b])
                        {
                            localIn[b] = std::move(in);
                            localOut[b] = std::move(out);
                            changed = true;
                        }
                    }
                }
                localLiveAfter_.assign(function.blocks.size(), {});
                for (std::size_t b = 0; b < function.blocks.size(); ++b)
                {
                    auto live = localOut[b];
                    for (const auto scope : function.blocks[b].terminator.exitedScopes)
                        if (scope < function.scopes.size())
                            for (const auto local : function.scopes[scope].locals) live.erase(local);
                    localLiveAfter_[b].resize(function.blocks[b].instructions.size());
                    for (std::size_t reverse = function.blocks[b].instructions.size(); reverse > 0; --reverse)
                    {
                        const auto i = reverse - 1;
                        localLiveAfter_[b][i] = live;
                        localTransfer(function.blocks[b].instructions[i], live);
                    }
                }
            }

            State initial(const AIRFunction& function) const
            {
                State state;
                state.live.resize(function.locals.size());
                state.locals.resize(function.locals.size());
                state.values.resize(function.valueTypes.size());
                for (std::size_t p = 0; p < function.parameters.size(); ++p)
                {
                    const auto local = function.parameters[p];
                    if (local >= state.live.size()) continue;
                    state.live[local] = 1;
                    const auto& type = types_.types.get(function.locals[local].type);
                    if (containsReference(function.locals[local].type))
                    {
                        const auto loan = parameterLoanBase_ + p;
                        const auto* source = function.locals[local].source;
                        const bool variadic = source && source->kind == "VariadicParameter";
                        const auto storageOrigin = variadic ? OriginKind::VariadicStorage : OriginKind::Parameter;
                        state.loans[loan] = {type.isMutable, {{storageOrigin, p, {}}}, {}, type.kind == TypeKind::Slice, false};
                        state.locals[local].insert(loan);
                        if (type.kind == TypeKind::Slice && containsReference(type.element))
                        {
                            const auto contents = parameterLoanBase_ + function.parameters.size() + p;
                            state.loans[contents] = {false, {{OriginKind::ParameterElements, p, {}}}, {}, false, true};
                            state.locals[local].insert(contents);
                        }
                    }
                }
                return state;
            }

            IdSet value(const State& state, AIRValueId id) const
            {
                return id < state.values.size() ? state.values[id] : IdSet{};
            }

            void permissionClosure(const State& state, IdSet& permission) const
            {
                bool changed = true;
                while (changed)
                {
                    const auto prior = permission.size();
                    const auto snapshot = permission;
                    for (const auto id : snapshot)
                        if (const auto found = state.loans.find(id); found != state.loans.end())
                            unite(permission, found->second.parents);
                    changed = permission.size() != prior;
                }
            }

            PlaceView place(const AIRPlace& place, const State& state) const
            {
                PlaceView view;
                if (place.pointer != InvalidAIRId)
                {
                    view.permission = value(state, place.pointer);
                    for (const auto id : view.permission)
                        if (const auto found = state.loans.find(id); found != state.loans.end())
                            unite(view.targets, found->second.targets);
                    permissionClosure(state, view.permission);
                }
                else if (place.local != InvalidAIRId)
                    view.targets.insert({OriginKind::Local, place.local, {}});
                else if (place.global != InvalidSymbolId)
                    view.targets.insert({OriginKind::Global, place.global, {}});
                std::set<Target> projected;
                for (auto target : view.targets)
                {
                    for (const auto& projection : place.projections)
                        if (projection.kind != AIRProjectionKind::Dereference)
                            target.projections.emplace_back(projection.kind, projection.field);
                    projected.insert(std::move(target));
                }
                view.targets = std::move(projected);
                return view;
            }

            void access(const AIRInstruction& instruction, const State& state, bool exclusive,
                        bool moving = false)
            {
                const auto view = place(instruction.place, state);
                for (const auto& target : view.targets)
                {
                    if (target.kind == OriginKind::Local && target.id < state.live.size()
                        && state.live[target.id] != 1 && instruction.opcode != AIROpcode::Write)
                        error(instruction.range, "Use of a moved or unavailable value.");
                    if (moving && (!target.projections.empty() || target.kind != OriginKind::Local))
                        error(instruction.range, "Moving an owning value out of a projected or borrowed place is not supported yet.");
                    for (const auto& [id, loan] : state.loans)
                    {
                        if (view.permission.contains(id) || (!exclusive && !loan.exclusive)) continue;
                        for (const auto& borrowed : loan.targets)
                            if (overlaps(target, borrowed))
                            {
                                error(instruction.range, exclusive
                                    ? "Cannot modify, move, or exclusively borrow a place while it is borrowed."
                                    : "Cannot read or share a place while it is exclusively borrowed.");
                                break;
                            }
                    }
                }
            }

            IdSet readProvenance(const AIRFunction& function, const AIRPlace& location, const State& state) const
            {
                if (!containsReference(location.type)) return {};
                IdSet result;
                TypeId base = InvalidTypeId;
                if (location.pointer != InvalidAIRId)
                {
                    if (location.pointer < function.valueTypes.size()) base = types_.types.get(function.valueTypes[location.pointer]).element;
                    const auto view = place(location, state);
                    for (const auto& target : view.targets)
                        if (target.kind == OriginKind::Local && target.id < state.locals.size()) unite(result, state.locals[target.id]);
                        else if (target.kind == OriginKind::Parameter || target.kind == OriginKind::ParameterElements || target.kind == OriginKind::VariadicStorage)
                            unite(result, value(state, location.pointer));
                }
                else if (location.local < state.locals.size())
                { result = state.locals[location.local]; base = function.locals[location.local].type; }
                if (!location.projections.empty() && location.projections.front().kind == AIRProjectionKind::Index
                    && types_.types.get(base).kind == TypeKind::Slice)
                {
                    // 원소 안의 참조를 읽어도 호출 프레임의 pack 저장소를 대여하지 않는다.
                    // Reading a reference stored in an element does not borrow the call-frame pack storage.
                    IdSet contents;
                    for (const auto loan : result)
                        if (const auto found = state.loans.find(loan); found != state.loans.end() && found->second.sliceContents) contents.insert(loan);
                    if (!contents.empty()) return contents;
                }
                return result;
            }

            bool ancestor(const AIRFunction& function, AIRScopeId outer, AIRScopeId inner) const
            {
                for (std::size_t limit = 0; inner < function.scopes.size() && limit <= function.scopes.size(); ++limit)
                {
                    if (outer == inner) return true;
                    inner = function.scopes[inner].parent;
                }
                return false;
            }

            void checkStorage(const AIRFunction& function, const AIRInstruction& instruction,
                              const State& state, const IdSet& provenance)
            {
                const auto destination = place(instruction.place, state);
                for (const auto loanId : provenance)
                {
                    const auto loan = state.loans.find(loanId);
                    if (loan == state.loans.end()) continue;
                    for (const auto& origin : loan->second.targets)
                    {
                        for (const auto& target : destination.targets)
                        {
                            if (target.kind != OriginKind::Local && origin.kind != OriginKind::Global)
                                error(instruction.range, "Cannot store a borrowed input or local reference in escaping storage.");
                            if (origin.kind == OriginKind::Local && origin.id < function.locals.size()
                                && (target.kind != OriginKind::Local || target.id >= function.locals.size()
                                    || !ancestor(function, function.locals[origin.id].scope,
                                                 function.locals[target.id].scope)))
                                error(instruction.range, "A reference cannot outlive the local value it borrows.");
                        }
                    }
                }
            }

            void endScope(const AIRFunction& function, AIRScopeId scope, State& state)
            {
                if (scope >= function.scopes.size()) return;
                for (const auto local : function.scopes[scope].locals)
                    if (local < state.live.size())
                    {
                        state.live[local] = 0;
                        state.locals[local].clear();
                        std::erase_if(state.extractedFields, [&](const auto& field) { return std::get<0>(field) == local; });
                    }
            }

            void prune(State& state, const IdSet& liveValues, const IdSet& liveLocals) const
            {
                IdSet retained;
                for (std::size_t i = 0; i < state.live.size(); ++i)
                    if ((state.live[i] & 1) != 0 && liveLocals.contains(i)) unite(retained, state.locals[i]);
                for (const auto id : liveValues)
                    if (id < state.values.size()) unite(retained, state.values[id]);
                permissionClosure(state, retained);
                std::erase_if(state.loans, [&](const auto& item) { return !retained.contains(item.first); });
            }

            void initializationEffects(const AIRInstruction& instruction, SymbolId storage, State& state)
            {
                if (state.initializedPersist.contains(storage)) return;
                if (unknownInitializerEffects_[storage] && !state.loans.empty())
                    error(instruction.range, "Cannot initialize persist through an external function while loans are active.");
                for (const auto& [global, write] : initializerEffects_[storage])
                {
                    auto effect = instruction;
                    effect.place = {}; effect.place.global = global;
                    access(effect, state, write);
                }
            }
            void transfer(const AIRFunction& function, std::size_t block, std::size_t index, State& state)
            {
                const auto& instruction = function.blocks[block].instructions[index];
                IdSet result;
                if (instruction.opcode == AIROpcode::Read || instruction.opcode == AIROpcode::Move)
                {
                    if (instruction.place.global != InvalidSymbolId && containsReference(instruction.type))
                        error(instruction.range, "Reading global reference-containing values requires initialization provenance analysis.");
                    const bool moved = instruction.opcode == AIROpcode::Move && !copy(instruction.type);
                    const bool extract = moved && instruction.operation == "enum.extract"
                        && instruction.place.pointer == InvalidAIRId
                        && instruction.place.local < function.locals.size()
                        && function.locals[instruction.place.local].name == "$enumselector"
                        && instruction.place.projections.size() == 1
                        && instruction.place.projections.front().kind == AIRProjectionKind::EnumField;
                    access(instruction, state, moved, moved && !extract);
                    if (extract)
                    {
                        const auto& field = instruction.place.projections.front();
                        const auto key = std::make_tuple(instruction.place.local, field.variant, field.field);
                        if (!state.extractedFields.insert(key).second)
                            error(instruction.range, "An enum payload field cannot be moved more than once.");
                    }
                    result = readProvenance(function, instruction.place, state);
                    if (moved && instruction.place.local < state.live.size()
                        && instruction.place.projections.empty() && instruction.place.pointer == InvalidAIRId)
                    {
                        state.live[instruction.place.local] = 2;
                        state.locals[instruction.place.local].clear();
                    }
                }
                else if (instruction.opcode == AIROpcode::BorrowShared || instruction.opcode == AIROpcode::BorrowMut || instruction.opcode == AIROpcode::SliceConstruct)
                {
                    const bool exclusive = instruction.opcode == AIROpcode::BorrowMut;
                    access(instruction, state, exclusive);
                    const auto target = place(instruction.place, state);
                    const auto id = loanIds_[block][index];
                    const bool slice = instruction.opcode == AIROpcode::SliceConstruct;
                    state.loans[id] = {exclusive, target.targets, target.permission, slice, false};
                    result.insert(id);
                    if (slice && containsReference(types_.types.get(instruction.type).element))
                    {
                        Loan contents; contents.sliceContents = true;
                        const auto values = readProvenance(function, instruction.place, state);
                        for (const auto source : values)
                            if (const auto found = state.loans.find(source); found != state.loans.end())
                            { unite(contents.targets, found->second.targets); contents.parents.insert(source); }
                        const auto contentsId = sliceContentLoanIds_[block][index];
                        state.loans[contentsId] = std::move(contents); result.insert(contentsId);
                    }
                }
                else if (instruction.opcode == AIROpcode::Write)
                {
                    access(instruction, state, true);
                    if (owner(instruction.place.type)
                        && (!instruction.place.projections.empty() || instruction.place.pointer != InvalidAIRId))
                        error(instruction.range, "Replacing an owning projected or borrowed value requires partial cleanup support.");
                    if (!instruction.operands.empty()) result = value(state, instruction.operands.front());
                    checkStorage(function, instruction, state, result);
                    const auto target = place(instruction.place, state);
                    for (const auto& item : target.targets)
                        if (item.kind == OriginKind::Local && item.id < state.live.size())
                        {
                            if (item.projections.empty())
                            {
                                state.live[item.id] = 1;
                                state.locals[item.id] = result;
                                std::erase_if(state.extractedFields, [&](const auto& field) { return std::get<0>(field) == item.id; });
                            }
                            else unite(state.locals[item.id], result);
                        }
                }
                else if (instruction.opcode == AIROpcode::VectorAppend || instruction.opcode == AIROpcode::VectorReserve)
                {
                    access(instruction, state, true);
                    for (const auto operand : instruction.operands) unite(result, value(state, operand));
                    checkStorage(function, instruction, state, result);
                    const auto target = place(instruction.place, state);
                    for (const auto& item : target.targets)
                        if (item.kind == OriginKind::Local && item.id < state.locals.size()) unite(state.locals[item.id], result);
                }
                else if (instruction.opcode == AIROpcode::EnsurePersist)
                {
                    initializationEffects(instruction, instruction.place.global, state);
                    state.initializedPersist.insert(instruction.place.global);
                }
                else if (instruction.opcode == AIROpcode::Call)
                {
                    for (const auto storage : persistEnsures_[instruction.callee]) initializationEffects(instruction, storage, state);
                    if (unknownEffects_[instruction.callee] && !state.loans.empty())
                        error(instruction.range, "Cannot call a function with unknown external effects while loans are active.");
                    // 전역 접근의 호출 그래프 요약도 현재 대여와 비교하여 간접 충돌을 막는다.
                    // Compare transitive global access effects with active loans to reject indirect conflicts.
                    for (const auto& [global, write] : globalEffects_[instruction.callee])
                    {
                        auto effect = instruction;
                        effect.place = {};
                        effect.place.global = global;
                        access(effect, state, write);
                    }
                    const auto found = summaries_.find(instruction.callee);
                    if (found != summaries_.end())
                    {
                        for (const auto parameter : found->second.parameters)
                            if (parameter < instruction.operands.size())
                                unite(result, value(state, instruction.operands[parameter]));
                        for (const auto parameter : found->second.elementParameters)
                            if (parameter < instruction.operands.size())
                                for (const auto loan : value(state, instruction.operands[parameter]))
                                    if (const auto source = state.loans.find(loan); source != state.loans.end() && source->second.sliceContents) result.insert(loan);
                        if (!found->second.globals.empty())
                        {
                            const auto id = loanIds_[block][index];
                            Loan loan;
                            const auto& returnedType = types_.types.get(instruction.type);
                            loan.exclusive = returnedType.kind != TypeKind::Reference || returnedType.isMutable;
                            for (const auto global : found->second.globals)
                                loan.targets.insert({OriginKind::Global, global, {}});
                            state.loans[id] = std::move(loan);
                            result.insert(id);
                        }
                    }
                    else if (!state.loans.empty() || containsReference(instruction.type))
                        error(instruction.range, "Cannot call a function without ownership effects while loans are active.");

                }
                else if (instruction.opcode == AIROpcode::Aggregate || instruction.opcode == AIROpcode::EnumConstruct
                    || instruction.opcode == AIROpcode::Unary)
                {
                    // 길이 같은 스칼라 계산 결과에는 원본 뷰의 대여 수명이 포함되지 않는다.
                    // Scalar computations such as lengths do not retain their source view lifetime.
                    if (instruction.opcode != AIROpcode::Unary || containsReference(instruction.type))
                        for (const auto operand : instruction.operands) unite(result, value(state, operand));
                }
                else if (instruction.opcode == AIROpcode::ScopeEnd)
                    endScope(function, instruction.scope, state);
                else if (instruction.opcode == AIROpcode::EnumTag)
                    access(instruction, state, false);
                else if (instruction.opcode == AIROpcode::Drop)
                {
                    if (instruction.place.local < state.live.size())
                    {
                        state.live[instruction.place.local] = 0;
                        state.locals[instruction.place.local].clear();
                    }
                }
                if (instruction.result < state.values.size()) state.values[instruction.result] = std::move(result);
                prune(state, liveAfter_[block][index], localLiveAfter_[block][index]);
            }

            void returned(const AIRFunction& function, const AIRTerminator& terminator,
                          const State& state, ReferenceReturnSummary& summary)
            {
                for (const auto id : value(state, terminator.value))
                {
                    const auto loan = state.loans.find(id);
                    if (loan == state.loans.end()) continue;
                    for (const auto& origin : loan->second.targets)
                    {
                        if (origin.kind == OriginKind::Local)
                            error(terminator.range, "Cannot return a reference to a local value.");
                        else if (origin.kind == OriginKind::VariadicStorage)
                            error(terminator.range, "Cannot return a view or reference into variadic call-frame storage.");
                        else if (origin.kind == OriginKind::ParameterElements)
                            summary.elementParameters.insert(origin.id);
                        else if (origin.kind == OriginKind::Parameter)
                            summary.parameters.insert(origin.id);
                        else
                        {
                            summary.global = true;
                            summary.globals.insert(origin.id);
                        }
                    }
                }
                (void)function;
            }

            FunctionFlow analyze(const AIRFunction& function, bool report)
            {
                FunctionFlow flow;
                flow.incoming.resize(function.blocks.size());
                if (function.blocks.empty()) return flow;
                flow.incoming[0] = initial(function);
                std::deque<AIRBlockId> pending{0};
                std::vector<bool> queued(function.blocks.size());
                queued[0] = true;
                reporting_ = false;
                while (!pending.empty())
                {
                    const auto block = pending.front(); pending.pop_front(); queued[block] = false;
                    auto state = *flow.incoming[block];
                    for (std::size_t i = 0; i < function.blocks[block].instructions.size(); ++i)
                        transfer(function, block, i, state);
                    for (const auto scope : function.blocks[block].terminator.exitedScopes)
                        endScope(function, scope, state);
                    for (const auto next : successors(function.blocks[block]))
                    {
                        if (next >= function.blocks.size()) continue;
                        const auto joined = flow.incoming[next] ? merge(*flow.incoming[next], state) : state;
                        if (!flow.incoming[next] || *flow.incoming[next] != joined)
                        {
                            flow.incoming[next] = joined;
                            if (!queued[next]) { pending.push_back(next); queued[next] = true; }
                        }
                    }
                }
                reporting_ = report;
                for (std::size_t b = 0; b < function.blocks.size(); ++b)
                {
                    if (!flow.incoming[b]) continue;
                    auto state = *flow.incoming[b];
                    for (std::size_t i = 0; i < function.blocks[b].instructions.size(); ++i)
                        transfer(function, b, i, state);
                    if (function.blocks[b].terminator.kind == AIRTerminatorKind::Return)
                        returned(function, function.blocks[b].terminator, state, flow.summary);
                }
                return flow;
            }

            std::size_t planDrops(AIRFunction& function, const FunctionFlow& flow)
            {
                std::size_t count = 0;
                for (std::size_t b = 0; b < function.blocks.size(); ++b)
                {
                    if (!flow.incoming[b]) continue;
                    auto state = *flow.incoming[b];
                    auto appendLocal = [&](std::vector<AIRInstruction>& output, AIRLocalId local,
                                           AIRScopeId scope, SourceRange range)
                    {
                        if (local >= function.locals.size() || (state.live[local] & 1) == 0
                            || !owner(function.locals[local].type)) return;
                        AIRInstruction drop;
                        drop.opcode = AIROpcode::Drop;
                        drop.place.local = local;
                        drop.place.type = function.locals[local].type;
                        drop.type = function.locals[local].type;
                        drop.operation = "ownership.cleanup";
                        drop.scope = scope;
                        drop.range = range;
                        output.push_back(std::move(drop));
                        ++count;
                    };
                    auto appendScope = [&](std::vector<AIRInstruction>& output, AIRScopeId scope, SourceRange range)
                    {
                        if (scope >= function.scopes.size()) return;
                        const auto& locals = function.scopes[scope].locals;
                        for (auto local = locals.rbegin(); local != locals.rend(); ++local)
                            appendLocal(output, *local, scope, range);
                    };
                    std::vector<AIRInstruction> instructions;
                    for (std::size_t i = 0; i < function.blocks[b].instructions.size(); ++i)
                    {
                        const auto& instruction = function.blocks[b].instructions[i];
                        if (instruction.opcode == AIROpcode::ScopeEnd)
                            appendScope(instructions, instruction.scope, instruction.range);
                        if (instruction.opcode == AIROpcode::Write
                            && instruction.place.pointer == InvalidAIRId
                            && instruction.place.projections.empty())
                            appendLocal(instructions, instruction.place.local, instruction.scope, instruction.range);
                        transfer(function, b, i, state);
                        instructions.push_back(instruction);
                    }
                    // 조기 반환과 반복 제어의 이탈 스코프는 하강 단계가 안쪽부터 기록한다.
                    // Lowering records scopes exited by returns and loop control from inner to outer.
                    for (const auto scope : function.blocks[b].terminator.exitedScopes)
                        appendScope(instructions, scope, function.blocks[b].terminator.range);
                    function.blocks[b].instructions = std::move(instructions);
                }
                return count;
            }
        };
    }

    BorrowChecker::BorrowChecker(Diagnostics& diagnostics) : diagnostics_(diagnostics) {}

    OwnershipResult BorrowChecker::check(AIRModule& module, const NameResolution& names,
                                         const TypeCheckResult& types)
    {
        return Checker(module, names, types, diagnostics_).run();
    }
}
