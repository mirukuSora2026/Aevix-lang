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
#include "aevix/Native/SyncOperation.hpp"
#include "aevix/Native/TaskOperation.hpp"

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
        using ProvenanceFields = std::map<ReferencePath, IdSet>;
        constexpr std::size_t maximumProvenanceEntries = 4096;
        constexpr std::size_t maximumProvenanceDepth = 64;

        enum class OriginKind { Local, Global, Parameter, ParameterContents, ParameterElements, VariadicStorage };
        struct Target final
        {
            OriginKind kind = OriginKind::Local;
            std::size_t id = InvalidAIRId;
            // 필드 번호는 서로 구별하지만 실행 중 인덱스는 보수적으로 겹친다고 본다.
            // Field indices are distinct, while runtime indices conservatively overlap.
            ReferencePath projections;
            ReferencePath storageProjection{};
            bool opaqueStorage = false;
            auto operator<=>(const Target&) const = default;
        };
        struct Loan final
        {
            bool exclusive = false;
            std::set<Target> targets;
            IdSet parents;
            bool sliceStorage = false;
            bool sliceContents = false;
            bool separateParents = false;
            bool operator==(const Loan&) const = default;
        };
        struct State final
        {
            // 1은 살아 있을 가능성, 2는 이동되었을 가능성이며 병합 시 두 비트를 합친다.
            // Bit 1 means possibly live and bit 2 possibly moved; joins combine both bits.
            std::vector<unsigned char> live;
            std::vector<IdSet> locals;
            std::vector<IdSet> values;
            std::vector<IdSet> parameterContents;
            std::vector<ProvenanceFields> localFields;
            std::vector<ProvenanceFields> valueFields;
            std::vector<ProvenanceFields> parameterFields;
            std::map<ReferenceParameterPath, IdSet> indirectStores;
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

        void mergeFields(ProvenanceFields& target, const ProvenanceFields& source)
        { for (const auto& [path, values] : source) unite(target[path], values); }

        IdSet flatten(const ProvenanceFields& fields)
        { IdSet result; for (const auto& [path, values] : fields) { (void)path; unite(result, values); } return result; }

        ProvenanceFields fieldFallback(const ProvenanceFields& fields, const IdSet& values)
        { return fields.empty() && !values.empty() ? ProvenanceFields{{{}, values}} : fields; }

        bool sameProjection(const ReferenceProjection& left, const ReferenceProjection& right)
        {
            return left.kind == right.kind && (left.kind == AIRProjectionKind::Index || left.field == right.field)
                && (left.kind != AIRProjectionKind::EnumField || left.variant == InvalidSymbolId
                    || right.variant == InvalidSymbolId || left.variant == right.variant);
        }

        ProvenanceFields selectFields(const ProvenanceFields& fields, const ReferencePath& selected)
        {
            ProvenanceFields result;
            for (const auto& [path, values] : fields)
            {
                bool matches = true;
                for (std::size_t i = 0; i < std::min(path.size(), selected.size()); ++i)
                    if (!sameProjection(path[i], selected[i])) { matches = false; break; }
                if (!matches) continue;
                ReferencePath remainder;
                if (path.size() > selected.size()) remainder.assign(path.begin() + selected.size(), path.end());
                unite(result[remainder], values);
            }
            return result;
        }

        ReferencePath projectionPath(const std::vector<AIRProjection>& projections)
        {
            ReferencePath result;
            for (const auto& projection : projections)
                if (projection.kind != AIRProjectionKind::Dereference)
                    result.push_back({projection.kind, projection.field, projection.variant});
            return result;
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
            for (std::size_t i = 0; i < result.parameterContents.size(); ++i)
                unite(result.parameterContents[i], other.parameterContents[i]);
            for (std::size_t i = 0; i < result.localFields.size(); ++i) mergeFields(result.localFields[i], other.localFields[i]);
            for (std::size_t i = 0; i < result.valueFields.size(); ++i) mergeFields(result.valueFields[i], other.valueFields[i]);
            for (std::size_t i = 0; i < result.parameterFields.size(); ++i) mergeFields(result.parameterFields[i], other.parameterFields[i]);
            for (const auto& [path, values] : other.indirectStores) unite(result.indirectStores[path], values);
            for (const auto& [id, loan] : other.loans)
            {
                auto& merged = result.loans[id];
                merged.exclusive = merged.exclusive || loan.exclusive;
                merged.sliceStorage = merged.sliceStorage || loan.sliceStorage;
                merged.sliceContents = merged.sliceContents || loan.sliceContents;
                merged.separateParents = merged.separateParents || loan.separateParents;
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
            const auto sourceCount = std::min(left.projections.size(), right.projections.size());
            const bool sourcePrefix = std::equal(left.projections.begin(), left.projections.begin() + sourceCount, right.projections.begin());
            if (sourcePrefix && left.projections.size() != right.projections.size())
            {
                const auto& longer = left.projections.size() > right.projections.size() ? left.projections : right.projections;
                // 참조를 따라간 다음 할당의 원천은 참조를 보관한 원래 저장소와 별개다.
                // A referent reached across a reference boundary is distinct from the storage holding that reference.
                if (longer[sourceCount].kind == AIRProjectionKind::Dereference) return false;
            }
            auto leftPath = left.projections, rightPath = right.projections;
            leftPath.insert(leftPath.end(), left.storageProjection.begin(), left.storageProjection.end());
            rightPath.insert(rightPath.end(), right.storageProjection.begin(), right.storageProjection.end());
            const auto count = std::min(leftPath.size(), rightPath.size());
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto [lk, li, lv] = leftPath[i];
                const auto [rk, ri, rv] = rightPath[i];
                if (lk == AIRProjectionKind::EnumField && rk == lk && lv != InvalidSymbolId && rv != InvalidSymbolId && lv != rv) return false;
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
                for (const auto& function : module_.functions)
                {
                    auto& summary = summaries_[function.symbol];
                    // 빈 효과에서 시작해야 반드시 비우는 함수의 내용 출처가 되살아나지 않는다.
                    // Start at the least effect so a definite clearing function cannot retain stale contents.
                    for (std::size_t p = 0; p < function.parameters.size(); ++p)
                        if (mutableContentsParameter(function, p)) summary.stores[p] = {};
                }
                for (const auto& function : module_.functions) for (const auto& block : function.blocks) for (const auto& instruction : block.instructions)
                    if (instruction.opcode == AIROpcode::Call && instruction.operation.starts_with("dyn.call.")) summaries_[instruction.callee] = {};
                // 정리를 먼저 명시해 소멸자의 대여와 호출 효과도 일반 제어 흐름과 함께 검사한다.
                // Materialize cleanup first so destructor loans and call effects participate in normal flow analysis.
                std::size_t plannedDrops = 0;
                for (auto& function : module_.functions)
                {
                    prepare(function);
                    const auto flow = analyze(function, false);
                    if (analysisLimit_) return {};
                    plannedDrops += planDrops(function, flow);
                }
                collectGlobalEffects();
                validatePersistDrops();
                // 반환 원천 요약은 호출 그래프의 순환과 무관하게 유한 집합의 고정점으로 구한다.
                // Compute returned-origin summaries as a finite-set fixed point, including recursion.
                bool changed = true;
                while (changed && !analysisLimit_)
                {
                    changed = false;
                    for (const auto& function : module_.functions) for (const auto& block : function.blocks) for (const auto& instruction : block.instructions)
                        if (instruction.opcode == AIROpcode::Call && instruction.operation.starts_with("dyn.call."))
                        {
                            auto& summary = summaries_[instruction.callee]; const auto previous = summary;
                            for (const auto target : instruction.dynamicTargets)
                            {
                                mergeSummary(summary, summaries_[target]);
                            }
                            changed = changed || summary != previous;
                        }
                    for (auto& function : module_.functions)
                    {
                        prepare(function);
                        const auto flow = analyze(function, false);
                        if (analysisLimit_) return {};
                        auto& summary = summaries_[function.symbol];
                        const auto prior = summary;
                        mergeSummary(summary, flow.summary);
                        std::size_t entries = summary.fields.size() + summary.fieldCapabilities.size() + summary.indirectStores.size()
                            + summary.parameterPaths.size() + summary.contentPaths.size();
                        for (const auto& [parameter, fields] : summary.storeFields) { (void)parameter; entries += fields.size(); }
                        if (entries > maximumProvenanceEntries) { provenanceLimit(); return {}; }
                        changed = changed || summary != prior;
                    }
                }
                for (auto& function : module_.functions)
                {
                    prepare(function);
                    analyze(function, true);
                    if (analysisLimit_) return {};
                }
                OwnershipResult result;
                result.returns = summaries_;
                result.succeeded = diagnostics_.size() == before;
                if (result.succeeded) result.plannedDrops = plannedDrops;
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
            mutable std::unordered_map<TypeId, IdSet> dropTargets_;
            std::map<std::tuple<SourceId, std::size_t, std::string>, bool> emitted_;
            std::vector<std::vector<IdSet>> liveAfter_;
            std::vector<std::vector<IdSet>> localLiveAfter_;
            std::vector<std::vector<std::size_t>> loanIds_;
            std::vector<std::vector<std::size_t>> sliceContentLoanIds_;
            std::size_t parameterLoanBase_ = 0;
            bool reporting_ = false;
            std::map<Target, std::size_t> symbolicLoans_;
            std::size_t nextSymbolicLoan_ = 0;
            std::map<std::pair<AIRValueId, ReferencePath>, std::size_t> returnedLoanIds_;
            const AIRFunction* currentFunction_ = nullptr;
            bool analysisLimit_ = false;

            static void mergeOrigins(ReferenceOrigins& target, const ReferenceOrigins& source)
            {
                unite(target.parameters, source.parameters);
                unite(target.contentParameters, source.contentParameters);
                unite(target.elementParameters, source.elementParameters);
                unite(target.parameterPaths, source.parameterPaths);
                unite(target.contentPaths, source.contentPaths);
                unite(target.globals, source.globals);
                target.global = target.global || source.global;
            }

            static void mergeSummary(ReferenceReturnSummary& target, const ReferenceReturnSummary& source)
            {
                mergeOrigins(target, source);
                for (const auto& [parameter, origins] : source.stores) mergeOrigins(target.stores[parameter], origins);
                for (const auto& [path, origins] : source.fields) mergeOrigins(target.fields[path], origins);
                for (const auto& [parameter, fields] : source.storeFields)
                    for (const auto& [path, origins] : fields) mergeOrigins(target.storeFields[parameter][path], origins);
                for (const auto& [path, capability] : source.fieldCapabilities)
                {
                    mergeOrigins(target.fieldCapabilities[path].parents, capability.parents);
                    target.fieldCapabilities[path].exclusive = target.fieldCapabilities[path].exclusive || capability.exclusive;
                }
                for (const auto& [path, origins] : source.indirectStores) mergeOrigins(target.indirectStores[path], origins);
                mergeOrigins(target.retainedParents, source.retainedParents);
                target.derivedCapability = target.derivedCapability || source.derivedCapability;
                target.targetExclusive = target.targetExclusive || source.targetExclusive;
            }

            bool mutableContentsParameter(const AIRFunction& function, std::size_t parameter) const
            {
                if (parameter >= function.parameters.size()) return false;
                const auto local = function.parameters[parameter];
                if (local >= function.locals.size()) return false;
                const auto& type = types_.types.get(function.locals[local].type);
                return (type.kind == TypeKind::Reference || type.kind == TypeKind::Slice)
                    && type.isMutable && containsReference(type.element);
            }

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
                                        || instruction.opcode == AIROpcode::BorrowMut || instruction.opcode == AIROpcode::VectorTake
                                        || instruction.opcode == AIROpcode::VectorTruncate || instruction.opcode == AIROpcode::VectorInsert;
                                    auto& effect = effects[instruction.place.global];
                                    effect = effect || write;
                                }
                                if (instruction.opcode == AIROpcode::Call || instruction.opcode == AIROpcode::Drop || instruction.opcode == AIROpcode::VectorTruncate)
                                {
                                    std::vector<SymbolId> targets;
                                    if (instruction.opcode == AIROpcode::Drop || instruction.opcode == AIROpcode::VectorTruncate)
                                    {
                                        const auto cleanupType = instruction.opcode == AIROpcode::VectorTruncate
                                            ? types_.types.get(instruction.place.type).element : instruction.place.type;
                                        const auto& cleanup = dropTargets(cleanupType);
                                        targets.assign(cleanup.begin(), cleanup.end());
                                    }
                                    else
                                    {
                                        targets = instruction.dynamicTargets;
                                        if (targets.empty() && !instruction.operation.starts_with("dyn.call.")) targets.push_back(instruction.callee);
                                    }
                                    for (const auto target : targets)
                                    {
                                        unknown = unknown || !summaries_.contains(target) || unknownEffects_[target];
                                        for (const auto& [global, write] : globalEffects_[target])
                                        { auto& effect = effects[global]; effect = effect || write; }
                                        unite(ensures, persistEnsures_[target]);
                                    }
                                    if (instruction.opcode == AIROpcode::Call && instruction.operation.starts_with("dyn.call."))
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
                if (types_.dropMethods.contains(id)) return false;
                if (syncOwner(type.kind) || syncGuard(type.kind) || taskOwner(type.kind)) return false;
                if (type.kind == TypeKind::String || type.kind == TypeKind::Array || type.kind == TypeKind::Vector || type.kind == TypeKind::Box || type.kind == TypeKind::Shared || type.kind == TypeKind::Weak || type.kind == TypeKind::Atomic
                    || type.kind == TypeKind::CancellationSource || type.kind == TypeKind::CancellationToken
                    || type.kind == TypeKind::Tuple || type.kind == TypeKind::Void || type.kind == TypeKind::Error) return false;
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

            const IdSet& dropTargets(TypeId id) const
            {
                if (const auto found = dropTargets_.find(id); found != dropTargets_.end()) return found->second;
                IdSet targets;
                std::unordered_set<TypeId> visited;
                auto visit = [&](auto&& self, TypeId current) -> void
                {
                    if (!visited.insert(current).second) return;
                    const auto& type = types_.types.get(current);
                    if (const auto method = types_.dropMethods.find(current); method != types_.dropMethods.end()) targets.insert(method->second);
                    if (type.kind == TypeKind::Vector || (type.kind == TypeKind::Array && type.extent != 0)) self(self, type.element);
                    else if (type.kind == TypeKind::Tuple)
                        for (const auto field : type.parameters) self(self, field);
                    else if (type.kind == TypeKind::Shared || syncOwner(type.kind)) self(self, type.element);
                    else if (type.kind == TypeKind::Box)
                    {
                        if (types_.types.get(type.element).kind != TypeKind::Dynamic) self(self, type.element);
                        else
                        {
                            // 동적 소유자의 실제 구현 후보도 소멸 효과와 대여 유지에 포함한다.
                            // Include concrete dynamic-owner candidates in destructor effects and retained loans.
                            for (const auto& [node, view] : types_.boxDynViews)
                            { (void)node; if (view.interfaceType == type.element) self(self, view.concreteType); }
                            for (const auto& [node, view] : types_.dynViews)
                            { (void)node; if (view.interfaceType == type.element) self(self, view.concreteType); }
                        }
                    }
                    if (const auto record = records_.find(current); record != records_.end())
                    {
                        for (const auto field : record->second->fieldTypes) self(self, field);
                        for (const auto& variant : record->second->variants)
                            for (const auto field : variant.fieldTypes) self(self, field);
                    }
                };
                visit(visit, id);
                return dropTargets_.emplace(id, std::move(targets)).first->second;
            }

            void validatePersistDrops()
            {
                reporting_ = true;
                for (const auto& storage : module_.globals)
                {
                    if (!storage.isPersist) continue;
                    for (const auto method : dropTargets(storage.type))
                    {
                        bool touchesPersist = !persistEnsures_[method].empty();
                        for (const auto& [global, write] : globalEffects_[method])
                        {
                            (void)write;
                            for (const auto& candidate : module_.globals)
                                if (candidate.symbol == global && candidate.isPersist) touchesPersist = true;
                        }
                        // 외부 정리가 다른 persist를 건드리지 않는다는 증명은 향후 제한된 FFI 계약이 제공해야 한다.
                        // A future bounded FFI contract must prove external cleanup cannot touch other persist storage.
                        if (touchesPersist || unknownEffects_[method])
                            error(storage.source ? storage.source->range : SourceRange{},
                                "A persist destructor cannot access persist storage or call functions with unknown cleanup effects.");
                    }
                }
                reporting_ = false;
            }

            bool nativeCleanup(TypeId id, bool retainedOnly, std::unordered_set<TypeId>& visited) const
            {
                if (!visited.insert(id).second) return false;
                const auto& type = types_.types.get(id);
                if (syncGuard(type.kind) || type.kind == TypeKind::Task) return true;
                if ((syncOwner(type.kind) || type.kind == TypeKind::CancellationSource || type.kind == TypeKind::CancellationToken
                    || type.kind == TypeKind::TaskScope) && !retainedOnly) return true;
                if ((syncOwner(type.kind) || type.kind == TypeKind::Shared || type.kind == TypeKind::Box
                    || type.kind == TypeKind::Vector || type.kind == TypeKind::Array) && nativeCleanup(type.element, retainedOnly, visited)) return true;
                if (type.kind == TypeKind::Tuple)
                    for (const auto field : type.parameters) if (nativeCleanup(field, retainedOnly, visited)) return true;
                if (const auto record = records_.find(id); record != records_.end())
                {
                    for (const auto field : record->second->fieldTypes) if (nativeCleanup(field, retainedOnly, visited)) return true;
                    for (const auto& variant : record->second->variants)
                        for (const auto field : variant.fieldTypes) if (nativeCleanup(field, retainedOnly, visited)) return true;
                }
                return false;
            }
            bool nativeCleanup(TypeId id, bool retainedOnly = false) const
            { std::unordered_set<TypeId> visited; return nativeCleanup(id, retainedOnly, visited); }
            bool containsReference(TypeId id, std::unordered_set<TypeId>& visiting) const
            {
                const auto& type = types_.types.get(id);
                // 관측 핸들의 실제 저장소가 정수여도 자식이 속한 스코프의 수명을 대여한다.
                // Even an integer-backed observation handle borrows its child's owning scope lifetime.
                if (type.kind == TypeKind::Reference || type.kind == TypeKind::Slice || type.kind == TypeKind::StringView || type.kind == TypeKind::Dynamic || syncGuard(type.kind)
                    || type.kind == TypeKind::Task) return true;
                if (!visiting.insert(id).second) return false;
                bool answer = (type.kind == TypeKind::Array || type.kind == TypeKind::Vector || type.kind == TypeKind::Box || type.kind == TypeKind::Shared || type.kind == TypeKind::Weak || syncOwner(type.kind)) && containsReference(type.element, visiting);
                if (type.kind == TypeKind::Tuple) for (const auto field : type.parameters) answer = answer || containsReference(field, visiting);
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

            bool containsMutableReference(TypeId id, std::unordered_set<TypeId>& visited) const
            {
                if (!visited.insert(id).second) return false;
                const auto& type = types_.types.get(id);
                if (syncGuard(type.kind)) return true;
                if (type.kind == TypeKind::Reference || type.kind == TypeKind::Slice)
                    return type.isMutable || containsMutableReference(type.element, visited);
                if ((type.kind == TypeKind::Array || type.kind == TypeKind::Vector || type.kind == TypeKind::Box || type.kind == TypeKind::Shared || type.kind == TypeKind::Weak || syncOwner(type.kind))
                    && containsMutableReference(type.element, visited)) return true;
                if (type.kind == TypeKind::Box && types_.types.get(type.element).kind == TypeKind::Dynamic) return true;
                if (type.kind == TypeKind::Tuple)
                    for (const auto field : type.parameters) if (containsMutableReference(field, visited)) return true;
                if (const auto record = records_.find(id); record != records_.end())
                {
                    for (const auto field : record->second->fieldTypes) if (containsMutableReference(field, visited)) return true;
                    for (const auto& variant : record->second->variants)
                        for (const auto field : variant.fieldTypes) if (containsMutableReference(field, visited)) return true;
                }
                return false;
            }

            void prepare(const AIRFunction& function)
            {
                currentFunction_ = &function;
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
                symbolicLoans_.clear(); returnedLoanIds_.clear(); nextSymbolicLoan_ = next + function.parameters.size() * 2;
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
                    if (instruction.opcode == AIROpcode::ClosureConsume)
                        for (const auto destination : instruction.closureLocals) live.erase(destination);
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

            TypeId pathType(TypeId type, const ReferencePath& path) const
            {
                for (const auto& projection : path)
                {
                    const auto& current = types_.types.get(type);
                    if (projection.kind == AIRProjectionKind::Dereference)
                    {
                        const bool ownedPayload = projection.field == 1 && (current.kind == TypeKind::Box || current.kind == TypeKind::Shared || current.kind == TypeKind::Weak);
                        if (current.kind != TypeKind::Reference && !ownedPayload && !syncGuard(current.kind)) return InvalidTypeId;
                        type = current.element;
                    }
                    else if (projection.kind == AIRProjectionKind::Index)
                    {
                        if (current.kind != TypeKind::Array && current.kind != TypeKind::Vector && current.kind != TypeKind::Slice) return InvalidTypeId;
                        type = current.element;
                    }
                    else if (current.kind == TypeKind::Tuple && projection.kind == AIRProjectionKind::Field)
                    { if (projection.field >= current.parameters.size()) return InvalidTypeId; type = current.parameters[projection.field]; }
                    else
                    {
                        const auto record = records_.find(type);
                        if (record == records_.end()) return InvalidTypeId;
                        if (projection.kind == AIRProjectionKind::EnumField)
                        {
                            const auto variant = std::find_if(record->second->variants.begin(), record->second->variants.end(),
                                [&](const EnumVariantInfo& value) { return value.variant == projection.variant; });
                            if (variant == record->second->variants.end() || projection.field >= variant->fieldTypes.size()) return InvalidTypeId;
                            type = variant->fieldTypes[projection.field];
                        }
                        else
                        { if (projection.field >= record->second->fieldTypes.size()) return InvalidTypeId; type = record->second->fieldTypes[projection.field]; }
                    }
                }
                return type;
            }

            std::map<ReferencePath, bool> referenceLeaves(TypeId id) const
            {
                std::map<ReferencePath, bool> result;
                std::unordered_set<TypeId> visited;
                const auto collect = [&](const auto& self, TypeId type, ReferencePath path) -> void
                {
                    if (!containsReference(type)) return;
                    const auto& current = types_.types.get(type);
                    // 봉인된 핸들의 비공개 정수 필드 대신 논리적인 공유 Scope 대여를 요약한다.
                    // Summarize the sealed handle's logical shared Scope loan, not its private integer field.
                    if (current.kind == TypeKind::Task) { result[path] = false; return; }
                    if (current.kind == TypeKind::Reference || syncGuard(current.kind) || syncOwner(current.kind))
                    { result[path] = current.kind != TypeKind::Reference || current.isMutable; return; }
                    if (!visited.insert(type).second) { result[path] = true; return; }
                    if (current.kind == TypeKind::Box || current.kind == TypeKind::Shared || current.kind == TypeKind::Weak)
                    { path.push_back({AIRProjectionKind::Dereference, 1}); self(self, current.element, std::move(path)); }
                    else if (current.kind == TypeKind::Array || current.kind == TypeKind::Vector)
                    { path.push_back({AIRProjectionKind::Index}); self(self, current.element, std::move(path)); }
                    else if (current.kind == TypeKind::Tuple)
                        for (std::size_t i = 0; i < current.parameters.size(); ++i)
                        { auto nested = path; nested.push_back({AIRProjectionKind::Field, i}); self(self, current.parameters[i], std::move(nested)); }
                    else if (const auto record = records_.find(type); record != records_.end())
                    {
                        for (std::size_t i = 0; i < record->second->fieldTypes.size(); ++i)
                        { auto nested = path; nested.push_back({AIRProjectionKind::Field, i}); self(self, record->second->fieldTypes[i], std::move(nested)); }
                        for (const auto& variant : record->second->variants)
                            for (std::size_t i = 0; i < variant.fieldTypes.size(); ++i)
                            { auto nested = path; nested.push_back({AIRProjectionKind::EnumField, i, variant.variant}); self(self, variant.fieldTypes[i], std::move(nested)); }
                    }
                    else result[path] = true;
                    visited.erase(type);
                };
                collect(collect, id, {}); return result;
            }

            void provenanceLimit()
            {
                if (analysisLimit_) return;
                analysisLimit_ = true;
                const bool reporting = reporting_; reporting_ = true;
                error(currentFunction_ && currentFunction_->source ? currentFunction_->source->range : SourceRange{},
                    "Reference provenance analysis exceeds its supported capacity (4096 entries or depth 64).");
                reporting_ = reporting;
            }

            ProvenanceFields normalizeFields(TypeId root, const ProvenanceFields& fields)
            {
                ProvenanceFields result;
                for (const auto& [path, values] : fields)
                {
                    ReferencePath canonical; TypeId type = root;
                    std::unordered_set<TypeId> visited; visited.insert(type);
                    for (const auto& projection : path)
                    {
                        const auto kind = types_.types.get(type).kind;
                        if (kind == TypeKind::Reference || kind == TypeKind::Slice || kind == TypeKind::StringView || syncGuard(kind)) break;
                        const auto next = pathType(type, {projection});
                        if (next == InvalidTypeId) break;
                        canonical.push_back(projection); type = next;
                        // 재귀 소유 타입 아래의 경로는 모든 후손을 포함하는 접두어로 넓힌다.
                        // Widen paths below a repeated owned type to a prefix covering every descendant.
                        if (!visited.insert(type).second) break;
                    }
                    if (canonical.size() > maximumProvenanceDepth) { provenanceLimit(); return {}; }
                    bool covered = false;
                    for (auto& [existing, retained] : result)
                        if (existing.size() <= canonical.size() && std::equal(existing.begin(), existing.end(), canonical.begin()))
                        { unite(retained, values); covered = true; break; }
                    if (covered) continue;
                    auto retained = values;
                    std::erase_if(result, [&](const auto& entry) {
                        if (canonical.size() <= entry.first.size() && std::equal(canonical.begin(), canonical.end(), entry.first.begin()))
                        { unite(retained, entry.second); return true; }
                        return false;
                    });
                    result[canonical] = std::move(retained);
                    if (result.size() > maximumProvenanceEntries) { provenanceLimit(); return {}; }
                }
                return result;
            }

            std::size_t symbolicLoan(State& state, Target target, bool exclusive)
            {
                if (target.projections.size() + target.storageProjection.size() > maximumProvenanceDepth
                    || (symbolicLoans_.size() >= maximumProvenanceEntries && !symbolicLoans_.contains(target)))
                { provenanceLimit(); return InvalidAIRId; }
                const auto [found, inserted] = symbolicLoans_.try_emplace(target, nextSymbolicLoan_);
                if (inserted) ++nextSymbolicLoan_;
                auto& loan = state.loans[found->second]; loan.exclusive = loan.exclusive || exclusive; loan.targets.insert(std::move(target));
                return found->second;
            }

            ProvenanceFields valueFields(const State& state, AIRValueId value) const
            {
                return value < state.valueFields.size() ? fieldFallback(state.valueFields[value], this->value(state, value)) : ProvenanceFields{};
            }

            bool symbolicReferenceTarget(const AIRFunction& function, const Target& target) const
            {
                if (target.kind == OriginKind::ParameterContents) return true;
                if (target.kind != OriginKind::Parameter || target.id >= function.parameters.size()) return false;
                const auto kind = types_.types.get(function.locals[function.parameters[target.id]].type).kind;
                return kind != TypeKind::Reference && kind != TypeKind::Slice;
            }

            void appendStoragePath(const AIRFunction& function, Target& target, const ReferencePath& path)
            {
                auto& destination = target.opaqueStorage || symbolicReferenceTarget(function, target) ? target.storageProjection : target.projections;
                if (destination.size() + path.size() > maximumProvenanceDepth) { provenanceLimit(); return; }
                destination.insert(destination.end(), path.begin(), path.end());
            }

            ReferenceParameterPath indirectPath(const AIRFunction& function, const Target& target) const
            {
                ReferenceParameterPath result{target.id, target.projections, target.storageProjection, target.kind == OriginKind::ParameterContents, target.opaqueStorage};
                if (target.id < function.parameters.size())
                {
                    const auto& parameter = types_.types.get(function.locals[function.parameters[target.id]].type);
                    const auto root = parameter.kind == TypeKind::Reference ? parameter.element : function.locals[function.parameters[target.id]].type;
                    if (result.storageProjection.empty() && syncGuard(types_.types.get(root).kind) && target.projections.size() == 1
                        && target.projections[0].kind == AIRProjectionKind::Field && target.projections[0].field == 1)
                    { result.path.clear(); result.storageProjection = target.projections; }
                }
                return result;
            }

            ProvenanceFields targetFields(const AIRFunction& function, const Target& target, TypeId type, State& state)
            {
                if (target.kind == OriginKind::Local && target.id < state.locals.size())
                    return selectFields(fieldFallback(state.localFields[target.id], state.locals[target.id]), target.projections);
                bool valueReference = false;
                if (target.kind == OriginKind::Parameter && target.id < function.parameters.size())
                {
                    const auto& parameter = types_.types.get(function.locals[function.parameters[target.id]].type);
                    valueReference = parameter.kind != TypeKind::Reference && parameter.kind != TypeKind::Slice;
                    if (!valueReference) return selectFields(fieldFallback(state.parameterFields[target.id], state.parameterContents[target.id]), target.projections);
                }
                ProvenanceFields result;
                if (target.kind == OriginKind::ParameterContents || valueReference)
                {
                    if (target.opaqueStorage)
                    {
                        auto origin = target; origin.opaqueStorage = false;
                        if (!origin.storageProjection.empty())
                        {
                            origin.projections.push_back({AIRProjectionKind::Dereference});
                            origin.projections.insert(origin.projections.end(), origin.storageProjection.begin(), origin.storageProjection.end());
                            origin.storageProjection.clear();
                        }
                        result[{}].insert(symbolicLoan(state, std::move(origin), true)); return result;
                    }
                    // 보관한 참조를 따라 읽은 내용은 그 참조 자체의 저장소 대여와 다르다.
                    // Contents read through a stored reference are distinct from the reference's own storage loan.
                    for (const auto& [path, exclusive] : referenceLeaves(type))
                    {
                        auto origin = target;
                        const auto forwarded = indirectPath(function, target);
                        if (!forwarded.storageProjection.empty()) origin.projections = forwarded.path;
                        origin.projections.push_back({AIRProjectionKind::Dereference});
                        auto storage = forwarded.storageProjection;
                        if (target.id < function.parameters.size())
                        {
                            const auto& parameter = types_.types.get(function.locals[function.parameters[target.id]].type);
                            const auto root = parameter.kind == TypeKind::Reference ? parameter.element : function.locals[function.parameters[target.id]].type;
                            if (syncGuard(types_.types.get(pathType(root, forwarded.path)).kind) && !storage.empty()
                                && storage.front() == ReferenceProjection{AIRProjectionKind::Field, 1}) storage.erase(storage.begin());
                        }
                        origin.projections.insert(origin.projections.end(), storage.begin(), storage.end());
                        origin.projections.insert(origin.projections.end(), path.begin(), path.end());
                        origin.storageProjection.clear();
                        if (origin.projections.size() > maximumProvenanceDepth)
                        { provenanceLimit(); continue; }
                        result[path].insert(symbolicLoan(state, std::move(origin), exclusive));
                    }
                    if (const auto changed = state.indirectStores.find(indirectPath(function, target)); changed != state.indirectStores.end())
                        unite(result[{}], changed->second);
                }
                return result;
            }

            ProvenanceFields readFields(const AIRFunction& function, const AIRPlace& location, State& state)
            {
                if (!containsReference(location.type)) return {};
                ProvenanceFields result;
                if (location.pointer == InvalidAIRId && location.local < state.locals.size())
                    result = selectFields(fieldFallback(state.localFields[location.local], state.locals[location.local]), projectionPath(location.projections));
                else
                {
                    const auto view = place(function, location, state);
                    for (const auto& target : view.targets)
                        mergeFields(result, targetFields(function, target, location.type, state));
                    if (result.empty() && location.pointer != InvalidAIRId)
                        for (const auto& target : view.targets)
                            if (target.kind == OriginKind::ParameterElements || target.kind == OriginKind::VariadicStorage)
                                unite(result[{}], value(state, location.pointer));
                }
                TypeId base = InvalidTypeId;
                if (location.local < function.locals.size()) base = function.locals[location.local].type;
                else if (location.pointer < function.valueTypes.size())
                {
                    const auto& pointer = types_.types.get(function.valueTypes[location.pointer]);
                    base = pointer.kind == TypeKind::Reference ? pointer.element : function.valueTypes[location.pointer];
                }
                if (!location.projections.empty() && location.projections.front().kind == AIRProjectionKind::Index
                    && types_.types.get(base).kind == TypeKind::Slice)
                {
                    // 원소 안의 외부 참조와 pack 자체의 저장소 출처를 상세 필드에서도 분리한다.
                    // Separate external element references from pack storage in detailed fields as well as flat origins.
                    IdSet contents;
                    for (const auto id : flatten(result))
                        if (const auto loan = state.loans.find(id); loan != state.loans.end() && loan->second.sliceContents) contents.insert(id);
                    if (!contents.empty()) return {{{}, std::move(contents)}};
                }
                return result;
            }

            State initial(const AIRFunction& function)
            {
                State state;
                state.live.resize(function.locals.size());
                state.locals.resize(function.locals.size());
                state.values.resize(function.valueTypes.size());
                state.parameterContents.resize(function.parameters.size());
                state.localFields.resize(function.locals.size());
                state.valueFields.resize(function.valueTypes.size());
                state.parameterFields.resize(function.parameters.size());
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
                        std::unordered_set<TypeId> visited;
                        const bool destructorMutates = (!dropTargets(function.locals[local].type).empty() || nativeCleanup(function.locals[local].type, true))
                            && containsMutableReference(function.locals[local].type, visited);
                        state.loans[loan] = {type.isMutable || destructorMutates, {{storageOrigin, p, {}}}, {}, type.kind == TypeKind::Slice, false};
                        state.locals[local].insert(loan);
                        if (type.kind == TypeKind::Slice && containsReference(type.element))
                        {
                            const auto contents = parameterLoanBase_ + function.parameters.size() + p;
                            state.loans[contents] = {false, {{OriginKind::ParameterElements, p, {}}}, {}, false, true};
                            state.locals[local].insert(contents);
                            state.parameterContents[p].insert(contents);
                        }
                        else if (type.kind == TypeKind::Reference && containsReference(type.element))
                        {
                            const auto contents = parameterLoanBase_ + function.parameters.size() + p;
                            std::unordered_set<TypeId> innerVisited;
                            state.loans[contents] = {containsMutableReference(type.element, innerVisited),
                                {{OriginKind::ParameterContents, p, {}}}, {}, false, false};
                            state.parameterContents[p].insert(contents);
                        }
                        if (type.kind != TypeKind::Reference && type.kind != TypeKind::Slice)
                        {
                            state.locals[local].clear(); state.loans.erase(loan);
                            for (const auto& [path, exclusive] : referenceLeaves(function.locals[local].type))
                            {
                                const auto id = symbolicLoan(state, {OriginKind::Parameter, p, path}, exclusive || destructorMutates);
                                state.localFields[local][path].insert(id); state.locals[local].insert(id);
                            }
                        }
                        else if (type.kind == TypeKind::Reference && containsReference(type.element))
                        {
                            const auto old = parameterLoanBase_ + function.parameters.size() + p;
                            state.parameterContents[p].clear(); state.loans.erase(old);
                            for (const auto& [path, exclusive] : referenceLeaves(type.element))
                            {
                                const auto id = symbolicLoan(state, {OriginKind::ParameterContents, p, path}, exclusive);
                                state.parameterFields[p][path].insert(id); state.parameterContents[p].insert(id);
                            }
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

            void referenceClosure(const State& state, IdSet& retained) const
            {
                // 대여한 집합의 필드를 나중에 읽을 수 있으므로 그 집합 안의 참조도 유지한다.
                // Retain references inside borrowed aggregates because their fields can be read later.
                bool changed = true;
                while (changed)
                {
                    const auto prior = retained.size();
                    permissionClosure(state, retained);
                    const auto snapshot = retained;
                    for (const auto id : snapshot)
                        if (const auto loan = state.loans.find(id); loan != state.loans.end())
                            for (const auto& target : loan->second.targets)
                            {
                                if (target.kind == OriginKind::Local && target.id < state.locals.size()
                                    && (state.live[target.id] & 1) != 0)
                                    unite(retained, state.locals[target.id]);
                                else if (target.kind == OriginKind::Parameter && target.id < state.parameterContents.size())
                                    unite(retained, state.parameterContents[target.id]);
                            }
                    changed = retained.size() != prior;
                }
            }

            PlaceView place(const AIRFunction& function, const AIRPlace& place, const State& state)
            {
                PlaceView view;
                if (place.pointer != InvalidAIRId)
                {
                    view.permission = value(state, place.pointer);
                    const bool slicePointer = place.pointer < function.valueTypes.size()
                        && types_.types.get(function.valueTypes[place.pointer]).kind == TypeKind::Slice;
                    for (const auto id : view.permission)
                        if (const auto found = state.loans.find(id); found != state.loans.end()
                            && !(slicePointer && found->second.sliceContents))
                            unite(view.targets, found->second.targets);
                    permissionClosure(state, view.permission);
                }
                else if (place.local != InvalidAIRId)
                    view.targets.insert({OriginKind::Local, place.local, {}});
                else if (place.global != InvalidSymbolId)
                    view.targets.insert({OriginKind::Global, place.global, {}});
                TypeId base = InvalidTypeId;
                if (place.local < function.locals.size()) base = function.locals[place.local].type;
                else if (place.pointer < function.valueTypes.size())
                {
                    const auto& pointerType = types_.types.get(function.valueTypes[place.pointer]);
                    base = pointerType.kind == TypeKind::Reference ? pointerType.element : function.valueTypes[place.pointer];
                }
                if (types_.types.get(base).kind == TypeKind::Slice && !place.projections.empty()
                    && place.projections.front().kind == AIRProjectionKind::Index)
                {
                    // 인덱스는 지역 Slice 설명자가 아니라 그 설명자가 빌린 데이터에 접근한다.
                    // Indexing addresses borrowed slice data rather than the local slice descriptor.
                    IdSet storage;
                    for (const auto& target : view.targets)
                        if (target.kind == OriginKind::Local && target.id < state.locals.size()) unite(storage, state.locals[target.id]);
                        else if (target.kind == OriginKind::Parameter && target.id < state.parameterContents.size())
                        {
                            const auto& parameterType = types_.types.get(function.locals[function.parameters[target.id]].type);
                            if (parameterType.kind == TypeKind::Reference) unite(storage, state.parameterContents[target.id]);
                            else if (place.pointer != InvalidAIRId) unite(storage, value(state, place.pointer));
                        }
                    std::set<Target> data;
                    for (const auto id : storage)
                        if (const auto loan = state.loans.find(id); loan != state.loans.end() && !loan->second.sliceContents)
                        { unite(data, loan->second.targets); view.permission.insert(id); }
                    if (!data.empty()) view.targets = std::move(data);
                    permissionClosure(state, view.permission);
                }
                std::set<Target> projected;
                for (auto target : view.targets)
                {
                    appendStoragePath(function, target, projectionPath(place.projections));
                    projected.insert(std::move(target));
                }
                view.targets = std::move(projected);
                return view;
            }

            void access(const AIRFunction& function, const AIRInstruction& instruction, const State& state, bool exclusive,
                        bool moving = false)
            {
                // 이동한 뷰를 통한 원소 쓰기는 원래 바인딩의 재초기화가 아니다.
                // Writing through a moved view does not reinitialize the original binding.
                if (instruction.place.local < state.live.size() && state.live[instruction.place.local] != 1
                    && (instruction.opcode != AIROpcode::Write || !instruction.place.projections.empty()))
                    error(instruction.range, "Use of a moved or unavailable value.");
                const auto view = place(function, instruction.place, state);
                if (moving && instruction.place.pointer != InvalidAIRId)
                    error(instruction.range, "Moving an owning value out of a borrowed place requires an ownership-preserving operation such as memory.replace.");
                if (exclusive && instruction.place.pointer < function.valueTypes.size())
                {
                    const auto& pointer = types_.types.get(function.valueTypes[instruction.place.pointer]);
                    // 이동도 저장소를 바꾸는 접근이므로 읽기 대여에서 소유권을 꺼낼 수 없다.
                    // Moving changes storage ownership and therefore cannot extract through a shared loan.
                    if ((pointer.kind == TypeKind::Reference || pointer.kind == TypeKind::Slice) && !pointer.isMutable)
                        error(instruction.range, "Cannot modify, move, or exclusively borrow through a read-only reference.");
                }
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

            IdSet readProvenance(const AIRFunction& function, const AIRPlace& location, State& state)
            {
                return flatten(readFields(function, location, state));
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

            bool mutableIndirectTarget(const AIRFunction& function, const Target& target) const
            {
                if ((target.kind != OriginKind::ParameterContents && target.kind != OriginKind::Parameter)
                    || target.id >= function.parameters.size() || target.projections.size() > 64) return false;
                const auto parameterType = function.locals[function.parameters[target.id]].type;
                const auto& parameter = types_.types.get(parameterType);
                const bool contents = target.kind == OriginKind::ParameterContents;
                if (contents && parameter.kind != TypeKind::Reference) return false;
                if (!contents && (parameter.kind == TypeKind::Reference || parameter.kind == TypeKind::Slice)) return false;
                const auto& selected = types_.types.get(pathType(contents ? parameter.element : parameterType, target.projections));
                if (syncGuard(selected.kind) && !target.storageProjection.empty()
                    && target.storageProjection.front() == ReferenceProjection{AIRProjectionKind::Field, 1}) return true;
                return selected.kind == TypeKind::Reference && selected.isMutable;
            }

            void checkTargets(const AIRFunction& function, const AIRInstruction& instruction, const State& state,
                              const IdSet& provenance, const std::set<Target>& destinations, bool forwarded)
            {
                for (const auto loanId : provenance)
                {
                    const auto loan = state.loans.find(loanId);
                    if (loan == state.loans.end()) continue;
                    for (const auto& origin : loan->second.targets)
                        for (const auto& target : destinations)
                        {
                            const bool symbolicInput = origin.kind == OriginKind::Parameter || origin.kind == OriginKind::ParameterContents
                                || origin.kind == OriginKind::ParameterElements;
                            const bool checkedForwarding = forwarded && symbolicInput && mutableIndirectTarget(function, target);
                            const bool directParameter = target.kind == OriginKind::Parameter && target.id < function.parameters.size()
                                && (types_.types.get(function.locals[function.parameters[target.id]].type).kind == TypeKind::Reference
                                    || types_.types.get(function.locals[function.parameters[target.id]].type).kind == TypeKind::Slice);
                            if (target.kind != OriginKind::Local && origin.kind != OriginKind::Global
                                && !(directParameter && symbolicInput) && !checkedForwarding)
                                error(instruction.range, "Cannot store a borrowed input or local reference in escaping storage.");
                            if (origin.kind == OriginKind::Local && origin.id < function.locals.size()
                                && (target.kind != OriginKind::Local || target.id >= function.locals.size()
                                    || !ancestor(function, function.locals[origin.id].scope, function.locals[target.id].scope)))
                                error(instruction.range, "A reference cannot outlive the local value it borrows.");
                        }
                }
            }

            void checkStorage(const AIRFunction& function, const AIRInstruction& instruction,
                              const State& state, const IdSet& provenance)
            { checkTargets(function, instruction, state, provenance, place(function, instruction.place, state).targets, false); }

            void updateTargets(const AIRFunction& function, const AIRInstruction& operation, State& state,
                               const std::set<Target>& targets, const IdSet& incoming,
                               const ProvenanceFields& fields = {}, bool append = false)
            {
                checkTargets(function, operation, state, incoming, targets, true);
                for (const auto& target : targets)
                {
                    IdSet* contents = nullptr; ProvenanceFields* carried = nullptr;
                    if (target.kind == OriginKind::Local && target.id < state.locals.size())
                    { contents = &state.locals[target.id]; carried = &state.localFields[target.id]; }
                    else if (mutableIndirectTarget(function, target))
                    {
                        // 값으로 전달한 소유 능력도 원본 저장소에 수행한 변경을 호출자에게 전달한다.
                        // Owned capabilities passed by value must forward mutations of their original storage too.
                        unite(state.indirectStores[indirectPath(function, target)], incoming); continue;
                    }
                    else if (target.kind == OriginKind::Parameter && target.id < state.parameterContents.size()
                        && !symbolicReferenceTarget(function, target))
                    { contents = &state.parameterContents[target.id]; carried = &state.parameterFields[target.id]; }
                    if (!contents) continue;
                    if (target.projections.empty() && !append && !target.opaqueStorage)
                    { *contents = incoming; *carried = fieldFallback(fields, incoming); }
                    else
                    {
                        if (carried->empty() && !contents->empty()) (*carried)[{}] = *contents;
                        for (const auto& [path, values] : fieldFallback(fields, incoming))
                        {
                            auto destination = target.projections;
                            destination.insert(destination.end(), path.begin(), path.end());
                            unite((*carried)[destination], values);
                        }
                        unite(*contents, incoming);
                    }
                    const auto storedType = target.kind == OriginKind::Local ? function.locals[target.id].type
                        : types_.types.get(function.locals[function.parameters[target.id]].type).element;
                    *carried = normalizeFields(storedType, *carried); *contents = flatten(*carried);
                }
            }

            void updateStorage(const AIRFunction& function, const AIRInstruction& operation, State& state,
                               const IdSet& incoming, const ProvenanceFields& fields = {}, bool append = false)
            { updateTargets(function, operation, state, place(function, operation.place, state).targets, incoming, fields, append); }

            void endScope(const AIRFunction& function, AIRScopeId scope, State& state)
            {
                if (scope >= function.scopes.size()) return;
                for (const auto local : function.scopes[scope].locals)
                    if (local < state.live.size())
                    {
                        state.live[local] = 0;
                        state.locals[local].clear(); state.localFields[local].clear();
                        std::erase_if(state.extractedFields, [&](const auto& field) { return std::get<0>(field) == local; });
                    }
            }

            void prune(const AIRFunction& function, State& state, const IdSet& liveValues, const IdSet& liveLocals) const
            {
                IdSet retained;
                for (const auto& contents : state.parameterContents) unite(retained, contents);
                for (const auto& [path, contents] : state.indirectStores) { (void)path; unite(retained, contents); }
                for (std::size_t i = 0; i < state.live.size(); ++i)
                    if ((state.live[i] & 1) != 0)
                    {
                        if (liveLocals.contains(i)) unite(retained, state.locals[i]);
                        if (i < function.locals.size() && (!dropTargets(function.locals[i].type).empty() || nativeCleanup(function.locals[i].type, true)))
                        {
                            auto cleanup = state.locals[i];
                            referenceClosure(state, cleanup);
                            unite(retained, cleanup);
                        }
                    }
                for (const auto id : liveValues)
                    if (id < state.values.size()) unite(retained, state.values[id]);
                // 소멸자 소유자에 저장되기 전의 대여 임시값에서도 중첩 참조가 먼저 사라지면 안 된다.
                // Nested references must survive borrowed temporaries before storage in a destructor owner as well.
                referenceClosure(state, retained);
                std::erase_if(state.loans, [&](const auto& item) { return !retained.contains(item.first); });
            }

            void initializationEffects(const AIRFunction& function, const AIRInstruction& instruction, SymbolId storage, State& state)
            {
                if (state.initializedPersist.contains(storage)) return;
                if (unknownInitializerEffects_[storage] && !state.loans.empty())
                    error(instruction.range, "Cannot initialize persist through an external function while loans are active.");
                for (const auto& [global, write] : initializerEffects_[storage])
                {
                    auto effect = instruction;
                    effect.place = {}; effect.place.global = global;
                    access(function, effect, state, write);
                }
            }

            bool crossesCustomDrop(const AIRFunction& function, const AIRPlace& location) const
            {
                TypeId current = InvalidTypeId;
                if (location.local < function.locals.size()) current = function.locals[location.local].type;
                else if (location.pointer < function.valueTypes.size())
                {
                    const auto& pointer = types_.types.get(function.valueTypes[location.pointer]);
                    current = pointer.kind == TypeKind::Reference || pointer.kind == TypeKind::Box ? pointer.element : InvalidTypeId;
                }
                else if (location.global != InvalidSymbolId)
                    for (const auto& global : module_.globals) if (global.symbol == location.global) current = global.type;
                for (const auto& projection : location.projections)
                {
                    if (types_.dropMethods.contains(current)) return true;
                    current = projection.type;
                }
                return false;
            }

            void destructorEffects(const AIRFunction& function, const AIRInstruction& instruction, State& state)
            {
                const auto local = instruction.place.local;
                if (local >= state.live.size() || (state.live[local] & 1) == 0) return;
                const auto& methods = dropTargets(function.locals[local].type);
                if (methods.empty() && !nativeCleanup(function.locals[local].type)) return;

                // 소멸자는 자기 저장소와 보유한 참조를 사용할 수 있으므로 파생 대여도 충돌을 검사한다.
                // A destructor can use its storage and retained references, including conflicts with derived loans.
                const Target storage{OriginKind::Local, local, {}};
                IdSet permission = state.locals[local];
                referenceClosure(state, permission);
                for (const auto& [id, loan] : state.loans)
                {
                    for (const auto& target : loan.targets)
                        if (overlaps(storage, target))
                            error(instruction.range, "Cannot destroy an owner while its storage is borrowed.");
                    if (permission.contains(id)) continue;
                    for (const auto own : permission)
                        if (const auto held = state.loans.find(own); held != state.loans.end()
                            && (held->second.exclusive || loan.exclusive))
                            for (const auto& target : held->second.targets)
                                for (const auto& borrowed : loan.targets)
                                    if (overlaps(target, borrowed))
                                        error(instruction.range, "A destructor cannot use a retained reference while a derived or conflicting loan is active.");
                }
                for (const auto method : methods)
                {
                    for (const auto storage : persistEnsures_[method]) initializationEffects(function, instruction, storage, state);
                    if (unknownEffects_[method] && !state.loans.empty())
                        error(instruction.range, "Cannot run a destructor with unknown external effects while loans are active.");
                    for (const auto& [global, write] : globalEffects_[method])
                    {
                        auto effect = instruction;
                        effect.place = {};
                        effect.place.global = global;
                        access(function, effect, state, write);
                    }
                }
            }

            void vectorCleanupEffects(const AIRFunction& function, const AIRInstruction& instruction, State& state)
            {
                const auto& methods = dropTargets(types_.types.get(instruction.place.type).element);
                if (methods.empty()) return;
                auto permission = readProvenance(function, instruction.place, state);
                referenceClosure(state, permission);
                for (const auto& [id, loan] : state.loans)
                {
                    if (permission.contains(id)) continue;
                    for (const auto own : permission)
                        if (const auto held = state.loans.find(own); held != state.loans.end()
                            && (held->second.exclusive || loan.exclusive))
                            for (const auto& target : held->second.targets)
                                for (const auto& borrowed : loan.targets)
                                    if (overlaps(target, borrowed))
                                        error(instruction.range, "A destructor cannot use a retained reference while a derived or conflicting loan is active.");
                }
                for (const auto method : methods)
                {
                    for (const auto storage : persistEnsures_[method]) initializationEffects(function, instruction, storage, state);
                    if (unknownEffects_[method] && !state.loans.empty())
                        error(instruction.range, "Cannot run a destructor with unknown external effects while loans are active.");
                    for (const auto& [global, write] : globalEffects_[method])
                    {
                        auto effect = instruction; effect.place = {}; effect.place.global = global;
                        access(function, effect, state, write);
                    }
                }
            }

            ProvenanceFields sourceFields(const AIRFunction& function, const AIRInstruction& instruction, State& state,
                                          std::size_t parameter, bool contents, const ReferencePath& path)
            {
                if (parameter >= instruction.operands.size()) return {};
                const auto argument = instruction.operands[parameter];
                if (argument >= function.valueTypes.size()) return {};
                TypeId type = function.valueTypes[argument];
                ProvenanceFields result = valueFields(state, argument);
                if (contents)
                {
                    const auto& argumentType = types_.types.get(type);
                    if (argumentType.kind != TypeKind::Reference) return {};
                    AIRPlace location; location.pointer = argument; location.type = argumentType.element;
                    result = readFields(function, location, state); type = argumentType.element;
                }
                for (const auto& projection : path)
                {
                    if (projection.kind == AIRProjectionKind::Dereference)
                    {
                        const auto& reference = types_.types.get(type);
                        const bool ownedPayload = projection.field == 1 && (reference.kind == TypeKind::Box || reference.kind == TypeKind::Shared || reference.kind == TypeKind::Weak);
                        if (ownedPayload)
                        { result = selectFields(result, {projection}); type = reference.element; continue; }
                        const bool guard = syncGuard(reference.kind);
                        if (reference.kind != TypeKind::Reference && !guard)
                        { error(instruction.range, "Cannot resolve reference provenance through this nested or erased value."); return {}; }
                        ProvenanceFields loaded;
                        const auto inputs = flatten(result);
                        for (const auto id : inputs)
                            if (const auto loan = state.loans.find(id); loan != state.loans.end())
                            {
                                const auto targets = loan->second.targets;
                                for (auto target : targets)
                                {
                                    if (guard) appendStoragePath(function, target, {{AIRProjectionKind::Field, 1}});
                                    mergeFields(loaded, targetFields(function, target, reference.element, state));
                                }
                            }
                        result = std::move(loaded); type = reference.element;
                    }
                    else if (syncGuard(types_.types.get(type).kind) && projection.kind == AIRProjectionKind::Field && projection.field == 1)
                        type = types_.types.get(type).element;
                    else
                    {
                        result = selectFields(result, {projection}); type = pathType(type, {projection});
                        if (type == InvalidTypeId)
                        { error(instruction.range, "Cannot resolve reference provenance through this nested or erased projection."); return {}; }
                    }
                }
                return result;
            }

            void transfer(const AIRFunction& function, std::size_t block, std::size_t index, State& state)
            {
                if (analysisLimit_) return;
                const auto& instruction = function.blocks[block].instructions[index];
                IdSet result; ProvenanceFields fields;
                if (instruction.opcode == AIROpcode::BoxExtract)
                {
                    if (!validBoxExtractionShape(function, instruction, types_.types))
                    { error(instruction.range, "Box extraction owner/result metadata is invalid."); return; }
                    // 바깥 Box 저장소만 소비하고 T가 보유한 원래 외부 대여를 그대로 이동한다.
                    // Consume only outer Box storage and transfer T's original external loans unchanged.
                    access(function, instruction, state, true, true);
                    fields = selectFields(readFields(function, instruction.place, state), {{AIRProjectionKind::Dereference, 1}});
                    result = flatten(fields);
                    const auto owner = instruction.place.local;
                    if (owner < state.live.size())
                    { state.live[owner] = 2; state.locals[owner].clear(); state.localFields[owner].clear(); }
                }
                else if (instruction.opcode == AIROpcode::ClosureConsume)
                {
                    // 전체 소유 환경만 소비한다. 캡처의 외부 대여 출처는 새 환경 지역의 대여로 바꾸지 않는다.
                    // Consume only the complete owned environment, preserving each capture's external loan origins.
                    access(function, instruction, state, true, true);
                    const auto captured = readFields(function, instruction.place, state);
                    const auto receiver = instruction.place.local;
                    for (std::size_t field = 0; field < instruction.closureLocals.size(); ++field)
                    {
                        const auto destination = instruction.closureLocals[field];
                        auto provenance = selectFields(captured, {{AIRProjectionKind::Field, field}});
                        state.localFields[destination] = normalizeFields(function.locals[destination].type, provenance);
                        state.locals[destination] = flatten(state.localFields[destination]); state.live[destination] = 1;
                    }
                    state.live[receiver] = 2; state.locals[receiver].clear(); state.localFields[receiver].clear();
                }
                else if (instruction.opcode == AIROpcode::Read || instruction.opcode == AIROpcode::Move)
                {
                    const bool staticText = instruction.place.global != InvalidSymbolId
                        && std::any_of(module_.globals.begin(), module_.globals.end(), [&](const AIRGlobal& global) {
                        return global.symbol == instruction.place.global && global.isStaticText;
                    });
                    if (instruction.place.global != InvalidSymbolId && containsReference(instruction.type) && !staticText)
                        error(instruction.range, "Reading global reference-containing values requires initialization provenance analysis.");
                    const bool moved = instruction.opcode == AIROpcode::Move && !copy(instruction.type);
                    if (moved && crossesCustomDrop(function, instruction.place))
                        error(instruction.range, "Cannot move a non-Copy field out of a type implementing Drop.");
                    const bool extract = moved && instruction.operation == "enum.extract"
                        && instruction.place.pointer == InvalidAIRId
                        && instruction.place.local < function.locals.size()
                        && function.locals[instruction.place.local].name == "$enumselector"
                        && instruction.place.projections.size() == 1
                        && instruction.place.projections.front().kind == AIRProjectionKind::EnumField;
                    access(function, instruction, state, moved, moved && !extract);
                    if (extract)
                    {
                        const auto& field = instruction.place.projections.front();
                        const auto key = std::make_tuple(instruction.place.local, field.variant, field.field);
                        if (!state.extractedFields.insert(key).second)
                            error(instruction.range, "An enum payload field cannot be moved more than once.");
                    }
                    fields = readFields(function, instruction.place, state);
                    result = readProvenance(function, instruction.place, state);
                    if (extract)
                    {
                        const auto& extracted = instruction.place.projections.front();
                        auto& remaining = state.localFields[instruction.place.local];
                        // 실제 선택한 항목의 이동된 필드는 부모 enum의 정리 대여에 남지 않는다.
                        // Moved fields of the selected variant no longer participate in the parent enum's cleanup loans.
                        std::erase_if(remaining, [&](const auto& entry) {
                            if (entry.first.empty() || entry.first.front().kind != AIRProjectionKind::EnumField) return false;
                            return entry.first.front().variant != extracted.variant || entry.first.front().field == extracted.field;
                        });
                        state.locals[instruction.place.local] = flatten(remaining);
                    }
                    if (moved && instruction.place.local < state.live.size()
                        && instruction.place.projections.empty() && instruction.place.pointer == InvalidAIRId)
                    {
                        state.live[instruction.place.local] = 2;
                        state.locals[instruction.place.local].clear(); state.localFields[instruction.place.local].clear();
                    }
                }
                else if (instruction.opcode == AIROpcode::BorrowShared || instruction.opcode == AIROpcode::BorrowMut || instruction.opcode == AIROpcode::SliceConstruct)
                {
                    const bool exclusive = instruction.opcode == AIROpcode::BorrowMut || (instruction.opcode == AIROpcode::SliceConstruct && types_.types.get(instruction.type).isMutable);
                    access(function, instruction, state, exclusive);
                    const auto target = place(function, instruction.place, state);
                    const auto id = loanIds_[block][index];
                    const bool slice = instruction.opcode == AIROpcode::SliceConstruct;
                    auto borrowedTargets = target.targets;
                    if (instruction.operation == "box.borrow")
                    {
                        std::set<Target> opaque;
                        for (auto owner : borrowedTargets)
                        { if (owner.opaqueStorage) owner.storageProjection.clear(); owner.opaqueStorage = true; opaque.insert(std::move(owner)); }
                        borrowedTargets = std::move(opaque);
                    }
                    state.loans[id] = {exclusive, std::move(borrowedTargets), target.permission, slice, false};
                    state.loans[id].separateParents = std::any_of(target.targets.begin(), target.targets.end(),
                        [](const Target& value) { return !value.storageProjection.empty(); });
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
                else if (instruction.opcode == AIROpcode::SyncIntrinsic)
                {
                    const auto* operation = syncOperation(instruction.operation);
                    if (operation)
                    {
                        if (operation->result == SyncResult::Owner || operation->result == SyncResult::IntoInner || operation->result == SyncResult::Recovered)
                            result = value(state, instruction.operands[0]);
                        else if (operation->result == SyncResult::Acquire || operation->result == SyncResult::OptionalAcquire || operation->result == SyncResult::UniqueReference)
                            result = value(state, instruction.operands[0]);
                        else if (operation->result == SyncResult::Reference || operation->result == SyncResult::MutableReference)
                        {
                            AIRPlace borrowed; borrowed.pointer = instruction.operands[0];
                            borrowed.type = types_.types.get(function.valueTypes[instruction.operands[0]]).element;
                            const auto parent = value(state, instruction.operands[0]);
                            const auto retained = readProvenance(function, borrowed, state);
                            Loan loan; loan.exclusive = operation->result == SyncResult::MutableReference; loan.separateParents = true;
                            loan.parents = parent; unite(loan.parents, retained);
                            for (const auto id : retained)
                                if (const auto source = state.loans.find(id); source != state.loans.end())
                                    for (auto target : source->second.targets)
                                    {
                                        // 보호된 T의 저장소와 보호자 자체의 생존 대여를 구분한다.
                                        // Distinguish protected T storage from the guard's own retained lifetime loan.
                                        appendStoragePath(function, target, {{AIRProjectionKind::Field, 1}});
                                        loan.targets.insert(std::move(target));
                                    }
                            const auto id = loanIds_[block][index]; state.loans[id] = std::move(loan); result.insert(id);
                        }
                    }
                }
                else if (instruction.opcode == AIROpcode::TaskIntrinsic)
                {
                    const auto* operation = taskOperation(instruction.operation);
                    if (operation && operation->result == TaskResult::Spawn && !instruction.operands.empty())
                    {
                        // 소유 캡처 전용 프로필에서도 Task<T>는 T와 별개로 Scope의 대여를 보관한다.
                        // In the owned-capture profile Task<T> retains a Scope loan independently of T.
                        result = value(state, instruction.operands[0]);
                    }
                    // join/try_take의 소유 결과에는 관측 핸들 자체의 스코프 대여를 옮기지 않는다.
                    // Owned join/try_take results do not inherit the observation handle's Scope loan.
                    // 대여 캡처·결과를 허용하기 전에는 별도의 ChildRoot 효과와 반환 출처가 필요하다.
                    // Borrowed captures/results require separate ChildRoot effects and result provenance first.
                }
                else if (instruction.opcode == AIROpcode::CancellationIntrinsic)
                {
                    // 취소 핸들은 독립적인 네이티브 상태를 소유하며 입력 설명자의 대여를 보관하지 않는다.
                    // Cancellation owners retain independent native state, never the input descriptor's loan.
                }
                else if (instruction.opcode == AIROpcode::AtomicIntrinsic)
                {
                    if (instruction.operation == "get_mut") result = value(state, instruction.operands[0]);
                }
                else if (instruction.opcode == AIROpcode::SharedIntrinsic)
                {
                    const auto method = instruction.operation.substr(instruction.operation.find('.') + 1);
                    if (method == "as_ref" || method == "get_mut")
                    {
                        const auto parents = value(state, instruction.operands[0]);
                        Loan borrowed; borrowed.exclusive = method == "get_mut"; borrowed.parents = parents; borrowed.separateParents = true;
                        for (const auto parent : parents)
                            if (const auto source = state.loans.find(parent); source != state.loans.end())
                                for (auto target : source->second.targets)
                                { target.opaqueStorage = true; borrowed.targets.insert(std::move(target)); }
                        const auto id = loanIds_[block][index]; state.loans[id] = std::move(borrowed); result = {id};
                    }
                    else if (method == "try_unwrap" || instruction.operation == "shared.new") result = value(state, instruction.operands[0]);
                    else if (method == "clone" || method == "downgrade" || method == "upgrade")
                    {
                        AIRPlace source; source.pointer = instruction.operands[0];
                        source.type = types_.types.get(function.valueTypes[instruction.operands[0]]).element;
                        result = readProvenance(function, source, state);
                    }
                    else if (method == "clear")
                    {
                        auto effect = instruction; effect.place = {}; effect.place.pointer = instruction.operands[0];
                        effect.place.type = types_.types.get(function.valueTypes[instruction.operands[0]]).element;
                        access(function, effect, state, true); updateStorage(function, effect, state, {});
                    }
                }
                else if (instruction.opcode == AIROpcode::MemoryIntrinsic)
                {
                    // 교체는 저장소를 비우지 않고 소유권과 내부 참조의 출처를 함께 옮긴다.
                    // Replacement preserves initialization while transferring ownership and contained-reference provenance.
                    auto left = instruction; left.place = {};
                    left.place.pointer = instruction.operands[0];
                    left.place.type = types_.types.get(function.valueTypes[instruction.operands[0]]).element;
                    access(function, left, state, true);
                    const auto oldLeft = readProvenance(function, left.place, state);
                    if (instruction.operation == "replace")
                    {
                        result = oldLeft; fields = readFields(function, left.place, state);
                        updateStorage(function, left, state, value(state, instruction.operands[1]), valueFields(state, instruction.operands[1]));
                    }
                    else
                    {
                        auto right = left; right.place.pointer = instruction.operands[1];
                        access(function, right, state, true);
                        const auto oldRight = readProvenance(function, right.place, state);
                        const auto leftFields = readFields(function, left.place, state), rightFields = readFields(function, right.place, state);
                        updateStorage(function, left, state, oldRight, rightFields); updateStorage(function, right, state, oldLeft, leftFields);
                    }
                }
                else if (instruction.opcode == AIROpcode::Write)
                {
                    access(function, instruction, state, true);
                    if (owner(instruction.place.type)
                        && (!instruction.place.projections.empty() || instruction.place.pointer != InvalidAIRId))
                        error(instruction.range, "Replacing an owning projected or borrowed value requires partial cleanup support.");
                    if (!instruction.operands.empty()) result = value(state, instruction.operands.front());
                    if (!instruction.operands.empty()) fields = valueFields(state, instruction.operands.front());
                    updateStorage(function, instruction, state, result, fields);
                    for (const auto& item : place(function, instruction.place, state).targets)
                        if (item.kind == OriginKind::Local && item.id < state.live.size() && item.projections.empty())
                        {
                            state.live[item.id] = 1;
                            std::erase_if(state.extractedFields, [&](const auto& field) { return std::get<0>(field) == item.id; });
                        }
                }
                else if (instruction.opcode == AIROpcode::VectorTake)
                {
                    access(function, instruction, state, true);
                    // 이동한 원소는 저장소 대여가 아니라 원소 안의 참조만 소유한다.
                    // The extracted owner retains element references rather than a loan of vector storage.
                    result = readProvenance(function, instruction.place, state);
                }
                else if (instruction.opcode == AIROpcode::VectorTruncate)
                {
                    access(function, instruction, state, true);
                    vectorCleanupEffects(function, instruction, state);
                    if (instruction.operation == "clear") updateStorage(function, instruction, state, {});
                }
                else if (instruction.opcode == AIROpcode::VectorAppend || instruction.opcode == AIROpcode::VectorReserve || instruction.opcode == AIROpcode::VectorInsert)
                {
                    access(function, instruction, state, true);
                    for (const auto operand : instruction.operands) unite(result, value(state, operand));
                    updateStorage(function, instruction, state, result, {}, true);
                }
                else if (instruction.opcode == AIROpcode::EnsurePersist)
                {
                    initializationEffects(function, instruction, instruction.place.global, state);
                    state.initializedPersist.insert(instruction.place.global);
                }
                else if (instruction.opcode == AIROpcode::Call)
                {
                    for (const auto storage : persistEnsures_[instruction.callee]) initializationEffects(function, instruction, storage, state);
                    if (unknownEffects_[instruction.callee] && !state.loans.empty())
                        error(instruction.range, "Cannot call a function with unknown external effects while loans are active.");
                    // 전역 접근의 호출 그래프 요약도 현재 대여와 비교하여 간접 충돌을 막는다.
                    // Compare transitive global access effects with active loans to reject indirect conflicts.
                    for (const auto& [global, write] : globalEffects_[instruction.callee])
                    {
                        auto effect = instruction;
                        effect.place = {};
                        effect.place.global = global;
                        access(function, effect, state, write);
                    }
                    const auto found = summaries_.find(instruction.callee);
                    if (found != summaries_.end())
                    {
                        // 호출 전 내용을 한 번 읽어 반환값과 교환된 두 저장소에 같은 스냅샷을 사용한다.
                        // Snapshot pre-call contents for both returned owners and exchanged destinations.
                        std::vector<IdSet> contents(instruction.operands.size());
                        for (std::size_t p = 0; p < instruction.operands.size(); ++p)
                        {
                            const auto argument = instruction.operands[p];
                            if (argument >= function.valueTypes.size()) continue;
                            const auto& argumentType = types_.types.get(function.valueTypes[argument]);
                            if (argumentType.kind == TypeKind::Reference)
                            {
                                AIRPlace location; location.pointer = argument; location.type = argumentType.element;
                                contents[p] = readProvenance(function, location, state);
                            }
                            else if (argumentType.kind == TypeKind::Slice)
                                for (const auto id : value(state, argument))
                                    if (const auto loan = state.loans.find(id); loan != state.loans.end() && loan->second.sliceContents)
                                        contents[p].insert(id);
                        }
                        const auto expand = [&](const ReferenceOrigins& origins)
                        {
                            IdSet expanded;
                            for (const auto p : origins.parameters)
                                if (p < instruction.operands.size()) unite(expanded, value(state, instruction.operands[p]));
                            const auto projected = [&](const IdSet& sources, const ReferencePath& path, bool opaque = false, const ReferencePath& physical = ReferencePath{})
                            {
                                if (path.empty() && physical.empty() && !opaque) return sources;
                                IdSet values;
                                for (const auto source : sources)
                                    if (const auto held = state.loans.find(source); held != state.loans.end())
                                    {
                                        const auto targets = held->second.targets; const bool exclusive = held->second.exclusive;
                                        for (auto target : targets)
                                        {
                                            if (opaque && target.opaqueStorage) target.storageProjection.clear();
                                            else appendStoragePath(function, target, path);
                                            target.opaqueStorage = target.opaqueStorage || opaque;
                                            appendStoragePath(function, target, physical);
                                            const auto id = symbolicLoan(state, std::move(target), exclusive);
                                            unite(state.loans[id].parents, sources); state.loans[id].parents.erase(id); state.loans[id].separateParents = true;
                                            values.insert(id);
                                        }
                                    }
                                return values;
                            };
                            for (const auto& selected : origins.parameterPaths)
                                if (selected.parameter < instruction.operands.size())
                                {
                                    const auto argument = instruction.operands[selected.parameter];
                                    const auto& argumentType = types_.types.get(function.valueTypes[argument]);
                                    if (argumentType.kind == TypeKind::Reference)
                                    {
                                        unite(expanded, projected(value(state, argument), selected.path, selected.opaqueStorage, selected.storageProjection));
                                    }
                                    else unite(expanded, projected(flatten(sourceFields(function, instruction, state, selected.parameter, false, selected.path)), {}, selected.opaqueStorage, selected.storageProjection));
                                }
                            for (const auto p : origins.contentParameters)
                                if (p < contents.size()) unite(expanded, contents[p]);
                            for (const auto& selected : origins.contentPaths)
                                unite(expanded, projected(flatten(sourceFields(function, instruction, state, selected.parameter, true, selected.path)), {}, selected.opaqueStorage, selected.storageProjection));
                            for (const auto p : origins.elementParameters)
                                if (p < instruction.operands.size())
                                    for (const auto id : value(state, instruction.operands[p]))
                                        if (const auto source = state.loans.find(id); source != state.loans.end() && source->second.sliceContents)
                                            expanded.insert(id);
                            if (!origins.globals.empty())
                            {
                                const auto id = loanIds_[block][index];
                                auto& loan = state.loans[id];
                                const auto& returnedType = types_.types.get(instruction.type);
                                loan.exclusive = returnedType.kind != TypeKind::Reference || returnedType.isMutable;
                                for (const auto global : origins.globals) loan.targets.insert({OriginKind::Global, global, {}});
                                expanded.insert(id);
                            }
                            return expanded;
                        };
                        const auto reconstruct = [&](IdSet sources, const ReferenceCapability& capability, const ReferencePath& path)
                        {
                            Loan returned; returned.exclusive = capability.exclusive; returned.separateParents = true;
                            for (const auto source : sources)
                                if (const auto loan = state.loans.find(source); loan != state.loans.end()) unite(returned.targets, loan->second.targets);
                            returned.parents = sources; unite(returned.parents, expand(capability.parents));
                            const auto key = std::make_pair(instruction.result, path);
                            if (returnedLoanIds_.size() + symbolicLoans_.size() >= maximumProvenanceEntries && !returnedLoanIds_.contains(key))
                            { provenanceLimit(); return IdSet{}; }
                            const auto [entry, inserted] = returnedLoanIds_.try_emplace(key, nextSymbolicLoan_);
                            if (inserted) ++nextSymbolicLoan_;
                            returned.parents.erase(entry->second); state.loans[entry->second] = std::move(returned);
                            return IdSet{entry->second};
                        };
                        result = expand(found->second);
                        for (const auto& [path, origins] : found->second.fields)
                        {
                            auto sources = expand(origins);
                            if (const auto capability = found->second.fieldCapabilities.find(path); capability != found->second.fieldCapabilities.end())
                                sources = reconstruct(std::move(sources), capability->second, path);
                            unite(fields[path], sources);
                        }
                        if (!fields.empty()) result = flatten(fields);
                        else if (found->second.derivedCapability)
                        {
                            result = reconstruct(std::move(result), {found->second.retainedParents, found->second.targetExclusive}, {});
                            fields = {{{}, result}};
                        }
                        struct StoreEffect { AIRInstruction operation; std::set<Target> targets; IdSet values; ProvenanceFields fields; bool append; };
                        std::vector<StoreEffect> stores;
                        for (const auto& [p, recorded] : found->second.stores)
                            if (p < instruction.operands.size())
                            {
                                auto effect = instruction; effect.place = {}; effect.place.pointer = instruction.operands[p];
                                const auto& argumentType = types_.types.get(function.valueTypes[instruction.operands[p]]);
                                effect.place.type = argumentType.element;
                                if (argumentType.kind == TypeKind::Slice) effect.place.projections.push_back({AIRProjectionKind::Index});
                                const auto detailed = found->second.storeFields.find(p);
                                if (detailed != found->second.storeFields.end())
                                {
                                    bool identity = !detailed->second.empty();
                                    ProvenanceFields output;
                                    for (const auto& [path, origins] : detailed->second)
                                    {
                                        const bool sameRoot = path.empty() && origins.contentParameters == IdSet{p} && origins.contentPaths.empty();
                                        const bool sameField = origins.contentParameters.empty()
                                            && origins.contentPaths == std::set<ReferenceParameterPath>{{p, path, {}, true, false}};
                                        identity = identity && (sameRoot || sameField) && origins.parameters.empty() && origins.parameterPaths.empty()
                                            && origins.elementParameters.empty() && origins.globals.empty() && !origins.global;
                                        unite(output[path], expand(origins));
                                    }
                                    // 평평한 출처가 같아도 필드 사이의 참조 재배치는 실제 효과다.
                                    // Reference redistribution between fields is an effect even when flat origins are unchanged.
                                    if (!identity) stores.push_back({effect, place(function, effect.place, state).targets, flatten(output), std::move(output), false});
                                }
                                else
                                    stores.push_back({effect, place(function, effect.place, state).targets, expand(recorded), {}, false});
                            }
                        for (const auto& [destination, origins] : found->second.indirectStores)
                        {
                            const auto references = flatten(sourceFields(function, instruction, state, destination.parameter, destination.fromContents, destination.path));
                            std::set<Target> targets;
                            for (const auto reference : references)
                                if (const auto loan = state.loans.find(reference); loan != state.loans.end())
                                    for (auto target : loan->second.targets)
                                    {
                                        appendStoragePath(function, target, destination.storageProjection);
                                        targets.insert(std::move(target));
                                    }
                            stores.push_back({instruction, std::move(targets), expand(origins), {}, true});
                        }
                        for (const auto& effect : stores) updateTargets(function, effect.operation, state, effect.targets, effect.values, effect.fields, effect.append);
                    }
                    else if (!state.loans.empty() || containsReference(instruction.type))
                        error(instruction.range, "Cannot call a function without ownership effects while loans are active.");

                }
                else if (instruction.opcode == AIROpcode::Aggregate || instruction.opcode == AIROpcode::EnumConstruct)
                {
                    const auto kind = types_.types.get(instruction.type).kind;
                    for (std::size_t i = 0; i < instruction.operands.size(); ++i)
                    {
                        const ReferenceProjection prefix{instruction.opcode == AIROpcode::EnumConstruct ? AIRProjectionKind::EnumField
                            : kind == TypeKind::Array || kind == TypeKind::Vector ? AIRProjectionKind::Index : AIRProjectionKind::Field,
                            kind == TypeKind::Array || kind == TypeKind::Vector ? 0 : i,
                            instruction.opcode == AIROpcode::EnumConstruct ? instruction.callee : InvalidSymbolId};
                        for (const auto& [path, values] : valueFields(state, instruction.operands[i]))
                        { auto nested = path; nested.insert(nested.begin(), prefix); unite(fields[nested], values); }
                    }
                    result = flatten(fields);
                }
                else if (instruction.opcode == AIROpcode::Unary || instruction.opcode == AIROpcode::TextIntrinsic
                    || instruction.opcode == AIROpcode::ArrayRepeat || instruction.opcode == AIROpcode::SliceSubview)
                {
                    if ((instruction.opcode != AIROpcode::Unary && instruction.opcode != AIROpcode::TextIntrinsic) || containsReference(instruction.type))
                        for (const auto operand : instruction.operands) unite(result, value(state, operand));
                }
                else if (instruction.opcode == AIROpcode::ScopeEnd)
                    endScope(function, instruction.scope, state);
                else if (instruction.opcode == AIROpcode::EnumTag)
                    access(function, instruction, state, false);
                else if (instruction.opcode == AIROpcode::Drop)
                {
                    destructorEffects(function, instruction, state);
                    if (instruction.place.local < state.live.size())
                    {
                        state.live[instruction.place.local] = 0;
                        state.locals[instruction.place.local].clear(); state.localFields[instruction.place.local].clear();
                    }
                }
                if (instruction.result < state.values.size())
                { state.valueFields[instruction.result] = normalizeFields(instruction.type, fieldFallback(fields, result)); state.values[instruction.result] = flatten(state.valueFields[instruction.result]); }
                prune(function, state, liveAfter_[block][index], localLiveAfter_[block][index]);
            }

            void summarizeOrigins(const AIRTerminator& terminator, const State& state,
                                  const IdSet& values, ReferenceOrigins& summary)
            {
                for (const auto id : values)
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
                        else if (origin.kind == OriginKind::ParameterContents)
                        { if (origin.projections.empty() && origin.storageProjection.empty() && !origin.opaqueStorage) summary.contentParameters.insert(origin.id);
                          else summary.contentPaths.insert({origin.id, origin.projections, origin.storageProjection, true, origin.opaqueStorage}); }
                        else if (origin.kind == OriginKind::Parameter)
                        { if (origin.projections.empty() && origin.storageProjection.empty() && !origin.opaqueStorage) summary.parameters.insert(origin.id);
                          else summary.parameterPaths.insert({origin.id, origin.projections, origin.storageProjection, true, origin.opaqueStorage}); }
                        else
                        {
                            summary.global = true;
                            summary.globals.insert(origin.id);
                        }
                    }
                }
            }

            void returned(const AIRFunction& function, const AIRTerminator& terminator,
                          const State& state, ReferenceReturnSummary& summary)
            {
                const auto returnedValues = value(state, terminator.value);
                summarizeOrigins(terminator, state, returnedValues, summary);
                for (const auto& [path, values] : valueFields(state, terminator.value))
                {
                    summarizeOrigins(terminator, state, values, summary.fields[path]);
                    for (const auto value : values)
                        if (const auto loan = state.loans.find(value); loan != state.loans.end() && loan->second.separateParents)
                        {
                            auto parents = loan->second.parents; permissionClosure(state, parents);
                            summarizeOrigins(terminator, state, parents, summary.fieldCapabilities[path].parents);
                            summary.fieldCapabilities[path].exclusive = summary.fieldCapabilities[path].exclusive || loan->second.exclusive;
                        }
                }
                for (const auto id : returnedValues)
                    if (const auto loan = state.loans.find(id); loan != state.loans.end() && loan->second.separateParents)
                    {
                        auto retained = loan->second.parents; permissionClosure(state, retained);
                        summarizeOrigins(terminator, state, retained, summary.retainedParents);
                        summary.derivedCapability = true;
                        summary.targetExclusive = summary.targetExclusive || loan->second.exclusive;
                    }
                for (std::size_t p = 0; p < function.parameters.size(); ++p)
                    if (mutableContentsParameter(function, p))
                    {
                        auto contents = state.parameterContents[p]; permissionClosure(state, contents);
                        summarizeOrigins(terminator, state, contents, summary.stores[p]);
                        auto& fields = summary.storeFields[p];
                        for (const auto& [path, values] : fieldFallback(state.parameterFields[p], state.parameterContents[p]))
                        {
                            auto held = values; permissionClosure(state, held);
                            summarizeOrigins(terminator, state, held, fields[path]);
                        }
                    }
                for (const auto& [path, values] : state.indirectStores)
                {
                    auto contents = values; permissionClosure(state, contents);
                    summarizeOrigins(terminator, state, contents, summary.indirectStores[path]);
                }
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
                while (!pending.empty() && !analysisLimit_)
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
