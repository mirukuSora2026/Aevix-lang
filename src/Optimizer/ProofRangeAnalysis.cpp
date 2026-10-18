// ========================
//
// Created By : miruku_sora2026
//
// File name : ProofRangeAnalysis.cpp
//
// Description :
//     부호·크기 기반 끝점과 보수적 CFG 구간 전파로 검사 제거의 증거만 수집한다.
//     Collect check-elision evidence using sign-magnitude endpoints and conservative CFG interval propagation.
//
// ========================
#include "aevix/Optimizer/ProofRangeAnalysis.hpp"
#include "aevix/Common/UInt128.hpp"
#include "aevix/Frontend/Sema/NumericLiteral.hpp"
#include <algorithm>
#include <deque>
#include <iterator>
#include <string_view>
#include <optional>
#include <new>
#include <utility>
#include <vector>
namespace aevix {
namespace {
struct Bound {
    UInt128 magnitude;
    bool negative = false;
    Bound() = default;
    Bound(UInt128 value, bool sign = false) : magnitude(value), negative(sign && bool(value)) {}
    friend bool operator==(const Bound&, const Bound&) = default;
    friend bool operator<(const Bound& a, const Bound& b) {
        if (a.negative != b.negative) return a.negative;
        return a.negative ? a.magnitude > b.magnitude : a.magnitude < b.magnitude;
    }
};
bool le(const Bound& a, const Bound& b) { return !(b < a); }
Bound neg(const Bound& a) { return {a.magnitude, !a.negative}; }
std::optional<Bound> add(const Bound& a, const Bound& b) {
    if (a.negative == b.negative) {
        const auto value = a.magnitude + b.magnitude;
        if (value < a.magnitude) return {};
        return Bound{value, a.negative};
    }
    if (a.magnitude >= b.magnitude) return Bound{a.magnitude - b.magnitude, a.negative};
    return Bound{b.magnitude - a.magnitude, b.negative};
}
std::optional<Bound> mul(const Bound& a, const Bound& b) {
    UInt128 value;
    if (UInt128::multiply(a.magnitude, b.magnitude, value)) return {};
    return Bound{value, a.negative != b.negative};
}
struct Interval {
    Bound low, high;
    bool known = false;
    friend bool operator==(const Interval&, const Interval&) = default;
};
Interval exact(Bound value) { return {value, value, true}; }
Interval full(const Type& type) {
    if (type.kind != TypeKind::Integer || type.bits < 8 || type.bits > 64) return {};
    if (type.isSigned) return {{UInt128{1} << unsigned(type.bits - 1), true},
                              {(UInt128{1} << unsigned(type.bits - 1)) - UInt128{1}}, true};
    return {{}, {UInt128::mask(unsigned(type.bits))}, true};
}
bool fits(const Interval& range, const Type& type) {
    const auto limit = full(type);
    return range.known && limit.known && le(limit.low, range.low) && le(range.high, limit.high);
}
Interval arithmetic(const Interval& a, const Interval& b, std::string_view operation) {
    if (!a.known || !b.known) return {};
    if (operation == "+" || operation == "-") {
        auto low = add(a.low, operation == "+" ? b.low : neg(b.high));
        auto high = add(a.high, operation == "+" ? b.high : neg(b.low));
        if (low && high) return {*low, *high, true};
        return {};
    }
    if (operation == "*") {
        std::optional<Bound> endpoints[] = {mul(a.low,b.low),mul(a.low,b.high),mul(a.high,b.low),mul(a.high,b.high)};
        if (std::any_of(std::begin(endpoints),std::end(endpoints),[](const auto& value){return !value;})) return {};
        Bound low = *endpoints[0], high = low;
        for (const auto& value : endpoints) { if (*value < low) low = *value; if (high < *value) high = *value; }
        return {low,high,true};
    }
    return {};
}
std::optional<Bound> parse(std::string_view value) {
    value = numericSpelling(value);
    bool sign = false;
    if (!value.empty() && (value.front()=='-' || value.front()=='+')) { sign=value.front()=='-';value.remove_prefix(1); }
    unsigned base = 10;
    if (value.size()>2 && value.front()=='0') {
        if(value[1]=='x'||value[1]=='X')base=16;
        if(value[1]=='b'||value[1]=='B')base=2;
        if(value[1]=='o'||value[1]=='O')base=8;
        if(base!=10)value.remove_prefix(2);
    }
    UInt128 magnitude;
    if (!UInt128::parse(value,magnitude,base)) return {};
    return Bound{magnitude,sign};
}
struct State {
    bool reachable = false;
    std::vector<Interval> locals, values;
};
struct Origin { AIRLocalId local = InvalidAIRId; std::size_t epoch = 0; };
struct Predicate { std::string operation; AIRValueId left, right; Origin leftOrigin, rightOrigin; };
struct Transfer { State state; std::optional<Predicate> branch; std::vector<std::size_t> epochs; };
class Analysis {
    const AIRFunction& function_;
    const TypeContext& types_;
    const ProofRangeOptions& options_;
    ProofRangeResult result_;
    std::vector<bool> addressTaken_, widen_;
    std::vector<std::vector<AIRBlockId>> successors_;
    std::vector<State> incoming_;
    bool stopped_ = false;
    bool scalar(TypeId type) const { return full(types_.get(type)).known; }
    Interval typeRange(TypeId type) const { return full(types_.get(type)); }
    TypeId valueType(AIRValueId value) const { return value < function_.valueTypes.size() ? function_.valueTypes[value] : InvalidTypeId; }
    bool root(const AIRPlace& place) const {
        return place.local < function_.locals.size() && place.global==InvalidSymbolId
            && place.pointer==InvalidAIRId && place.projections.empty()
            && place.type==function_.locals[place.local].type;
    }
    bool plain(const AIRPlace& place) const { return root(place) && scalar(place.type) && !addressTaken_[place.local]; }
    void stop(std::string reason, bool budget=false) {
        stopped_=true;result_.arithmeticNoOverflow.clear();result_.fixedArrayIndices.clear();
        result_.stats.skipped=1;result_.stats.skipReason=std::move(reason);result_.stats.budgetFallback=budget;
        result_.stats.proven=0;result_.stats.arithmeticProven=0;result_.stats.fixedIndicesProven=0;
    }
    State empty() const {
        State state;
        for (const auto& local:function_.locals)state.locals.push_back(typeRange(local.type));
        for (const auto type:function_.valueTypes)state.values.push_back(typeRange(type));
        return state;
    }
    bool cleanupPure(TypeId id, unsigned depth=0) const {
        if(depth>32)return false;
        const auto& type=types_.get(id);
        switch(type.kind) {
            case TypeKind::Integer:case TypeKind::Float:case TypeKind::Bool:case TypeKind::Byte:
            case TypeKind::Char:case TypeKind::Unit:case TypeKind::Reference:case TypeKind::StringView:return true;
            case TypeKind::Array:return cleanupPure(type.element,depth+1);
            case TypeKind::Tuple:return std::all_of(type.parameters.begin(),type.parameters.end(),[&](auto item){return cleanupPure(item,depth+1);});
            default:return false;
        }
    }
    bool scopePure(AIRScopeId scope) const {
        if(scope>=function_.scopes.size())return false;
        for(auto local:function_.scopes[scope].locals)
            if(local>=function_.locals.size()||!cleanupPure(function_.locals[local].type))return false;
        return true;
    }
    bool validate() {
        const auto blocks=function_.blocks.size(), cells=function_.locals.size()+function_.valueTypes.size();
        if(!blocks){stop("no blocks");return false;}
        if(blocks>options_.maxBlocks || (cells && blocks>options_.maxStateCells/cells)) {stop("state budget",true);return false;}
        addressTaken_.assign(function_.locals.size(),false);successors_.resize(blocks);widen_.assign(blocks,false);
        std::vector<bool> defined(function_.valueTypes.size(),false);
        std::size_t instructions=0;
        for(std::size_t block=0;block<blocks;++block) {
            const auto& body=function_.blocks[block];
            for(const auto& item:body.instructions) {
                if(++instructions>options_.maxInstructionVisits){stop("instruction budget",true);return false;}
                if(item.result!=InvalidAIRId) {
                    if(item.result>=defined.size()||defined[item.result]||function_.valueTypes[item.result]!=item.type) {stop("invalid value definition");return false;}
                    defined[item.result]=true;
                }
                for(auto operand:item.operands)if(operand>=function_.valueTypes.size()){stop("invalid operand");return false;}
                for(const auto& projection:item.place.projections)
                    if(projection.kind==AIRProjectionKind::Index && projection.index>=function_.valueTypes.size()){stop("invalid index");return false;}
                if((item.opcode==AIROpcode::BorrowShared||item.opcode==AIROpcode::BorrowMut)&&item.place.local<addressTaken_.size())addressTaken_[item.place.local]=true;
            }
            const auto& end=body.terminator;
            if(end.kind==AIRTerminatorKind::Jump||end.kind==AIRTerminatorKind::Branch) {
                if(end.target>=blocks||(end.kind==AIRTerminatorKind::Branch&&end.alternative>=blocks)){stop("invalid CFG target");return false;}
                successors_[block].push_back(end.target);
                if(end.kind==AIRTerminatorKind::Branch) {
                    if(end.value>=function_.valueTypes.size()||types_.get(valueType(end.value)).kind!=TypeKind::Bool){stop("invalid branch");return false;}
                    successors_[block].push_back(end.alternative);
                }
            }
        }
        // 소스의 반복문 모양을 가정하지 않고 DFS 되돌림 간선의 도착점을 확장 지점으로 사용한다.
        // DFS back-edge destinations suffice as widening points without source-loop assumptions.
        std::vector<unsigned char> color(blocks,0);
        std::vector<std::pair<std::size_t,std::size_t>> stack{{0,0}};color[0]=1;
        while(!stack.empty()) {
            auto& frame=stack.back();
            if(frame.second==successors_[frame.first].size()){color[frame.first]=2;stack.pop_back();continue;}
            const auto next=successors_[frame.first][frame.second++];
            if(color[next]==1)widen_[next]=true;
            if(color[next]==0){color[next]=1;stack.push_back({next,0});}
        }
        return true;
    }
    void kill(Transfer& output) const {
        for(std::size_t i=0;i<output.state.locals.size();++i){output.state.locals[i]=typeRange(function_.locals[i].type);++output.epochs[i];}
    }
    void indices(const AIRInstruction& item,const State& state) {
        for(std::size_t n=0;n<item.place.projections.size();++n) {
            const auto& index=item.place.projections[n];
            if(index.kind!=AIRProjectionKind::Index)continue;
            ++result_.stats.considered;++result_.stats.fixedIndicesConsidered;
            const auto& place=item.place;
            if(place.projections.size()!=1||place.local>=function_.locals.size()||place.global!=InvalidSymbolId||place.pointer!=InvalidAIRId)continue;
            const auto& array=types_.get(function_.locals[place.local].type);
            if(array.kind!=TypeKind::Array||!array.extent||index.extent!=array.extent||index.type!=array.element||place.type!=array.element)continue;
            const auto& value=state.values[index.index];
            if(value.known&&!value.low.negative&&value.high<Bound{UInt128{static_cast<std::uint64_t>(array.extent)}}) {
                result_.fixedArrayIndices[&item].insert(n);++result_.stats.proven;++result_.stats.fixedIndicesProven;
            }
        }
    }
    Transfer transfer(std::size_t block,const State& input,bool collect) {
        Transfer output{input,{},std::vector<std::size_t>(function_.locals.size(),0)};
        std::vector<Origin> origins(function_.valueTypes.size());
        std::unordered_map<AIRValueId,Predicate> predicates;
        for(const auto& item:function_.blocks[block].instructions) {
            if(++result_.stats.instructionVisits>options_.maxInstructionVisits){stop("instruction visit budget",true);return output;}
            if(collect)indices(item,output.state);
            Interval range=typeRange(item.type);
            const bool produces=item.result<output.state.values.size();
            switch(item.opcode) {
                case AIROpcode::Constant: {
                    if(auto value=parse(item.operation);value&&fits(exact(*value),types_.get(item.type)))range=exact(*value);
                    break;
                }
                case AIROpcode::Read:case AIROpcode::Move:
                    if(plain(item.place)&&item.type==item.place.type) {
                        range=output.state.locals[item.place.local];
                        if(produces)origins[item.result]={item.place.local,output.epochs[item.place.local]};
                        if(item.opcode==AIROpcode::Move){output.state.locals[item.place.local]=typeRange(item.place.type);++output.epochs[item.place.local];}
                    } else if(item.place.pointer!=InvalidAIRId||item.place.global!=InvalidSymbolId)kill(output);
                    break;
                case AIROpcode::Write:
                    if(plain(item.place)&&item.operands.size()==1&&valueType(item.operands[0])==item.place.type) {
                        output.state.locals[item.place.local]=output.state.values[item.operands[0]];++output.epochs[item.place.local];
                    } else if(item.place.pointer!=InvalidAIRId||item.place.global!=InvalidSymbolId||!cleanupPure(item.place.type))kill(output);
                    break;
                case AIROpcode::Unary:
                    if(item.operands.size()==1&&valueType(item.operands[0])==item.type) {
                        const auto& operand=output.state.values[item.operands[0]];
                        if(item.operation=="+" )range=operand;
                        if(item.operation=="-"&&operand.known) {Interval candidate{neg(operand.high),neg(operand.low),true};if(fits(candidate,types_.get(item.type)))range=candidate;}
                    }
                    break;
                case AIROpcode::Binary:
                    if(item.operands.size()==2) {
                        const auto left=item.operands[0],right=item.operands[1];
                        const auto& a=output.state.values[left];const auto& b=output.state.values[right];
                        const bool same=valueType(left)==valueType(right);
                        if(same&&item.type==valueType(left)&&(item.operation=="+"||item.operation=="-"||item.operation=="*")) {
                            if(collect){++result_.stats.considered;++result_.stats.arithmeticConsidered;}
                            const auto candidate=arithmetic(a,b,item.operation);
                            if(fits(candidate,types_.get(item.type))) {
                                range=candidate;
                                if(collect){result_.arithmeticNoOverflow.insert(&item);++result_.stats.proven;++result_.stats.arithmeticProven;}
                            }
                        } else if(same&&item.type==valueType(left)&&item.operation=="%"&&!types_.get(item.type).isSigned&&a.known&&b.known&&b.low==b.high&&!b.low.negative&&bool(b.low.magnitude)) {
                            const Bound limit{b.low.magnitude-UInt128{1}};range={{},a.high<limit?a.high:limit,true};
                        } else if(same&&scalar(valueType(left))&&types_.get(item.type).kind==TypeKind::Bool&&produces) {
                            if(item.operation=="<"||item.operation=="<="||item.operation==">"||item.operation==">="||item.operation=="=="||item.operation=="!=")
                                predicates[item.result]={item.operation,left,right,origins[left],origins[right]};
                        }
                    }
                    break;
                case AIROpcode::ScopeBegin:break;
                case AIROpcode::ScopeEnd:if(!scopePure(item.scope))kill(output);break;
                case AIROpcode::Drop:if(!cleanupPure(item.place.type))kill(output);break;
                case AIROpcode::BorrowShared:case AIROpcode::BorrowMut:break;
                default:kill(output);break;
            }
            if(produces)output.state.values[item.result]=range;
        }
        const auto& end=function_.blocks[block].terminator;
        // 분기 전에 끝나는 스코프의 사용자 정리도 현재 지역 값에 영향을 줄 수 있다.
        // Cleanup on an outgoing edge may affect current local values before branch refinement.
        for(const auto scope:end.exitedScopes)if(!scopePure(scope))kill(output);
        if(end.kind==AIRTerminatorKind::Branch)
            if(auto found=predicates.find(end.value);found!=predicates.end())output.branch=found->second;
        return output;
    }
    void constrain(State& state,AIRValueId value,Origin origin,const Bound& low,const Bound& high,const std::vector<std::size_t>& epochs) const {
        auto& interval=state.values[value];
        if(!interval.known)return;
        if(interval.low<low)interval.low=low;if(high<interval.high)interval.high=high;
        if(interval.high<interval.low){state.reachable=false;return;}
        if(origin.local<state.locals.size()&&origin.epoch==epochs[origin.local]) {
            auto& local=state.locals[origin.local];
            if(local.known){if(local.low<interval.low)local.low=interval.low;if(interval.high<local.high)local.high=interval.high;if(local.high<local.low)state.reachable=false;}
        }
    }
    State edge(const Transfer& output,bool truth) const {
        State state=output.state;
        if(!output.branch)return state;
        auto predicate=*output.branch;
        if(!truth) {
            if(predicate.operation=="<")predicate.operation=">=";else if(predicate.operation=="<=")predicate.operation=">";
            else if(predicate.operation==">")predicate.operation="<=";else if(predicate.operation==">=")predicate.operation="<";
            else if(predicate.operation=="==")predicate.operation="!=";else predicate.operation="==";
        }
        if(predicate.operation==">") {std::swap(predicate.left,predicate.right);std::swap(predicate.leftOrigin,predicate.rightOrigin);predicate.operation="<";}
        if(predicate.operation==">=") {std::swap(predicate.left,predicate.right);std::swap(predicate.leftOrigin,predicate.rightOrigin);predicate.operation="<=";}
        const auto a=state.values[predicate.left],b=state.values[predicate.right];
        if(!a.known||!b.known)return state;
        if(predicate.operation=="<"||predicate.operation=="<=") {
            auto upper=predicate.operation=="<"?add(b.high,Bound{UInt128{1},true}):std::optional<Bound>{b.high};
            auto lower=predicate.operation=="<"?add(a.low,Bound{UInt128{1}}):std::optional<Bound>{a.low};
            if(upper&&lower){constrain(state,predicate.left,predicate.leftOrigin,a.low,*upper,output.epochs);constrain(state,predicate.right,predicate.rightOrigin,*lower,b.high,output.epochs);}
        } else if(predicate.operation=="==") {
            const auto low=a.low<b.low?b.low:a.low,high=a.high<b.high?a.high:b.high;
            constrain(state,predicate.left,predicate.leftOrigin,low,high,output.epochs);constrain(state,predicate.right,predicate.rightOrigin,low,high,output.epochs);
        } else if(a.low==a.high&&b.low==b.high&&a.low==b.low)state.reachable=false;
        return state;
    }
    bool join(std::size_t block,const State& source) {
        if(!source.reachable)return false;
        auto& target=incoming_[block];
        if(!target.reachable){target=source;return true;}
        bool changed=false;
        const auto combine=[&](std::vector<Interval>& to,const std::vector<Interval>& from,bool locals) {
            for(std::size_t i=0;i<to.size();++i) {
                if(!to[i].known||!from[i].known) {
                    const auto unknown=typeRange(locals?function_.locals[i].type:function_.valueTypes[i]);
                    if(to[i]!=unknown){to[i]=unknown;changed=true;}continue;
                }
                auto next=to[i];const auto limit=typeRange(locals?function_.locals[i].type:function_.valueTypes[i]);
                if(from[i].low<next.low)next.low=widen_[block]?limit.low:from[i].low;
                if(next.high<from[i].high)next.high=widen_[block]?limit.high:from[i].high;
                if(next!=to[i]){to[i]=next;changed=true;}
            }
        };
        combine(target.locals,source.locals,true);combine(target.values,source.values,false);return changed;
    }
public:
    Analysis(const AIRFunction& function,const TypeContext& types,const ProofRangeOptions& options):function_(function),types_(types),options_(options){}
    void solve() {
        result_.stats.functions=1;if(!validate())return;
        incoming_.assign(function_.blocks.size(),empty());incoming_[0].reachable=true;
        std::deque<std::size_t> pending{0};std::vector<bool> queued(function_.blocks.size(),false);queued[0]=true;
        while(!pending.empty()&&!stopped_) {
            if(++result_.stats.cfgIterations>options_.maxCFGIterations){stop("CFG iteration budget",true);break;}
            const auto block=pending.front();pending.pop_front();queued[block]=false;
            auto output=transfer(block,incoming_[block],false);if(stopped_)break;
            const auto& end=function_.blocks[block].terminator;
            for(std::size_t i=0;i<successors_[block].size();++i) {
                const auto next=successors_[block][i];
                const auto state=end.kind==AIRTerminatorKind::Branch?edge(output,i==0):output.state;
                if(join(next,state)&&!queued[next]){queued[next]=true;pending.push_back(next);}
            }
        }
        // 중간의 좁은 구간을 믿지 않고 수렴한 입력 상태로만 증거를 수집한다.
        // Never retain optimistic interim proofs; collect only in a second pass after convergence.
        if(!stopped_)for(std::size_t block=0;block<incoming_.size();++block)
            if(incoming_[block].reachable){transfer(block,incoming_[block],true);if(stopped_)break;}
    }
    ProofRangeResult run() {
        try { solve(); }
        catch(const std::bad_alloc&) {
            // 이미 생성된 결과 저장소를 비워서 오류 처리 중 새 컨테이너나 메시지를 만들지 않는다.
            // Reuse the existing result storage without constructing a diagnostic or fallback container.
            result_.arithmeticNoOverflow.clear();result_.fixedArrayIndices.clear();
            result_.stats.functions=1;result_.stats.skipped=1;result_.stats.budgetFallback=true;
            result_.stats.proven=0;result_.stats.arithmeticProven=0;result_.stats.fixedIndicesProven=0;
            result_.stats.skipReason.clear();
        }
        return std::move(result_);
    }
};
}
ProofRangeResult analyzeProofRanges(const AIRFunction& function,const TypeContext& types,const ProofRangeOptions& options) {
    return Analysis(function,types,options).run();
}
}
