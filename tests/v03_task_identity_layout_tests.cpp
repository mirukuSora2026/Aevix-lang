// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_task_identity_layout_tests.cpp
//
// Description :
//     두 64비트 대상에서 Task 진단 설명자와 중첩 소유자의 내부 배치를 검사한다.
//     Check Task diagnostic descriptors and nested owning layouts on both 64-bit targets.
//
// ========================
#include "aevix/Frontend/Sema/TypeLayout.hpp"
#include <cstdio>
namespace
{
    using namespace aevix;
    unsigned checks=0, failures=0;
    void expect(bool valid,const char* name) { ++checks; if(!valid){++failures;std::fprintf(stderr,"FAIL: %s\n",name);} }
}
int main()
{
    TypeContext types; NameResolution names; std::unordered_map<SymbolId,TypeId> symbols;
    const auto task=types.taskOwner(TypeKind::Task,999,"Task",types.builtin("i32"));
    const auto scope=types.taskOwner(TypeKind::TaskScope,998,"TaskScope");
    auto wrapper=makeNode("Struct","Wrapper");
    auto closure=makeNode("Struct","Closure");
    auto choice=makeNode("Enum","Choice"); auto variant=makeNode("Variant","Owned");
    const auto bind=[&](Node& node, const char* name, TypeId type) {
        auto field=makeNode("Variable",name); auto* address=field.get(); node.children.push_back(std::move(field));
        const auto id=names.symbols.size(); names.symbols.push_back({id,0,address,name,SymbolKind::Value,false}); symbols[id]=type;
    };
    const auto nominal=[&](Node& node) {
        const auto id=names.symbols.size(); names.symbols.push_back({id,0,&node,node.value,SymbolKind::Type,false});
        return types.nominal(id,node.value);
    };
    const auto wrapperType=nominal(*wrapper);
    bind(*wrapper,"before",types.builtin("bool")); bind(*wrapper,"owner",task); bind(*wrapper,"after",types.builtin("u8"));
    const auto closureType=nominal(*closure);
    bind(*closure,"owner",task); bind(*closure,"count",types.builtin("i32"));
    const auto choiceType=nominal(*choice); bind(*variant,"owner",task); choice->children.push_back(std::move(variant));
    for (const auto target:{LayoutTarget::AArch64,LayoutTarget::X86_64})
    {
        TypeLayout layout(types,names,symbols,target);
        const auto measure=[&](TypeId type,std::size_t size,std::size_t align,const char* label) {
            const auto got=layout.layout(type); expect(got.succeeded && got.layout.size==size && got.layout.alignment==align,label);
        };
        measure(task,16,8,"Task observation descriptor is 16 bytes aligned to 8");
        expect(layout.layout(task).layout.fields.empty(),"Task metadata does not become public physical fields");
        measure(scope,8,8,"TaskScope ABI remains one owner word");
        measure(types.tuple({types.builtin("bool"),task,types.builtin("i32")}),32,8,"Tuple retains both Task words");
        measure(types.array(task,3),48,8,"fixed Task array uses 16-byte element stride");
        measure(types.array(types.tuple({task,types.builtin("u64")}),2),48,8,"nested fixed tuple array uses complete descriptor stride");
        measure(types.vector(task),24,8,"Vector owner remains 24 bytes independent of element type");
        measure(types.array(task,0),0,8,"empty Task array preserves element alignment");
        measure(wrapperType,32,8,"nominal nested Task storage and tail padding");
        const auto aggregate=layout.layout(wrapperType);
        expect(aggregate.layout.fields.size()==3 && aggregate.layout.fields[1].offset==8 && aggregate.layout.fields[2].offset==24,
            "following struct field begins after complete Task descriptor");
        measure(closureType,24,8,"closure owning Task capture retains complete descriptor");
        measure(choiceType,24,8,"enum tagged Task payload includes identity word");
        const auto enumeration=layout.layout(choiceType);
        expect(enumeration.layout.fields.size()==1 && enumeration.layout.fields[0].offset==8,"enum Task payload aligned after tag");
    }
    std::printf("%u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
