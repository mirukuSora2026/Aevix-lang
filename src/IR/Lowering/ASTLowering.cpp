// ========================
//
// Created By : miruku_sora2026
//
// File name : ASTLowering.cpp
//
// Description :
//     검사된 구문을 장소 연산과 명시적인 분기 블록으로 낮춘다.
//     Lower checked syntax into place operations and explicit branch blocks.
//
// ========================

#include "aevix/IR/Lowering/ASTLowering.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace aevix
{
    namespace
    {
        const Node* child(const Node& node, const std::string& kind)
        {
            for (const auto& item : node.children) if (item->kind == kind) return item.get();
            return nullptr;
        }
        bool flag(const Node& node, const std::string& name)
        {
            const auto* modifiers = child(node, "Modifier");
            return modifiers && (" " + modifiers->value + " ").find(" " + name + " ") != std::string::npos;
        }
        const Node* initializer(const Node& node)
        {
            for (std::size_t i = 1; i < node.children.size(); ++i)
                if (node.children[i]->kind != "Modifier" && node.children[i]->kind != "ArraySuffixes")
                    return node.children[i].get();
            return nullptr;
        }
        bool numericLiteralSyntax(const Node& node)
        {
            if (node.kind == "Group" || (node.kind == "Prefix" && (node.value == "+" || node.value == "-")))
                return !node.children.empty() && numericLiteralSyntax(*node.children[0]);
            return node.kind == "Literal" && !node.value.empty() && node.value.front() >= '0' && node.value.front() <= '9';
        }
        class Lowerer final
        {
        public:
            Lowerer(const ModuleGraph& graph, const NameResolution& names, const TypeCheckResult& types, Diagnostics& diagnostics)
                : graph_(graph), names_(names), typed_(types), diagnostics_(diagnostics) {}
            std::optional<AIRModule> run()
            {
                for (const auto& symbol : names_.symbols)
                    if (symbol.declaration) declarations_.emplace(symbol.declaration, symbol.id);
                for (const auto& module : graph_.modules)
                    for (const auto& declaration : module->ast->children) collect(*declaration, false);
                const auto ordinaryFunctions = result_.functions.size();
                for (std::size_t index = 0; index < ordinaryFunctions; ++index) collectLocalPersist(*result_.functions[index].source);
                for (const auto& global : result_.globals)
                {
                    globalSymbols_.insert(global.symbol);
                    std::unordered_set<TypeId> visiting;
                    if (containsReference(global.type, visiting) && !global.isStaticText && global.source)
                        error(*global.source, "reference-containing global storage requires initialization provenance analysis and is not supported in this stage.");
                }
                for (auto& global : result_.globals)
                {
                    if (!global.isPersist) continue;
                    checkPersistInitializer(global);
                    AIRFunction initializerFunction;
                    initializerFunction.symbol = names_.symbols.size() + result_.functions.size();
                    initializerFunction.name = "$persist.init." + global.name;
                    initializerFunction.source = global.source;
                    initializerFunction.result = typed_.types.builtin("void");
                    initializerFunction.initializesGlobal = global.symbol;
                    global.initializerFunction = initializerFunction.symbol;
                    result_.functions.push_back(std::move(initializerFunction));
                }
                for (std::size_t index = 0; index < result_.functions.size(); ++index)
                {
                    function_ = &result_.functions[index];
                    if (function_->initializesGlobal == InvalidSymbolId) lowerFunction(*function_->source);
                    else lowerInitializer();
                }
                insertGlobalEnsures();
                orderInitializers();
                if (!diagnostics_.empty()) return std::nullopt;
                return std::move(result_);
            }
        private:
            const ModuleGraph& graph_;
            const NameResolution& names_;
            const TypeCheckResult& typed_;
            Diagnostics& diagnostics_;
            AIRModule result_;
            AIRFunction* function_ = nullptr;
            AIRInstruction discardedInstruction_;
            AIRBlockId block_ = 0;
            std::vector<AIRScopeId> scopes_;
            std::unordered_map<const Node*, SymbolId> declarations_;
            std::unordered_map<SymbolId, AIRLocalId> locals_;
            std::unordered_set<SymbolId> globalSymbols_;
            struct Loop { AIRBlockId stop; AIRBlockId next; std::size_t breakDepth; std::size_t continueDepth; };
            std::vector<Loop> loops_;
            std::unordered_map<const Node*, TypeId> originalDynTypes_;
            std::unordered_set<const Node*> loweringDynViews_;
            TypeId type(const Node& node) const
            {
                if (const auto original = originalDynTypes_.find(&node); original != originalDynTypes_.end()) return original->second;
                const auto found = typed_.nodeTypes.find(&node);
                return found == typed_.nodeTypes.end() ? typed_.types.error() : found->second;
            }
            SymbolId id(const Node& node) const
            {
                const auto declared = declarations_.find(&node);
                if (declared != declarations_.end()) return declared->second;
                const auto found = names_.references.find(&node);
                return found != names_.references.end() && found->second.size() == 1 ? found->second[0] : InvalidSymbolId;
            }
            TypeId symbolType(SymbolId symbol) const
            {
                const auto found = typed_.symbolTypes.find(symbol);
                return found == typed_.symbolTypes.end() ? typed_.types.error() : found->second;
            }
            void error(const Node& node, const std::string& detail)
            {
                diagnostics_.push_back({node.range, "AIR lowering: " + detail});
            }
            void collect(const Node& node, bool member)
            {
                if (child(node, "CompilerBox")) return;
                if (node.kind == "CompilerTemplate" || node.kind == "CompilerContract"
                    || child(node, "CompilerTemplate") || child(node, "CompilerContract") || child(node, "CompilerIntrinsic")) return;
                if (node.kind == "Function" && child(node, "Block") && typed_.symbolTypes.contains(id(node)))
                {
                    AIRFunction function;
                    function.symbol = id(node);
                    function.source = &node;
                    function.name = node.value;
                    function.result = typed_.types.get(symbolType(function.symbol)).result;
                    if (!member && node.value == "main" && function.symbol < names_.symbols.size()
                        && names_.symbols[function.symbol].module == graph_.entry)
                    {
                        if (result_.entry != InvalidSymbolId) error(node, "more than one entry function named main.");
                        result_.entry = function.symbol;
                    }
                    result_.functions.push_back(std::move(function));
                }
                else if (node.kind == "Function" && flag(node, "extern") && typed_.symbolTypes.contains(id(node)))
                {
                    const auto& signature = typed_.types.get(symbolType(id(node)));
                    result_.externals.push_back({id(node), node.value, signature.result, signature.parameters, false, "C"});
                }
                else if ((node.kind == "Struct" || node.kind == "Enum") && typed_.symbolTypes.contains(id(node)))
                {
                    AIRRecord record;
                    record.type = symbolType(id(node));
                    record.isEnum = node.kind == "Enum";
                    if (record.isEnum)
                        for (const auto& variant : node.children)
                            if (variant->kind == "Variant")
                            {
                                const auto found = typed_.enumVariants.find(id(*variant));
                                if (found != typed_.enumVariants.end()) record.variants.push_back(found->second);
                            }
                    for (const auto& field : node.children)
                        if (field->kind == "Variable" && !flag(*field, "persist"))
                        {
                            record.fields.push_back(id(*field));
                            record.fieldTypes.push_back(symbolType(id(*field)));
                        }
                    result_.records.push_back(std::move(record));
                    for (const auto& item : node.children) collect(*item, true);
                }
                else if (node.kind == "Variant")
                {
                    for (const auto& item : node.children) if (item->kind == "Function") collect(*item, true);
                }
                else if (node.kind == "Variable" && !member) addGlobal(node, false);
            }
            void addGlobal(const Node& node, bool localPersist)
            {
                AIRGlobal global;
                global.symbol = id(node); global.name = node.value; global.type = type(node);
                global.isMutable = flag(node, "mut"); global.initializer = initializer(node); global.source = &node;
                global.isConstant = flag(node, "const"); global.isLocalPersist = localPersist;
                // 정적 문자열 상수는 지역 대여를 저장하지 않는다는 사실을 기록한다.
                // Record that a static text constant stores no borrow of local storage.
                if (global.isConstant && !global.isMutable && global.initializer
                    && typed_.types.get(global.type).kind == TypeKind::StringView)
                {
                    const auto evaluated = ConstEvaluator(names_, typed_.types, typed_.nodeTypes, typed_.symbolTypes)
                        .evaluate(*global.initializer, global.type);
                    global.isStaticText = evaluated.status == ConstStatus::Constant
                        && evaluated.value.kind == ConstantKind::String;
                }
                global.isPersist = !global.isConstant && (global.initializer || flag(node, "persist"));
                std::ostringstream key;
                const auto module = global.symbol < names_.symbols.size() ? names_.symbols[global.symbol].module : InvalidModuleId;
                key << (module < graph_.modules.size() ? graph_.modules[module]->name : "") << ':'
                    << std::setfill('0') << std::setw(20) << node.range.begin.offset << ':' << node.value;
                if (localPersist) key << ':' << global.symbol;
                global.initializationOrderKey = key.str();
                result_.globals.push_back(std::move(global));
            }
            void collectLocalPersist(const Node& node)
            {
                if (node.kind == "CompilerTemplate" || node.kind == "CompilerContract" || child(node, "CompilerTemplate") || child(node, "CompilerContract")) return;
                if (node.kind == "Variable" && flag(node, "persist")) { addGlobal(node, true); return; }
                for (const auto& item : node.children) collectLocalPersist(*item);
            }
            void checkPersistInitializer(const AIRGlobal& global)
            {
                if (!global.source) return;
                if (!global.initializer) { error(*global.source, "persist requires an initializer at its declaration."); return; }
                if (flag(*global.source, "persist") && global.isMutable)
                    error(*global.source, "shared persist mutation requires an explicit synchronization type; plain mut persist is not supported.");
                std::function<void(const Node&)> visit = [&](const Node& node)
                {
                    if (typed_.propagations.contains(&node))
                        error(node, "persist initialization cannot propagate with ?; store the Result or Optional value explicitly.");
                    const auto symbol = id(node);
                    if (global.isLocalPersist && (node.kind == "Name" || node.kind == "Member") && symbol < names_.symbols.size())
                    {
                        const auto* declaration = names_.symbols[symbol].declaration;
                        if (declaration && (declaration->kind == "Parameter" || declaration->kind == "VariadicParameter" || declaration->kind == "PatternBinding" || declaration->kind == "Binding"
                            || (declaration->kind == "Variable" && !globalSymbols_.contains(symbol) && !flag(*declaration, "const"))))
                            error(node, "a local persist initializer cannot depend directly on a parameter or ordinary local variable.");
                    }
                    for (const auto& item : node.children) visit(*item);
                };
                visit(*global.initializer);
            }
            void lowerInitializer()
            {
                locals_.clear(); scopes_.clear(); loops_.clear();
                block_ = block("persist.entry"); enter(function_->source);
                const AIRGlobal* storage = nullptr;
                for (const auto& global : result_.globals) if (global.symbol == function_->initializesGlobal) storage = &global;
                if (storage && storage->initializer)
                {
                    const auto value = expression(*storage->initializer);
                    if (!terminated())
                    {
                        write(symbolPlace(storage->symbol, storage->type), value, *storage->source);
                        function_->blocks[block_].instructions.back().operation = "persist.initialize";
                    }
                }
                leave(function_->source);
                if (!terminated()) function_->blocks[block_].terminator.kind = AIRTerminatorKind::Return;
                scopes_.clear();
            }
            void insertGlobalEnsures()
            {
                for (auto& function : result_.functions)
                    for (auto& block : function.blocks)
                    {
                        std::vector<AIRInstruction> instructions;
                        for (auto& item : block.instructions)
                        {
                            const AIRGlobal* storage = nullptr;
                            for (const auto& global : result_.globals) if (global.symbol == item.place.global && global.isPersist) storage = &global;
                            const bool initializeWrite = item.opcode == AIROpcode::Write && item.operation == "persist.initialize" && function.initializesGlobal == item.place.global;
                            const bool access = item.opcode == AIROpcode::Read || item.opcode == AIROpcode::Move || item.opcode == AIROpcode::Write
                                || item.opcode == AIROpcode::BorrowShared || item.opcode == AIROpcode::BorrowMut || item.opcode == AIROpcode::EnumTag;
                            if (storage && access && !initializeWrite)
                            {
                                AIRInstruction ensure; ensure.opcode = AIROpcode::EnsurePersist;
                                ensure.place.global = storage->symbol; ensure.place.type = storage->type;
                                ensure.scope = item.scope; ensure.source = item.source; ensure.range = item.range;
                                instructions.push_back(std::move(ensure));
                            }
                            instructions.push_back(std::move(item));
                        }
                        block.instructions = std::move(instructions);
                    }
            }
            void orderInitializers()
            {
                std::map<SymbolId, std::set<SymbolId>> effects;
                bool changed = true;
                while (changed)
                {
                    changed = false;
                    for (const auto& function : result_.functions)
                    {
                        auto next = effects[function.symbol];
                        for (const auto& block : function.blocks)
                            for (const auto& item : block.instructions)
                            {
                                if (item.place.global != InvalidSymbolId && !(item.opcode == AIROpcode::Write && item.operation == "persist.initialize")) next.insert(item.place.global);
                                if (item.opcode == AIROpcode::Call) next.insert(effects[item.callee].begin(), effects[item.callee].end());
                            }
                        if (next != effects[function.symbol]) { effects[function.symbol] = std::move(next); changed = true; }
                    }
                }
                std::map<SymbolId, AIRGlobal*> globals;
                for (auto& global : result_.globals)
                {
                    globals[global.symbol] = &global;
                    if (global.isPersist) global.dependencies.assign(effects[global.initializerFunction].begin(), effects[global.initializerFunction].end());
                }
                std::vector<AIRGlobal*> roots;
                for (auto& global : result_.globals) if (global.isPersist && !global.isLocalPersist) roots.push_back(&global);
                auto less = [](const AIRGlobal* left, const AIRGlobal* right) { return left->initializationOrderKey < right->initializationOrderKey; };
                std::sort(roots.begin(), roots.end(), less);
                std::map<SymbolId, unsigned> state;
                std::function<void(AIRGlobal*)> visit = [&](AIRGlobal* global)
                {
                    if (state[global->symbol] == 2 || !global->isPersist) return;
                    if (state[global->symbol] == 1) { error(*global->source, "cyclic persist initialization dependency."); return; }
                    state[global->symbol] = 1;
                    std::vector<AIRGlobal*> dependencies;
                    for (const auto symbol : global->dependencies) if (globals.contains(symbol) && globals[symbol]->isPersist) dependencies.push_back(globals[symbol]);
                    std::sort(dependencies.begin(), dependencies.end(), less);
                    for (auto* dependency : dependencies) visit(dependency);
                    state[global->symbol] = 2;
                    if (!global->isLocalPersist) result_.initializationOrder.push_back(global->symbol);
                };
                for (auto* root : roots) visit(root);
            }
            AIRBlockId block(const std::string& name)
            {
                function_->blocks.push_back({name, {}, {}});
                return function_->blocks.size() - 1;
            }
            bool terminated() const
            {
                return function_->blocks[block_].terminator.kind != AIRTerminatorKind::Unreachable;
            }
            AIRInstruction& instruction(AIROpcode opcode, const Node* source = nullptr, TypeId valueType = InvalidTypeId, bool result = false)
            {
                // 종료된 경로의 바깥 표현식은 값이나 부수 효과를 더 만들지 않는다.
                // Enclosing expressions emit no further values or effects after divergence.
                if (terminated())
                {
                    discardedInstruction_ = {};
                    discardedInstruction_.opcode = opcode;
                    discardedInstruction_.type = valueType;
                    return discardedInstruction_;
                }
                AIRInstruction value;
                value.opcode = opcode;
                value.type = valueType;
                value.source = source;
                if (source) value.range = source->range;
                value.scope = scopes_.empty() ? 0 : scopes_.back();
                if (result)
                {
                    value.result = function_->valueTypes.size();
                    function_->valueTypes.push_back(valueType);
                }
                function_->blocks[block_].instructions.push_back(std::move(value));
                return function_->blocks[block_].instructions.back();
            }
            void jump(AIRBlockId destination, const Node* source = nullptr, std::size_t depth = InvalidAIRId)
            {
                if (terminated()) return;
                auto& end = function_->blocks[block_].terminator;
                end.kind = AIRTerminatorKind::Jump;
                end.target = destination;
                end.source = source;
                if (source) end.range = source->range;
                if (depth != InvalidAIRId)
                    for (std::size_t index = scopes_.size(); index > depth; --index) end.exitedScopes.push_back(scopes_[index - 1]);
            }
            void branch(AIRValueId condition, AIRBlockId yes, AIRBlockId no, const Node& source)
            {
                if (terminated()) return;
                auto& end = function_->blocks[block_].terminator;
                end.kind = AIRTerminatorKind::Branch;
                end.value = condition;
                end.target = yes;
                end.alternative = no;
                end.source = &source;
                end.range = source.range;
            }
            void enter(const Node* source = nullptr)
            {
                AIRScope scope;
                scope.parent = scopes_.empty() ? InvalidAIRId : scopes_.back();
                function_->scopes.push_back(std::move(scope));
                scopes_.push_back(function_->scopes.size() - 1);
                instruction(AIROpcode::ScopeBegin, source).scope = scopes_.back();
            }
            void leave(const Node* source = nullptr)
            {
                if (!terminated()) instruction(AIROpcode::ScopeEnd, source).scope = scopes_.back();
                scopes_.pop_back();
            }
            AIRLocalId local(const Node* source, TypeId valueType, std::string name = {})
            {
                const auto symbol = source ? id(*source) : InvalidSymbolId;
                const auto slot = function_->locals.size();
                function_->locals.push_back({source ? source->value : std::move(name), symbol, valueType,
                                             source && flag(*source, "mut"), false, scopes_.back(), source});
                function_->scopes[scopes_.back()].locals.push_back(slot);
                if (symbol != InvalidSymbolId) locals_[symbol] = slot;
                return slot;
            }
            AIRPlace localPlace(AIRLocalId index) const
            {
                AIRPlace answer;
                answer.local = index;
                answer.type = function_->locals[index].type;
                return answer;
            }
            AIRPlace symbolPlace(SymbolId symbol, TypeId valueType) const
            {
                AIRPlace answer;
                if (const auto found = locals_.find(symbol); found != locals_.end()) answer.local = found->second;
                else answer.global = symbol;
                answer.type = valueType;
                return answer;
            }
            const AIRRecord* record(TypeId valueType) const
            {
                for (const auto& item : result_.records) if (item.type == valueType) return &item;
                return nullptr;
            }
            AIRValueId pointerExpression(const Node& node)
            {
                if (node.kind == "Name" || node.kind == "Member" || node.kind == "Index" || node.kind == "Group"
                    || (node.kind == "Prefix" && node.value == "*")) return read(place(node), node, false);
                return expression(node);
            }
            AIRPlace place(const Node& node)
            {
                if (node.kind == "Group") return place(*node.children[0]);
                if (node.kind == "Name" || (node.kind == "Member" && id(node) != InvalidSymbolId))
                    return symbolPlace(id(node), type(node));
                if (node.kind == "Prefix" && node.value == "*")
                {
                    AIRPlace answer;
                    answer.pointer = pointerExpression(*node.children[0]);
                    answer.type = type(node);
                    return answer;
                }
                if (node.kind == "Member" || node.kind == "Index")
                {
                    const auto& receiver = *node.children[0];
                    const auto& receiverType = typed_.types.get(type(receiver));
                    AIRPlace answer;
                    TypeId baseType = type(receiver);
                    if (receiverType.kind == TypeKind::Reference)
                    {
                        answer.pointer = pointerExpression(receiver);
                        baseType = receiverType.element;
                    }
                    else answer = place(receiver);
                    if (node.kind == "Index")
                    {
                        const auto index = expression(*node.children[1]);
                        const auto& array = typed_.types.get(baseType);
                        answer.projections.push_back({AIRProjectionKind::Index, 0, index, type(node), array.extent});
                    }
                    else
                    {
                        const auto* fields = record(baseType);
                        std::size_t field = InvalidAIRId;
                        if (fields)
                            for (std::size_t index = 0; index < fields->fields.size(); ++index)
                                if (names_.symbols[fields->fields[index]].name == node.value) field = index;
                        if (field == InvalidAIRId) error(node, "unresolved struct field.");
                        answer.projections.push_back({AIRProjectionKind::Field, field, InvalidAIRId, type(node), 0});
                    }
                    answer.type = type(node);
                    return answer;
                }
                // 집합 임시값의 필드도 하나의 주소 가능한 임시 저장소를 사용한다.
                // Fields of aggregate temporaries use one addressable temporary storage location.
                const auto value = expression(node);
                const auto temporary = local(nullptr, type(node), "$temporary");
                write(localPlace(temporary), value, node);
                return localPlace(temporary);
            }
            bool containsReference(TypeId valueType, std::unordered_set<TypeId>& visiting) const
            {
                const auto& value = typed_.types.get(valueType);
                if (value.kind == TypeKind::Reference || value.kind == TypeKind::Slice || value.kind == TypeKind::StringView) return true;
                if (!visiting.insert(valueType).second) return false;
                if (value.kind == TypeKind::Box && typed_.types.get(value.element).kind == TypeKind::Dynamic) return true;
                if (value.kind == TypeKind::Array || value.kind == TypeKind::Vector || value.kind == TypeKind::Box) return containsReference(value.element, visiting);
                if (value.kind == TypeKind::Tuple)
                { for (const auto field : value.parameters) if (containsReference(field, visiting)) return true; return false; }
                if (value.kind != TypeKind::Nominal) return false;
                const auto* description = record(valueType);
                if (!description) return false;
                for (const auto field : description->fieldTypes) if (containsReference(field, visiting)) return true;
                for (const auto& variant : description->variants)
                    for (const auto field : variant.fieldTypes) if (containsReference(field, visiting)) return true;
                return false;
            }
            bool copyType(TypeId valueType) const
            {
                const auto& value = typed_.types.get(valueType);
                if (value.kind == TypeKind::Reference || value.kind == TypeKind::Slice) return !value.isMutable;
                if (value.kind == TypeKind::Array || value.kind == TypeKind::Tuple || value.kind == TypeKind::Vector || value.kind == TypeKind::Box || value.kind == TypeKind::String) return false;
                if (value.kind != TypeKind::Nominal) return true;
                if (value.declaration >= names_.symbols.size()) return false;
                const auto* declaration = names_.symbols[value.declaration].declaration;
                const auto* interfaces = declaration ? child(*declaration, "Implements") : nullptr;
                if (interfaces) for (const auto& item : interfaces->children) if (item->value == "Copy") return true;
                return false;
            }
            AIRValueId read(AIRPlace address, const Node& node, bool consume = true)
            {
                auto& item = instruction(consume && !copyType(address.type) ? AIROpcode::Move : AIROpcode::Read, &node, address.type, true);
                item.place = std::move(address);
                return item.result;
            }
            void write(AIRPlace address, AIRValueId value, const Node& node)
            {
                auto& item = instruction(AIROpcode::Write, &node, address.type);
                item.place = std::move(address);
                item.operands.push_back(value);
            }
            AIRValueId constant(TypeId valueType, const std::string& spelling, const Node* source = nullptr)
            {
                auto& item = instruction(AIROpcode::Constant, source, valueType, true);
                item.operation = spelling;
                return item.result;
            }
            AIRValueId binary(const std::string& operation, AIRValueId left, AIRValueId right, TypeId valueType, const Node& node)
            {
                auto& item = instruction(AIROpcode::Binary, &node, valueType, true);
                item.operation = operation;
                item.operands = {left, right};
                return item.result;
            }
            AIRValueId condition(const Node& node, bool logical)
            {
                const bool diverges = typed_.types.get(type(node)).kind == TypeKind::Never;
                const auto slot = diverges ? InvalidAIRId : local(nullptr, type(node), "$conditional");
                const auto left = expression(*node.children[0]);
                if (terminated()) return InvalidAIRId;
                const auto yes = block("conditional.yes");
                const auto no = block("conditional.no");
                const auto done = block("conditional.end");
                branch(left, yes, no, node);
                block_ = yes;
                const auto yesValue = logical && node.value == "||" ? constant(type(node), "true", &node)
                    : expression(*node.children[1]);
                if (slot != InvalidAIRId) write(localPlace(slot), yesValue, node);
                jump(done);
                block_ = no;
                const auto noValue = logical ? (node.value == "&&" ? constant(type(node), "false", &node)
                    : expression(*node.children[1])) : expression(*node.children[2]);
                if (slot != InvalidAIRId) write(localPlace(slot), noValue, node);
                jump(done);
                block_ = done;
                if (diverges)
                {
                    function_->blocks[block_].terminator.kind = AIRTerminatorKind::Trap;
                    return InvalidAIRId;
                }
                return read(localPlace(slot), node);
            }
            AIRValueId argumentExpression(const Node& node, TypeId expected)
            {
                if (typed_.dynViews.contains(&node)) return expression(node);
                const auto& wanted = typed_.types.get(expected);
                const auto& actual = typed_.types.get(type(node));
                if (wanted.kind == TypeKind::Reference && actual.kind == TypeKind::Reference
                    && (node.kind == "Name" || node.kind == "Member" || node.kind == "Group"))
                {
                    const auto pointer = read(place(node), node, false);
                    AIRPlace address;
                    address.pointer = pointer;
                    address.type = wanted.element;
                    auto& loan = instruction(wanted.isMutable ? AIROpcode::BorrowMut : AIROpcode::BorrowShared, &node, expected, true);
                    loan.place = address;
                    if (typed_.types.get(wanted.element).kind == TypeKind::Dynamic) loan.operation = "dyn.reborrow";
                    return loan.result;
                }
                return expression(node);
            }
            AIRValueId call(const Node& node)
            {
                const auto selected = typed_.calls.find(&node);
                if (selected == typed_.calls.end()) { error(node, "call has no selected target."); return InvalidAIRId; }
                const auto target = selected->second;
                const auto* declaration = names_.symbols[target].declaration;
                const auto* parameters = declaration ? child(*declaration, "Parameters") : nullptr;
                if (!parameters) { error(node, "call has no parameter declaration."); return InvalidAIRId; }
                std::vector<AIRValueId> arguments(parameters->children.size(), InvalidAIRId);
                std::vector<AIRLocalId> argumentOwners(parameters->children.size(), InvalidAIRId);
                const bool variadic = !parameters->children.empty() && parameters->children.back()->kind == "VariadicParameter";
                if (variadic) enter(&node);
                const auto packSlot = variadic ? parameters->children.size() - 1 : parameters->children.size();
                std::vector<AIRValueId> packValues;
                std::vector<AIRLocalId> packOwners;
                const auto retainArgument = [&](std::size_t index, AIRValueId value, const Node& source)
                {
                    const auto wanted = symbolType(id(*parameters->children[index]));
                    if (!copyType(wanted))
                    {
                        const auto slot = local(nullptr, wanted, "$argument");
                        write(localPlace(slot), value, source); argumentOwners[index] = slot;
                    }
                    else arguments[index] = value;
                };
                std::size_t positional = 0;
                const auto& callee = *node.children[0];
                if (!parameters->children.empty() && parameters->children[0]->value == "self"
                    && callee.kind == "Member" && id(callee) == InvalidSymbolId)
                {
                    const auto& receiver = *callee.children[0];
                    const auto wanted = symbolType(id(*parameters->children[0]));
                    const auto& receiverType = typed_.types.get(wanted);
                    const auto& actualReceiver = typed_.types.get(type(receiver));
                    const auto actualBase = actualReceiver.kind == TypeKind::Reference ? actualReceiver.element : type(receiver);
                    if (receiverType.kind == TypeKind::Reference && typed_.types.get(actualBase).kind == TypeKind::Box)
                    {
                        AIRPlace address;
                        if (actualReceiver.kind == TypeKind::Reference) { address.pointer = pointerExpression(receiver); address.type = actualBase; }
                        else address = place(receiver);
                        auto& loan = instruction(receiverType.isMutable ? AIROpcode::BorrowMut : AIROpcode::BorrowShared, &receiver, wanted, true);
                        loan.place = address; loan.operation = "box.borrow"; retainArgument(0, loan.result, receiver);
                    }
                    else if (receiverType.kind == TypeKind::Reference && typed_.types.get(type(receiver)).kind != TypeKind::Reference)
                    {
                        const auto address = place(receiver);
                        auto& loan = instruction(receiverType.isMutable ? AIROpcode::BorrowMut : AIROpcode::BorrowShared, &receiver, wanted, true);
                        loan.place = address;
                        retainArgument(0, loan.result, receiver);
                    }
                    else retainArgument(0, argumentExpression(receiver, wanted), receiver);
                    positional = 1;
                }
                for (std::size_t index = 1; index < node.children.size(); ++index)
                {
                    const auto& argument = *node.children[index];
                    std::size_t slot = positional++;
                    if (!argument.value.empty())
                    {
                        slot = arguments.size();
                        for (std::size_t item = 0; item < parameters->children.size(); ++item)
                            if (parameters->children[item]->value == argument.value) slot = item;
                    }
                    if (variadic && argument.value.empty() && slot >= packSlot)
                    {
                        const auto element = typed_.types.get(symbolType(id(*parameters->children.back()))).element;
                        const auto value = argumentExpression(*argument.children[0], element);
                        packValues.push_back(value); packOwners.push_back(InvalidAIRId);
                        if (!copyType(element))
                        {
                            const auto owner = local(nullptr, element, "$packelement");
                            write(localPlace(owner), value, argument); packOwners.back() = owner;
                        }
                        continue;
                    }
                    const auto value = slot < parameters->children.size() ? argumentExpression(*argument.children[0], symbolType(id(*parameters->children[slot]))) : expression(*argument.children[0]);
                    if (slot < arguments.size()) retainArgument(slot, value, *argument.children[0]);
                    else error(argument, "invalid bound parameter slot.");
                }
                if (variadic)
                {
                    for (std::size_t index = 0; index < packValues.size(); ++index)
                        if (packOwners[index] != InvalidAIRId) packValues[index] = read(localPlace(packOwners[index]), node, true);
                    const auto arrayType = typed_.variadicPacks.at(&node);
                    auto& aggregate = instruction(AIROpcode::Aggregate, &node, arrayType, true);
                    aggregate.operands = std::move(packValues);
                    const auto value = aggregate.result;
                    const auto storage = local(nullptr, arrayType, "$variadicpack"); write(localPlace(storage), value, node);
                    auto& view = instruction(AIROpcode::SliceConstruct, &node, symbolType(id(*parameters->children.back())), true);
                    view.place = localPlace(storage); arguments[packSlot] = view.result;
                }
                for (std::size_t index = 0; index < packSlot; ++index)
                    if (arguments[index] == InvalidAIRId && argumentOwners[index] == InvalidAIRId)
                    {
                        const auto* fallback = child(*parameters->children[index], "Default");
                        if (fallback) retainArgument(index, expression(*fallback->children[0]), *fallback->children[0]);
                        else error(node, "missing bound argument.");
                    }
                for (std::size_t index = 0; index < argumentOwners.size(); ++index)
                    if (argumentOwners[index] != InvalidAIRId) arguments[index] = read(localPlace(argumentOwners[index]), node, true);
                if (terminated()) { if (variadic) leave(&node); return InvalidAIRId; }
                const auto resultKind = typed_.types.get(type(node)).kind;
                auto& item = instruction(AIROpcode::Call, &node, type(node),
                    resultKind != TypeKind::Void && resultKind != TypeKind::Never);
                item.callee = target;
                if (const auto found = typed_.dynCalls.find(&node); found != typed_.dynCalls.end())
                {
                    item.operation = "dyn.call." + std::to_string(found->second.slot);
                    item.dynamicTargets = found->second.targets;
                    item.dynamicSignature = symbolType(target);
                }
                item.operands = std::move(arguments);
                const auto result = item.result;
                if (resultKind == TypeKind::Never)
                {
                    auto& end = function_->blocks[block_].terminator;
                    end.kind = AIRTerminatorKind::NoReturn;
                    end.source = &node;
                    end.range = node.range;
                }
                if (variadic) leave(&node);
                return result;
            }
            AIRValueId rangeResult(const Node& node, bool length)
            {
                auto descriptor = expression(*node.children[1]->children[0]);
                if (length)
                {
                    AIRPlace address; address.pointer = descriptor; address.type = typed_.types.get(type(*node.children[1]->children[0])).element;
                    descriptor = read(address, node, false);
                }
                const auto& returned = typed_.carriers.at(type(node));
                AIRValueId success, payload;
                if (length)
                {
                    auto& count = instruction(AIROpcode::RangeCount, &node, typed_.types.builtin("usize"), true); count.operands = {descriptor}; payload = count.result;
                    auto& fits = instruction(AIROpcode::RangeCountFits, &node, typed_.types.builtin("bool"), true); fits.operands = {descriptor}; success = fits.result;
                }
                else
                {
                    const auto step = expression(*node.children[2]->children[0]);
                    const auto stepType = type(*node.children[2]->children[0]);
                    const auto zero = constant(stepType, "0", &node); success = binary("!=", step, zero, typed_.types.builtin("bool"), node);
                    auto& stepped = instruction(AIROpcode::RangeWithStep, &node, returned.value, true); stepped.operands = {descriptor, step}; payload = stepped.result;
                }
                const auto resultSlot = local(nullptr, type(node), "$range.result");
                const auto yes = block("range.result.ok"), no = block("range.result.error"), done = block("range.result.done");
                branch(success, yes, no, node); block_ = yes;
                auto& good = instruction(AIROpcode::EnumConstruct, &node, type(node), true); good.callee = returned.success; good.operands = {payload}; const auto ok = good.result;
                write(localPlace(resultSlot), ok, node); jump(done, &node); block_ = no;
                SymbolId variant = InvalidSymbolId;
                for (const auto& [candidate, info] : typed_.enumVariants)
                    if (info.type == returned.error && names_.symbols[candidate].name == (length ? "CountOverflow" : "InvalidStep")) variant = candidate;
                if (variant == InvalidSymbolId) { error(node, "Missing RangeError variant."); return InvalidAIRId; }
                auto& errorValue = instruction(AIROpcode::EnumConstruct, &node, returned.error, true); errorValue.callee = variant; const auto error = errorValue.result;
                auto& bad = instruction(AIROpcode::EnumConstruct, &node, type(node), true); bad.callee = returned.failure; bad.operands = {error}; const auto fail = bad.result;
                write(localPlace(resultSlot), fail, node); jump(done, &node); block_ = done;
                return read(localPlace(resultSlot), node, true);
            }
            AIRValueId sliceExpression(const Node& node)
            {
                const auto& carrier = typed_.carriers.at(type(node));
                const auto source = expression(*node.children[1]);
                const auto start = expression(*node.children[2]);
                const auto end = expression(*node.children[3]);
                auto& lengthInstruction = instruction(AIROpcode::SliceLength, &node, typed_.types.builtin("usize"), true);
                lengthInstruction.operands = {source}; const auto length = lengthInstruction.result;
                const auto reversed = binary(">", start, end, typed_.types.builtin("bool"), node);
                const auto slot = local(nullptr, type(node), "$slice.result");
                const auto reverseBlock = block("slice.reversed"), boundsBlock = block("slice.bounds"), badBlock = block("slice.outofbounds"), goodBlock = block("slice.ok"), done = block("slice.done");
                const auto failure = [&](std::string_view name)
                {
                    SymbolId selected = InvalidSymbolId;
                    for (const auto& [candidate, information] : typed_.enumVariants)
                        if (information.type == carrier.error && names_.symbols[candidate].name == name) selected = candidate;
                    if (selected == InvalidSymbolId) { error(node, "Missing BoundsError variant."); return; }
                    auto& reason = instruction(AIROpcode::EnumConstruct, &node, carrier.error, true); reason.callee = selected; const auto reasonValue = reason.result;
                    auto& bad = instruction(AIROpcode::EnumConstruct, &node, type(node), true); bad.callee = carrier.failure; bad.operands = {reasonValue}; const auto value = bad.result;
                    write(localPlace(slot), value, node); jump(done, &node);
                };
                branch(reversed, reverseBlock, boundsBlock, node); block_ = reverseBlock; failure("ReversedRange");
                block_ = boundsBlock;
                const auto within = binary("<=", end, length, typed_.types.builtin("bool"), node);
                branch(within, goodBlock, badBlock, node); block_ = badBlock; failure("OutOfBounds");
                block_ = goodBlock;
                auto& subview = instruction(AIROpcode::SliceSubview, &node, carrier.value, true); subview.operands = {source, start, end};
                const auto payload = subview.result;
                auto& good = instruction(AIROpcode::EnumConstruct, &node, type(node), true); good.callee = carrier.success; good.operands = {payload}; const auto value = good.result;
                write(localPlace(slot), value, node); jump(done, &node); block_ = done;
                return read(localPlace(slot), node, true);
            }
            AIRValueId numericExpression(const Node& node)
            {
                const auto& carrier = typed_.carriers.at(type(node));
                const auto source = expression(*node.children[1]);
                auto& statusInstruction = instruction(AIROpcode::NumericStatus, &node, typed_.types.builtin("i32"), true);
                statusInstruction.operands = {source}; statusInstruction.operation = node.value; statusInstruction.dynamicSignature = carrier.value;
                const auto status = statusInstruction.result;
                const auto zero = constant(typed_.types.builtin("i32"), "0", &node);
                const auto success = binary("==", status, zero, typed_.types.builtin("bool"), node);
                const auto slot = local(nullptr, type(node), "$numeric.result");
                const auto yes = block("numeric.ok"), no = block("numeric.error"), done = block("numeric.done");
                branch(success, yes, no, node); block_ = yes;
                auto& conversion = instruction(AIROpcode::NumericConvert, &node, carrier.value, true);
                conversion.operands = {source}; conversion.operation = node.value; const auto converted = conversion.result;
                auto& ok = instruction(AIROpcode::EnumConstruct, &node, type(node), true);
                ok.callee = carrier.success; ok.operands = {converted}; const auto result = ok.result;
                write(localPlace(slot), result, node); jump(done, &node); block_ = no;
                const auto fail = [&](std::string_view name)
                {
                    SymbolId selected = InvalidSymbolId;
                    for (const auto& [candidate, description] : typed_.enumVariants)
                        if (description.type == carrier.error && names_.symbols[candidate].name == name) selected = candidate;
                    if (selected == InvalidSymbolId) { error(node, "Missing numeric error variant."); return; }
                    auto& reason = instruction(AIROpcode::EnumConstruct, &node, carrier.error, true); reason.callee = selected; const auto errorValue = reason.result;
                    auto& bad = instruction(AIROpcode::EnumConstruct, &node, type(node), true); bad.callee = carrier.failure; bad.operands = {errorValue}; const auto errorResult = bad.result;
                    write(localPlace(slot), errorResult, node); jump(done, &node);
                };
                if (node.value == "Integer.convert") fail("OutOfRange");
                else
                {
                    const auto nonfinite = block("numeric.nonfinite"), remaining = block("numeric.remaining");
                    const auto two = constant(typed_.types.builtin("i32"), "2", &node);
                    const auto notFinite = binary("==", status, two, typed_.types.builtin("bool"), node);
                    branch(notFinite, nonfinite, remaining, node); block_ = nonfinite; fail("NonFinite"); block_ = remaining;
                    const auto nonexact = block("numeric.nonexact"), range = block("numeric.range");
                    const auto three = constant(typed_.types.builtin("i32"), "3", &node);
                    const auto notExact = binary("==", status, three, typed_.types.builtin("bool"), node);
                    branch(notExact, nonexact, range, node); block_ = nonexact; fail("NonExact"); block_ = range; fail("OutOfRange");
                }
                block_ = done; return read(localPlace(slot), node, true);
            }
            AIRValueId collectionIntrinsic(const Node& node, const std::string& operation)
            {
                if (operation == "range.with_step" || operation == "range.len") return rangeResult(node, operation == "range.len");
                if (operation == "slice.len")
                {
                    const auto operand = expression(*node.children[1]->children[0]);
                    auto& item = instruction(AIROpcode::SliceLength, &node, type(node), true);
                    item.operands = {operand}; return item.result;
                }
                std::vector<AIRValueId> operands;
                if (node.kind == "Binary" || node.kind == "Range")
                    for (const auto& bound : node.children) operands.push_back(expression(*bound));
                else for (std::size_t i = 1; i < node.children.size(); ++i) operands.push_back(expression(*node.children[i]->children[0]));
                auto& item = instruction(AIROpcode::RangeConstruct, &node, type(node), true);
                item.operation = operation; item.operands = std::move(operands); return item.result;
            }
            AIRValueId valueIntrinsic(const Node& node, const std::string& operation)
            {
                const auto input = [&](std::size_t index) -> const Node&
                { return node.kind == "CompilerTextExpression" ? *node.children[index] : *node.children[index]->children[0]; };
                if (operation == "byte.from_u8" || operation == "byte.to_u8")
                {
                    const auto source = expression(input(1));
                    auto& item = instruction(AIROpcode::Unary, &node, type(node), true);
                    item.operands = {source}; item.operation = operation; return item.result;
                }
                if (operation == "tuple.make")
                {
                    std::vector<AIRValueId> values;
                    std::vector<AIRLocalId> owners;
                    for (std::size_t index = 1; index < node.children.size(); ++index)
                    {
                        const auto& field = input(index); const auto value = expression(field);
                        if (!copyType(type(field)))
                        { const auto owner = local(nullptr, type(field), "$tuplefield"); write(localPlace(owner), value, field); owners.push_back(owner); values.push_back(InvalidAIRId); }
                        else { owners.push_back(InvalidAIRId); values.push_back(value); }
                    }
                    for (std::size_t index = 0; index < owners.size(); ++index) if (owners[index] != InvalidAIRId) values[index] = read(localPlace(owners[index]), node, true);
                    auto& item = instruction(AIROpcode::Aggregate, &node, type(node), true); item.operands = std::move(values); return item.result;
                }
                if (operation.starts_with("tuple.") || operation == "slice.view"
                    || ((operation == "text.view" || operation == "text.as_bytes") && typed_.types.get(type(input(1))).kind == TypeKind::Reference))
                {
                    const auto& source = input(1); AIRPlace address;
                    if (source.kind == "Prefix" && (source.value == "&" || source.value == "&mut")) address = place(*source.children[0]);
                    else { address.pointer = pointerExpression(source); address.type = typed_.types.get(type(source)).element; }
                    if (operation == "tuple.len" || operation == "tuple.is_empty")
                        return constant(type(node), operation == "tuple.len" ? std::to_string(typed_.types.get(address.type).parameters.size()) : typed_.types.get(address.type).parameters.empty() ? "true" : "false", &node);
                    if (operation == "tuple.copy") return read(address, node, false);
                    if (operation == "tuple.get" || operation == "tuple.get_mut")
                    {
                        const auto index = typed_.tupleIndices.at(&node);
                        const auto field = typed_.types.get(address.type).parameters[index];
                        address.projections.push_back({AIRProjectionKind::Field, index, InvalidAIRId, field, 0}); address.type = field;
                        auto& loan = instruction(operation == "tuple.get_mut" ? AIROpcode::BorrowMut : AIROpcode::BorrowShared, &node, type(node), true);
                        loan.place = address; return loan.result;
                    }
                    auto& loan = instruction(AIROpcode::SliceConstruct, &node, type(node), true); loan.place = address; loan.operation = operation; return loan.result;
                }
                std::vector<AIRValueId> values;
                for (std::size_t index = 1; index < node.children.size(); ++index) values.push_back(expression(input(index)));
                auto& item = instruction(AIROpcode::TextIntrinsic, &node, type(node), true); item.operands = std::move(values); item.operation = operation; return item.result;
            }
            AIRValueId aggregate(const Node& node)
            {
                const auto* fields = node.kind == "ArrayLiteral" ? nullptr : record(type(node));
                if (node.kind != "ArrayLiteral" && !fields) { error(node, "unknown aggregate record."); return InvalidAIRId; }
                const auto count = node.kind == "ArrayLiteral" ? node.children.size() : fields->fields.size();
                std::vector<AIRValueId> values(count, InvalidAIRId);
                std::vector<AIRLocalId> owners(count, InvalidAIRId);
                const auto retain = [&](std::size_t index, const Node& expressionNode, TypeId wanted)
                {
                    const auto value = expression(expressionNode);
                    if (!copyType(wanted))
                    {
                        const auto slot = local(nullptr, wanted, "$aggregatefield"); write(localPlace(slot), value, expressionNode); owners[index] = slot;
                    }
                    else values[index] = value;
                };
                if (node.kind == "ArrayLiteral")
                    for (std::size_t index = 0; index < node.children.size(); ++index) retain(index, *node.children[index], typed_.types.get(type(node)).element);
                else
                {
                    for (std::size_t index = 1; index < node.children.size(); ++index)
                        for (std::size_t slot = 0; slot < fields->fields.size(); ++slot)
                            if (names_.symbols[fields->fields[slot]].name == node.children[index]->value)
                                retain(slot, *node.children[index]->children[0], fields->fieldTypes[slot]);
                    for (std::size_t slot = 0; slot < values.size(); ++slot)
                        if (values[slot] == InvalidAIRId && owners[slot] == InvalidAIRId)
                            if (const auto* initial = initializer(*names_.symbols[fields->fields[slot]].declaration)) retain(slot, *initial, fields->fieldTypes[slot]);
                }
                for (std::size_t index = 0; index < owners.size(); ++index)
                    if (owners[index] != InvalidAIRId) values[index] = read(localPlace(owners[index]), node, true);
                auto& item = instruction(AIROpcode::Aggregate, &node, type(node), true); item.operands = std::move(values); return item.result;
            }
            AIRValueId enumConstructor(const Node& node, SymbolId variant)
            {
                const auto found = typed_.enumVariants.find(variant);
                if (found == typed_.enumVariants.end()) { error(node, "missing enum variant metadata."); return InvalidAIRId; }
                const auto& info = found->second;
                std::vector<AIRValueId> operands(info.fields.size(), InvalidAIRId);
                std::vector<AIRLocalId> owners(info.fields.size(), InvalidAIRId);
                if (node.kind == "NamedInitializer")
                    for (std::size_t index = 1; index < node.children.size(); ++index)
                        for (std::size_t field = 0; field < info.fields.size(); ++field)
                            if (names_.symbols[info.fields[field]].name == node.children[index]->value)
                            {
                                const auto value = expression(*node.children[index]->children[0]);
                                if (!copyType(info.fieldTypes[field]))
                                {
                                    const auto slot = local(nullptr, info.fieldTypes[field], "$variantfield"); write(localPlace(slot), value, *node.children[index]); owners[field] = slot;
                                }
                                else operands[field] = value;
                            }
                for (std::size_t index = 0; index < owners.size(); ++index)
                    if (owners[index] != InvalidAIRId) operands[index] = read(localPlace(owners[index]), node, true);
                auto& constructed = instruction(AIROpcode::EnumConstruct, &node, info.type, true);
                constructed.callee = variant; constructed.operands = std::move(operands); return constructed.result;
            }
            AIRValueId carrierPredicate(const Node& node, const CorePredicateInfo& predicate)
            {
                const auto& argument = *node.children[1]->children[0];
                AIRPlace carrier; carrier.pointer = pointerExpression(argument); carrier.type = predicate.carrier;
                auto& loaded = instruction(AIROpcode::EnumTag, &node, typed_.types.builtin("u32"), true);
                loaded.place = carrier; const auto tag = loaded.result;
                const auto& description = typed_.carriers.at(predicate.carrier);
                const auto variant = predicate.success ? description.success : description.failure;
                const auto expected = constant(typed_.types.builtin("u32"), std::to_string(typed_.enumVariants.at(variant).tag), &node);
                return binary("==", tag, expected, typed_.types.builtin("bool"), node);
            }
            AIRValueId extractCarrier(AIRPlace carrier, SymbolId variant, const Node& node)
            {
                const auto& info = typed_.enumVariants.at(variant);
                if (info.fields.empty()) return InvalidAIRId;
                carrier.projections.push_back({AIRProjectionKind::EnumField, 0, InvalidAIRId, info.fieldTypes[0], 0, variant});
                carrier.type = info.fieldTypes[0];
                const auto value = read(carrier, node, true);
                if (!copyType(carrier.type)) function_->blocks[block_].instructions.back().operation = "enum.extract";
                return value;
            }
            void retireCarrier(const AIRPlace& carrier, const Node& node)
            {
                // 활성 payload의 소유권을 옮긴 뒤 빈 운반 저장소의 정리 플래그를 해제한다.
                // Clear cleanup ownership of carrier storage after moving its only active payload.
                read(carrier, node, true);
                function_->blocks[block_].instructions.back().operation = "carrier.retire";
            }
            AIRValueId propagate(const Node& node, const PropagationInfo& propagation)
            {
                const auto owned = expression(*node.children[0]);
                const auto slot = local(nullptr, propagation.operand, "$enumselector");
                const auto carrier = localPlace(slot);
                write(carrier, owned, node);
                const auto& operand = typed_.carriers.at(propagation.operand);
                const auto& returned = typed_.carriers.at(propagation.result);
                auto& loaded = instruction(AIROpcode::EnumTag, &node, typed_.types.builtin("u32"), true);
                loaded.place = carrier; const auto tag = loaded.result;
                const auto successTag = constant(typed_.types.builtin("u32"), std::to_string(typed_.enumVariants.at(operand.success).tag), &node);
                const auto success = binary("==", tag, successTag, typed_.types.builtin("bool"), node);
                const auto yes = block("propagate.ok"); const auto no = block("propagate.error");
                branch(success, yes, no, node);
                block_ = no;
                const auto error = extractCarrier(carrier, operand.failure, node);
                retireCarrier(carrier, node);
                auto& constructed = instruction(AIROpcode::EnumConstruct, &node, propagation.result, true);
                constructed.callee = returned.failure;
                if (!operand.optional) constructed.operands = {error};
                const auto result = constructed.result;
                auto& end = function_->blocks[block_].terminator;
                end.kind = AIRTerminatorKind::Return; end.value = result; end.source = &node; end.range = node.range;
                end.exitedScopes.assign(scopes_.rbegin(), scopes_.rend());
                block_ = yes;
                const auto value = extractCarrier(carrier, operand.success, node);
                retireCarrier(carrier, node);
                return value;
            }
            AIRValueId collectionLiteral(const Node& node, TypeId collection, AIRBlockId failed)
            {
                const auto& target = typed_.types.get(collection);
                std::vector<const Node*> elements;
                for (const auto& item : node.children) if (item->kind != "CompilerArrayResult") elements.push_back(item.get());
                if (target.kind == TypeKind::Vector)
                {
                    auto& allocation = instruction(AIROpcode::VectorAllocate, &node, collection, true);
                    allocation.operation = std::to_string(elements.size()); const auto allocated = allocation.result;
                    const auto slot = local(nullptr, collection, "$vector.literal"); write(localPlace(slot), allocated, node);
                    const auto view = read(localPlace(slot), node, false);
                    auto& available = instruction(AIROpcode::VectorAvailable, &node, typed_.types.builtin("bool"), true);
                    available.operands = {view}; const auto valid = available.result;
                    const auto ready = block("vector.allocated"); branch(valid, ready, failed, node); block_ = ready;
                    for (const auto* element : elements)
                    {
                        const auto value = element->kind == "ArrayLiteral" ? collectionLiteral(*element, target.element, failed) : expression(*element);
                        auto& append = instruction(AIROpcode::VectorAppend, element, collection);
                        append.place = localPlace(slot); append.operands = {value};
                    }
                    return read(localPlace(slot), node, true);
                }
                std::vector<AIRLocalId> slots;
                for (const auto* element : elements)
                {
                    const auto value = element->kind == "ArrayLiteral" ? collectionLiteral(*element, target.element, failed) : expression(*element);
                    const auto slot = local(nullptr, target.element, "$literal.element"); write(localPlace(slot), value, *element); slots.push_back(slot);
                }
                std::vector<AIRValueId> values;
                for (const auto slot : slots) values.push_back(read(localPlace(slot), node, true));
                auto& aggregate = instruction(AIROpcode::Aggregate, &node, collection, true); aggregate.operands = std::move(values); return aggregate.result;
            }
            AIRValueId dynamicLiteral(const Node& node, TypeId resultType)
            {
                const auto resultSlot = local(nullptr, resultType, "$literal.result");
                const auto& carrier = typed_.carriers.at(resultType);
                enter(&node);
                const auto failed = block("literal.allocation.error"), done = block("literal.done");
                const auto value = collectionLiteral(node, carrier.value, failed);
                auto& success = instruction(AIROpcode::EnumConstruct, &node, resultType, true);
                success.callee = carrier.success; success.operands = {value}; const auto succeeded = success.result;
                write(localPlace(resultSlot), succeeded, node); jump(done, &node);
                block_ = failed;
                SymbolId errorVariant = InvalidSymbolId;
                for (const auto& [variant, info] : typed_.enumVariants)
                    if (info.type == carrier.error && names_.symbols[variant].name == "AllocationFailure") errorVariant = variant;
                if (errorVariant == InvalidSymbolId) { error(node, "CollectionError.AllocationFailure is missing."); return InvalidAIRId; }
                auto& allocationError = instruction(AIROpcode::EnumConstruct, &node, carrier.error, true);
                allocationError.callee = errorVariant; const auto failureValue = allocationError.result;
                auto& failure = instruction(AIROpcode::EnumConstruct, &node, resultType, true);
                failure.callee = carrier.failure; failure.operands = {failureValue}; const auto failedResult = failure.result;
                write(localPlace(resultSlot), failedResult, node); jump(done, &node);
                block_ = done; leave(&node);
                return read(localPlace(resultSlot), node, true);
            }
            AIRValueId vectorGrowth(const Node& node, bool push)
            {
                const auto receiver = expression(*node.children[1]->children[0]);
                const auto vector = typed_.types.get(type(*node.children[1]->children[0])).element;
                const auto input = expression(*node.children[2]->children[0]);
                const auto inputType = push ? typed_.types.get(vector).element : typed_.types.builtin("usize");
                const auto inputSlot = local(nullptr, inputType, "$vector.input"); write(localPlace(inputSlot), input, node);
                const auto additional = push ? constant(typed_.types.builtin("usize"), "1", &node) : read(localPlace(inputSlot), node);
                auto& reserve = instruction(AIROpcode::VectorReserve, &node, typed_.types.builtin("u32"), true);
                reserve.place.pointer = receiver; reserve.place.type = vector; reserve.operands = {additional}; const auto status = reserve.result;
                const auto zero = constant(typed_.types.builtin("u32"), "0", &node);
                const auto success = binary("==", status, zero, typed_.types.builtin("bool"), node);
                const auto resultSlot = local(nullptr, type(node), "$vector.result");
                const auto yes = block("vector.grow.ok"), no = block("vector.grow.failed"), size = block("vector.grow.overflow"), allocation = block("vector.grow.allocation"), done = block("vector.grow.done");
                const auto& carrier = typed_.carriers.at(type(node));
                branch(success, yes, no, node); block_ = yes;
                if (push)
                {
                    const auto value = read(localPlace(inputSlot), node, true);
                    auto& append = instruction(AIROpcode::VectorAppend, &node, vector); append.place.pointer = receiver; append.place.type = vector; append.operands = {value};
                }
                const auto unit = constant(typed_.types.builtin("unit"), "zeroinitializer", &node);
                auto& complete = instruction(AIROpcode::EnumConstruct, &node, type(node), true); complete.callee = carrier.success; complete.operands = {unit};
                const auto result = complete.result; write(localPlace(resultSlot), result, node); jump(done, &node);
                block_ = no;
                const auto overflow = constant(typed_.types.builtin("u32"), "2", &node);
                const auto tooLarge = binary("==", status, overflow, typed_.types.builtin("bool"), node); branch(tooLarge, size, allocation, node);
                const auto fail = [&](AIRBlockId blockId, std::string_view name)
                {
                    block_ = blockId; SymbolId variant = InvalidSymbolId;
                    for (const auto& [id, info] : typed_.enumVariants) if (info.type == carrier.error && names_.symbols[id].name == name) variant = id;
                    if (variant == InvalidSymbolId) { error(node, "Missing collection growth error variant."); return; }
                    std::vector<AIRValueId> values;
                    if (push) values.push_back(read(localPlace(inputSlot), node, true));
                    auto& errorValue = instruction(AIROpcode::EnumConstruct, &node, carrier.error, true); errorValue.callee = variant; errorValue.operands = std::move(values); const auto error = errorValue.result;
                    auto& returned = instruction(AIROpcode::EnumConstruct, &node, type(node), true); returned.callee = carrier.failure; returned.operands = {error}; const auto result = returned.result;
                    write(localPlace(resultSlot), result, node); jump(done, &node);
                };
                fail(size, "SizeOverflow"); fail(allocation, "AllocationFailure"); block_ = done;
                return read(localPlace(resultSlot), node, true);
            }
            AIRValueId expression(const Node& node)
            {
                if (terminated()) return InvalidAIRId;
                if (const auto found = typed_.dynamicLiterals.find(&node); found != typed_.dynamicLiterals.end()) return dynamicLiteral(node, found->second);
                if (typed_.collectionCalls.contains(&node))
                {
                    const auto& operation = typed_.collectionCalls.at(&node);
                    if (operation == "push" || operation == "reserve") return vectorGrowth(node, operation == "push");
                    const auto argument = expression(*node.children[1]->children[0]);
                    auto& length = instruction(AIROpcode::VectorLength, &node, type(node), true); length.operation = operation; length.operands = {argument}; return length.result;
                }
                if (const auto found = typed_.dynViews.find(&node); found != typed_.dynViews.end() && !loweringDynViews_.contains(&node))
                {
                    const auto& information = found->second;
                    const auto targetType = type(node);
                    AIRPlace address;
                    if (node.kind == "Prefix" && (node.value == "&" || node.value == "&mut")) address = place(*node.children[0]);
                    else
                    {
                        // 반환된 참조도 원래 ABI로 한 번 평가한 후 fat view로 변환한다.
                        // Evaluate returned references once with their original ABI before making the fat view.
                        originalDynTypes_[&node] = information.original;
                        loweringDynViews_.insert(&node);
                        const bool stored = node.kind == "Name" || node.kind == "Member" || node.kind == "Index" || node.kind == "Group";
                        const auto pointer = stored ? read(place(node), node, false) : expression(node);
                        originalDynTypes_.erase(&node); loweringDynViews_.erase(&node);
                        address.pointer = pointer; address.type = information.concreteType;
                    }
                    const bool exclusive = typed_.types.get(targetType).isMutable;
                    auto& loan = instruction(exclusive ? AIROpcode::BorrowMut : AIROpcode::BorrowShared, &node, targetType, true);
                    loan.place = address;
                    loan.operation = typed_.types.get(information.concreteType).kind == TypeKind::Dynamic ? "dyn.reborrow"
                        : "dyn.view." + std::to_string(information.interfaceType) + "." + std::to_string(information.concreteType);
                    return loan.result;
                }
                if (typed_.boxIntrinsics.contains(&node))
                {
                    const auto& input = *node.children[1];
                    if (node.value == "as_ref" || node.value == "as_mut")
                    {
                        AIRPlace address;
                        if (input.kind == "Prefix" && (input.value == "&" || input.value == "&mut")) address = place(*input.children[0]);
                        else { address.pointer = pointerExpression(input); address.type = typed_.types.get(type(input)).element; }
                        auto& item = instruction(node.value == "as_mut" ? AIROpcode::BorrowMut : AIROpcode::BorrowShared, &node, type(node), true);
                        item.place = address; item.operation = "box.borrow"; return item.result;
                    }
                    const auto inputValue = expression(input);
                    auto& item = instruction(AIROpcode::Unary, &node, type(node), true);
                    item.operands = {inputValue}; item.operation = "box." + node.value;
                    if (node.value == "into_dyn")
                    {
                        const auto& view = typed_.boxDynViews.at(&node);
                        item.operation += "." + std::to_string(view.interfaceType) + "." + std::to_string(view.concreteType);
                    }
                    return item.result;
                }
                if (const auto parallel = typed_.parallelCalls.find(&node); parallel != typed_.parallelCalls.end())
                {
                    const auto input = parallel->second == InvalidSymbolId ? InvalidAIRId : expression(*node.children[1]->children[0]);
                    auto& item = instruction(AIROpcode::ParallelIntrinsic, &node, type(node), true);
                    item.callee = parallel->second;
                    item.operation = parallel->second == InvalidSymbolId ? "worker_count" : "for_each";
                    if (input != InvalidAIRId) item.operands = {input};
                    return item.result;
                }
                if (const auto intrinsic = typed_.valueIntrinsics.find(&node); intrinsic != typed_.valueIntrinsics.end()) return valueIntrinsic(node, intrinsic->second);
                if (node.kind == "CompilerSliceExpression") return sliceExpression(node);
                if (node.kind == "CompilerNumericExpression") return numericExpression(node);
                if (typed_.unitValues.contains(&node)) return constant(typed_.types.builtin("unit"), "zeroinitializer", &node);
                if (const auto found = typed_.enumConstructors.find(&node); found != typed_.enumConstructors.end()) return enumConstructor(node, found->second);
                if (const auto found = typed_.predicates.find(&node); found != typed_.predicates.end()) return carrierPredicate(node, found->second);
                if (const auto found = typed_.propagations.find(&node); found != typed_.propagations.end()) return propagate(node, found->second);
                if (const auto found = typed_.intrinsics.find(&node); found != typed_.intrinsics.end())
                {
                    if (found->second.starts_with("simd."))
                    {
                        std::vector<AIRValueId> arguments;
                        for (std::size_t i = 1; i < node.children.size(); ++i)
                            arguments.push_back(expression(*node.children[i]->children[0]));
                        auto& operation = instruction(AIROpcode::Simd, &node, type(node), true);
                        operation.operation = found->second;
                        operation.dynamicSignature = typed_.simdTypes.at(&node);
                        operation.operands = std::move(arguments);
                        return operation.result;
                    }
                    if (found->second == "never.unreachable")
                    {
                        auto& end = function_->blocks[block_].terminator;
                        end.kind = AIRTerminatorKind::Trap;
                        end.source = &node;
                        end.range = node.range;
                        return InvalidAIRId;
                    }
                    return collectionIntrinsic(node, found->second);
                }
                if (node.kind == "Group") return expression(*node.children[0]);
                if (node.kind == "Literal") return constant(type(node), node.value, &node);
                if ((node.kind == "Name" || node.kind == "Member") && function_->initializesGlobal != InvalidSymbolId)
                {
                    const auto symbol = id(node);
                    if (!globalSymbols_.contains(symbol) && !locals_.contains(symbol) && symbol < names_.symbols.size())
                    {
                        const auto* declaration = names_.symbols[symbol].declaration;
                        if (declaration && flag(*declaration, "const"))
                        {
                            const auto evaluated = ConstEvaluator(names_, typed_.types, typed_.nodeTypes, typed_.symbolTypes).evaluate(node, type(node));
                            if (evaluated.status == ConstStatus::Constant)
                            {
                                const auto& value = evaluated.value;
                                if (value.kind == ConstantKind::Integer || value.kind == ConstantKind::Character)
                                    return constant(type(node), value.integerSpelling(), &node);
                                if (value.kind == ConstantKind::Boolean) return constant(type(node), value.boolean ? "true" : "false", &node);
                                if (value.kind == ConstantKind::Unit) return constant(type(node), "", &node);
                                if (value.kind == ConstantKind::Float)
                                {
                                    std::ostringstream spelling; spelling << std::setprecision(std::numeric_limits<long double>::max_digits10) << value.floating;
                                    return constant(type(node), spelling.str(), &node);
                                }
                            }
                        }
                    }
                }
                if (node.kind == "Name" || node.kind == "Member" || node.kind == "Index") return read(place(node), node);
                if (node.kind == "Call") return call(node);
                if (node.kind == "ArrayLiteral" || node.kind == "NamedInitializer") return aggregate(node);
                if (node.kind == "ArrayRepeat")
                {
                    const auto operand = expression(*node.children[0]);
                    auto& item = instruction(AIROpcode::ArrayRepeat, &node, type(node), true); item.operands = {operand}; return item.result;
                }
                if (node.kind == "Conditional") return condition(node, false);
                if (node.kind == "Binary")
                {
                    if (node.value == "&&" || node.value == "||") return condition(node, true);
                    const auto left = expression(*node.children[0]);
                    const auto right = expression(*node.children[1]);
                    return binary(node.value, left, right, type(node), node);
                }
                if (node.kind == "Prefix" || node.kind == "Postfix")
                {
                    if (node.value == "*") return read(place(node), node);
                    if (node.value == "&" || node.value == "&mut")
                    {
                        const auto address = place(*node.children[0]);
                        auto& item = instruction(node.value == "&mut" ? AIROpcode::BorrowMut : AIROpcode::BorrowShared, &node, type(node), true);
                        item.place = address;
                        return item.result;
                    }
                    if (node.value == "++" || node.value == "--")
                    {
                        const auto address = place(*node.children[0]);
                        const auto before = read(address, node, false);
                        const auto one = constant(type(node), "1", &node);
                        const auto after = binary(node.value == "++" ? "+" : "-", before, one, type(node), node);
                        write(address, after, node);
                        return node.kind == "Postfix" ? before : after;
                    }
                    if ((node.value == "+" || node.value == "-") && numericLiteralSyntax(node))
                    {
                        const auto folded = ConstEvaluator(names_, typed_.types, typed_.nodeTypes, typed_.symbolTypes).evaluate(node, type(node));
                        if (folded.status == ConstStatus::Constant && folded.value.kind == ConstantKind::Integer)
                            return constant(type(node), folded.value.integerSpelling(), &node);
                        if (folded.status == ConstStatus::Constant && folded.value.kind == ConstantKind::Float)
                        {
                            std::ostringstream spelling;
                            spelling << std::setprecision(std::numeric_limits<long double>::max_digits10) << folded.value.floating;
                            return constant(type(node), spelling.str(), &node);
                        }
                    }
                    const auto operand = expression(*node.children[0]);
                    auto& item = instruction(AIROpcode::Unary, &node, type(node), true);
                    item.operation = node.value;
                    item.operands = {operand};
                    return item.result;
                }
                error(node, "unsupported expression '" + node.kind + "'.");
                return constant(typed_.types.builtin("i32"), "0", &node);
            }
            void variable(const Node& node)
            {
                if (flag(node, "persist"))
                {
                    auto& ensure = instruction(AIROpcode::EnsurePersist, &node);
                    ensure.place = symbolPlace(id(node), type(node));
                    return;
                }
                const auto slot = local(&node, type(node));
                if (const auto* initial = initializer(node))
                {
                    const auto value = expression(*initial);
                    write(localPlace(slot), value, node);
                }
            }
            void assignment(const Node& node)
            {
                const auto address = place(*node.children[0]);
                AIRValueId value;
                if (node.value == "=") value = expression(*node.children[1]);
                else
                {
                    const auto left = read(address, node, false);
                    const auto right = expression(*node.children[1]);
                    value = binary(node.value.substr(0, node.value.size() - 1), left, right, address.type, node);
                }
                write(address, value, node);
            }
            void statement(const Node& node)
            {
                if (terminated()) return;
                if (node.kind == "Block" || node.kind == "CaseScope")
                {
                    enter(&node);
                    for (const auto& item : node.children) statement(*item);
                    leave(&node);
                }
                else if (node.kind == "Variable") variable(node);
                else if (node.kind == "Assignment") assignment(node);
                else if (node.kind == "ExpressionStatement")
                {
                    const auto& expressionNode = *node.children[0];
                    const auto result = expression(expressionNode);
                    if (result != InvalidAIRId && !copyType(type(expressionNode))
                        && typed_.types.get(type(expressionNode)).kind != TypeKind::Reference)
                    {
                        const auto temporary = local(nullptr, type(expressionNode), "$discarded");
                        const auto address = localPlace(temporary);
                        write(address, result, expressionNode);
                        auto& cleanup = instruction(AIROpcode::Drop, &expressionNode, address.type);
                        cleanup.place = address;
                        cleanup.operation = "temporary.cleanup";
                    }
                }
                else if (node.kind == "Return")
                {
                    const auto value = node.children.empty() ? InvalidAIRId : expression(*node.children[0]);
                    if (terminated()) return;
                    auto& end = function_->blocks[block_].terminator;
                    end.kind = AIRTerminatorKind::Return;
                    end.value = value;
                    end.source = &node;
                    end.range = node.range;
                    end.exitedScopes.assign(scopes_.rbegin(), scopes_.rend());
                }
                else if (node.kind == "If")
                {
                    const auto value = expression(*node.children[0]);
                    const auto yes = block("if.then");
                    const auto no = block("if.else");
                    const auto done = block("if.end");
                    branch(value, yes, no, node);
                    block_ = yes;
                    statement(*node.children[1]);
                    jump(done);
                    block_ = no;
                    if (node.children.size() > 2) statement(*node.children[2]);
                    jump(done);
                    block_ = done;
                }
                else if (node.kind == "While" || node.kind == "For")
                {
                    const auto outerDepth = scopes_.size();
                    enter(&node);
                    if (node.kind == "For") statement(*node.children[0]);
                    const auto test = block("loop.test");
                    const auto body = block("loop.body");
                    const auto step = block("loop.step");
                    const auto done = block("loop.end");
                    jump(test);
                    block_ = test;
                    const auto& testNode = *node.children[node.kind == "For" ? 1 : 0];
                    const auto value = testNode.kind == "Empty" ? constant(typed_.types.builtin("bool"), "true", &node) : expression(testNode);
                    branch(value, body, done, node);
                    block_ = body;
                    loops_.push_back({done, step, outerDepth + 1, outerDepth + 1});
                    statement(*node.children[node.kind == "For" ? 3 : 1]);
                    loops_.pop_back();
                    jump(step);
                    block_ = step;
                    if (node.kind == "For") statement(*node.children[2]);
                    jump(test);
                    block_ = done;
                    leave(&node);
                }
                else if (node.kind == "EnhancedFor") enhancedFor(node);
                else if (node.kind == "RangeFor") rangeFor(node);
                else if (node.kind == "Switch") switchStatement(node);
                else if (node.kind == "Break" || node.kind == "Continue")
                {
                    if (loops_.empty()) { error(node, "loop control has no target."); return; }
                    auto target = loops_.back();
                    if (node.kind == "Continue")
                    {
                        auto found = loops_.rbegin();
                        while (found != loops_.rend() && found->next == InvalidAIRId) ++found;
                        if (found == loops_.rend()) { error(node, "continue has no loop target."); return; }
                        target = *found;
                    }
                    jump(node.kind == "Break" ? target.stop : target.next, &node,
                         node.kind == "Break" ? target.breakDepth : target.continueDepth);
                }
                else if (node.kind != "Empty") error(node, "unsupported statement '" + node.kind + "'.");
            }
            void rangeFor(const Node& node)
            {
                const auto outerDepth = scopes_.size(); enter(&node);
                auto rangeType = type(*node.children[1]);
                auto source = expression(*node.children[1]);
                if (typed_.types.get(rangeType).kind == TypeKind::Reference)
                {
                    AIRPlace address; address.pointer = source; address.type = typed_.types.get(rangeType).element;
                    rangeType = address.type; source = read(address, node, false);
                }
                const auto cursor = local(nullptr, rangeType, "$range.cursor"); write(localPlace(cursor), source, node);
                const auto binding = local(node.children[0].get(), type(*node.children[0]));
                const auto test = block("range.test"), body = block("range.body"), step = block("range.step"), done = block("range.end");
                jump(test); block_ = test;
                const auto descriptor = read(localPlace(cursor), node, false);
                auto& available = instruction(AIROpcode::RangeTest, &node, typed_.types.builtin("bool"), true);
                available.operands = {descriptor}; const auto condition = available.result;
                branch(condition, body, done, node); block_ = body;
                auto& current = instruction(AIROpcode::RangeValue, &node, type(*node.children[0]), true);
                current.operands = {descriptor}; const auto element = current.result;
                write(localPlace(binding), element, node);
                loops_.push_back({done, step, outerDepth + 1, outerDepth + 1});
                statement(*node.children[2]); loops_.pop_back(); jump(step);
                block_ = step; const auto old = read(localPlace(cursor), node, false);
                auto& next = instruction(AIROpcode::RangeNext, &node, rangeType, true); next.operands = {old};
                const auto nextValue = next.result; write(localPlace(cursor), nextValue, node); jump(test);
                block_ = done; leave(&node);
            }
            void enhancedFor(const Node& node)
            {
                const auto outerDepth = scopes_.size();
                enter(&node);
                const auto& source = *node.children[1];
                const auto& sourceType = typed_.types.get(type(source));
                const auto baseType = sourceType.kind == TypeKind::Reference ? sourceType.element : type(source);
                const auto& collection = typed_.types.get(baseType);
                const bool text = collection.kind == TypeKind::String || collection.kind == TypeKind::StringView;
                const auto element = text ? typed_.types.builtin("char") : collection.element;
                if (!copyType(element)) { error(node, "iteration by value requires a Copy-compatible element."); leave(); return; }
                const auto viewType = typed_.iterationViews.at(&node);
                AIRValueId view;
                if (collection.kind == TypeKind::StringView || collection.kind == TypeKind::Slice)
                {
                    if (sourceType.kind == TypeKind::Reference)
                    { AIRPlace address; address.pointer = pointerExpression(source); address.type = baseType; view = read(address, source, false); }
                    else view = expression(source);
                }
                else
                {
                    AIRPlace address;
                    if (sourceType.kind == TypeKind::Reference) { address.pointer = pointerExpression(source); address.type = baseType; }
                    else address = place(source);
                    auto& loan = instruction(AIROpcode::SliceConstruct, &source, viewType, true); loan.place = address; view = loan.result;
                }
                const auto viewSlot = local(nullptr, viewType, "$iterationview"); write(localPlace(viewSlot), view, source);
                const auto indexType = typed_.types.builtin("usize");
                const auto index = local(nullptr, indexType, "$index");
                write(localPlace(index), constant(indexType, "0"), node);
                const auto binding = local(node.children[0].get(), type(*node.children[0]));
                const auto scalarSlot = text ? local(nullptr, element, "$iterationscalar") : InvalidAIRId;
                const auto test = block("foreach.test");
                const auto body = block("foreach.body");
                const auto step = block("foreach.step");
                const auto done = block("foreach.end");
                jump(test);
                block_ = test;
                const auto at = read(localPlace(index), node, false);
                const auto descriptor = read(localPlace(viewSlot), node, false);
                auto& length = instruction(text ? AIROpcode::TextIntrinsic : AIROpcode::SliceLength, &node, indexType, true);
                length.operands = {descriptor}; length.operation = "text.byte_len"; const auto size = length.result;
                const auto conditionValue = binary("<", at, size, typed_.types.builtin("bool"), node);
                branch(conditionValue, body, done, node);
                block_ = body;
                if (text)
                {
                    auto& decode = instruction(AIROpcode::TextIntrinsic, &node, element, true); decode.operation = "text.decode"; decode.operands = {descriptor, at};
                    const auto scalar = decode.result; write(localPlace(binding), scalar, node); write(localPlace(scalarSlot), scalar, node);
                }
                else
                {
                    auto address = localPlace(viewSlot); address.projections.push_back({AIRProjectionKind::Index, 0, at, element, 0}); address.type = element;
                    write(localPlace(binding), read(address, node, false), node);
                }
                loops_.push_back({done, step, outerDepth + 1, outerDepth + 1});
                statement(*node.children[2]);
                loops_.pop_back();
                jump(step);
                block_ = step;
                const auto previous = read(localPlace(index), node, false);
                AIRValueId next;
                if (text)
                {
                    const auto scalar = read(localPlace(scalarSlot), node, false), descriptor = read(localPlace(viewSlot), node, false);
                    auto& advance = instruction(AIROpcode::TextIntrinsic, &node, indexType, true); advance.operation = "text.next"; advance.operands = {descriptor, scalar, previous}; next = advance.result;
                }
                else next = binary("+", previous, constant(indexType, "1"), indexType, node);
                write(localPlace(index), next, node);
                jump(test);
                block_ = done;
                leave(&node);
            }
            void enumSwitch(const Node& node)
            {
                const auto outerDepth = scopes_.size();
                enter(&node);
                const auto& selectorNode = *node.children[0];
                auto selectorType = type(selectorNode);
                AIRPlace selector;
                if (node.value == "move")
                {
                    const auto owned = expression(selectorNode);
                    const auto slot = local(nullptr, selectorType, "$enumselector");
                    selector = localPlace(slot);
                    write(selector, owned, selectorNode);
                }
                else if (typed_.types.get(selectorType).kind == TypeKind::Reference)
                {
                    selector.pointer = pointerExpression(selectorNode);
                    selectorType = typed_.types.get(selectorType).element;
                    selector.type = selectorType;
                }
                else selector = place(selectorNode);
                auto& tagInstruction = instruction(AIROpcode::EnumTag, &selectorNode, typed_.types.builtin("u32"), true);
                tagInstruction.place = selector;
                const auto tag = tagInstruction.result;
                const auto done = block("enum.end");
                const auto invalid = block("enum.invalid");
                std::vector<AIRBlockId> cases;
                for (std::size_t index = 1; index < node.children.size(); ++index) cases.push_back(block("enum.case"));
                AIRBlockId fallback = invalid;
                for (std::size_t index = 1; index < node.children.size(); ++index)
                    if (node.children[index]->kind == "Default") fallback = cases[index - 1];
                for (std::size_t index = 1; index < node.children.size(); ++index)
                {
                    const auto& arm = *node.children[index];
                    const auto found = typed_.enumCases.find(&arm);
                    if (found == typed_.enumCases.end()) continue;
                    const auto& variant = typed_.enumVariants.at(found->second);
                    const auto label = constant(typed_.types.builtin("u32"), std::to_string(variant.tag), &arm);
                    const auto matches = binary("==", tag, label, typed_.types.builtin("bool"), arm);
                    const auto next = block("enum.test");
                    branch(matches, cases[index - 1], next, arm);
                    block_ = next;
                }
                jump(fallback);
                loops_.push_back({done, InvalidAIRId, outerDepth + 1, outerDepth + 1});
                for (std::size_t index = 1; index < node.children.size(); ++index)
                {
                    const auto& arm = *node.children[index];
                    block_ = cases[index - 1];
                    enter(&arm);
                    if (const auto* bindings = child(arm, "PatternBindings"))
                        for (const auto& binding : bindings->children)
                        {
                            const auto bound = typed_.enumBindings.find(binding.get());
                            if (bound == typed_.enumBindings.end()) continue;
                            const auto& info = typed_.enumVariants.at(bound->second.variant);
                            const auto found = std::find(info.fields.begin(), info.fields.end(), bound->second.field);
                            if (found == info.fields.end()) { error(*binding, "missing enum field metadata."); continue; }
                            const auto field = static_cast<std::size_t>(found - info.fields.begin());
                            AIRPlace payload = selector;
                            payload.projections.push_back({AIRProjectionKind::EnumField, field, InvalidAIRId, info.fieldTypes[field], 0, info.variant});
                            payload.type = info.fieldTypes[field];
                            const auto slot = local(binding.get(), type(*binding));
                            const auto& wanted = typed_.types.get(type(*binding));
                            AIRValueId value;
                            if (wanted.kind == TypeKind::Reference)
                            {
                                auto& loan = instruction(wanted.isMutable ? AIROpcode::BorrowMut : AIROpcode::BorrowShared, binding.get(), type(*binding), true);
                                loan.place = payload;
                                value = loan.result;
                            }
                            else
                            {
                                value = read(payload, *binding, bound->second.consuming);
                                if (bound->second.consuming && !copyType(payload.type)) function_->blocks[block_].instructions.back().operation = "enum.extract";
                            }
                            write(localPlace(slot), value, *binding);
                        }
                    if (const auto* scope = child(arm, "CaseScope"))
                        for (const auto& item : scope->children) statement(*item);
                    leave(&arm);
                    jump(done);
                }
                loops_.pop_back();
                block_ = done;
                leave(&node);
            }
            void switchStatement(const Node& node)
            {
                auto selected = type(*node.children[0]);
                if (typed_.types.get(selected).kind == TypeKind::Reference) selected = typed_.types.get(selected).element;
                if (const auto* description = record(selected); description && description->isEnum) { enumSwitch(node); return; }
                const auto selector = expression(*node.children[0]);
                const auto done = block("switch.end");
                std::vector<AIRBlockId> cases;
                for (std::size_t index = 1; index < node.children.size(); ++index) cases.push_back(block("switch.case"));
                AIRBlockId fallback = done;
                for (std::size_t index = 1; index < node.children.size(); ++index)
                    if (node.children[index]->kind == "Default") fallback = cases[index - 1];
                for (std::size_t index = 1; index < node.children.size(); ++index)
                {
                    const auto& arm = *node.children[index];
                    if (arm.kind != "Case") continue;
                    const auto label = expression(*arm.children[0]);
                    const auto matches = binary("==", selector, label, typed_.types.builtin("bool"), arm);
                    const auto next = block("switch.test");
                    branch(matches, cases[index - 1], next, arm);
                    block_ = next;
                }
                jump(fallback);
                loops_.push_back({done, InvalidAIRId, scopes_.size(), scopes_.size()});
                for (std::size_t index = 1; index < node.children.size(); ++index)
                {
                    block_ = cases[index - 1];
                    if (const auto* scope = child(*node.children[index], "CaseScope")) statement(*scope);
                    jump(done);
                }
                loops_.pop_back();
                block_ = done;
            }
            void lowerFunction(const Node& node)
            {
                locals_.clear(); scopes_.clear(); loops_.clear();
                block_ = block("entry");
                enter(&node);
                if (const auto* parameters = child(node, "Parameters"))
                    for (const auto& parameter : parameters->children)
                    {
                        const auto slot = local(parameter.get(), symbolType(id(*parameter)));
                        function_->locals[slot].isParameter = true;
                        function_->parameters.push_back(slot);
                    }
                if (const auto* numeric = child(node, "CompilerComplexMath"))
                {
                    std::vector<AIRValueId> arguments;
                    for (const auto parameter : function_->parameters) arguments.push_back(read(localPlace(parameter), node, false));
                    auto& operation = instruction(AIROpcode::ComplexIntrinsic, &node, function_->result, true);
                    operation.operation = numeric->value; operation.operands = std::move(arguments);
                    operation.dynamicSignature = function_->locals[function_->parameters[0]].type;
                    const auto result = operation.result;
                    instruction(AIROpcode::ScopeEnd, &node).scope = scopes_.back();
                    auto& end = function_->blocks[block_].terminator; end.kind = AIRTerminatorKind::Return;
                    end.value = result; end.source = &node; end.range = node.range;
                }
                else statement(*child(node, "Block"));
                if (!terminated() && typed_.types.get(function_->result).kind == TypeKind::Void)
                {
                    instruction(AIROpcode::ScopeEnd, &node).scope = scopes_.back();
                    function_->blocks[block_].terminator.kind = AIRTerminatorKind::Return;
                }
                scopes_.clear();
            }
        };
    }
    std::optional<AIRModule> lowerToAIR(const ModuleGraph& graph, const NameResolution& names,
                                      const TypeCheckResult& types, Diagnostics& diagnostics)
    {
        if (!types.succeeded || !names.succeeded || !diagnostics.empty()) return std::nullopt;
        return Lowerer(graph, names, types, diagnostics).run();
    }
}
