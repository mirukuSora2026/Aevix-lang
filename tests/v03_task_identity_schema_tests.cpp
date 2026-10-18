// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_task_identity_schema_tests.cpp
//
// Description :
//     정규 Task와 TaskId 선언의 타입 메타데이터를 변조해 봉인된 필드 형태를 검사한다.
//     Mutate canonical Task and TaskId declaration metadata to verify sealed field schemas.
//
// ========================
#include "aevix/Frontend/Modules/ModuleLoader.hpp"
#include "aevix/Frontend/Sema/CoreErrorTypes.hpp"
#include "aevix/Frontend/Sema/GenericResolver.hpp"
#include "aevix/Frontend/Sema/TypeChecker.hpp"
#include "aevix/Support/Process.hpp"
#include <cstdio>
#include <fstream>
#include <functional>
#include <string_view>

namespace
{
    using namespace aevix;
    unsigned checks=0, failures=0;
    Node* member(Node& node,std::string_view kind)
    { for (auto& value:node.children) if(value->kind==kind) return value.get(); return nullptr; }
    void expect(bool valid,const char* name) { ++checks; if(!valid){++failures;std::fprintf(stderr,"FAIL: %s\n",name);} }
    bool verify(const char* path,bool identity,const std::function<void(Node&,Node&)>& mutate)
    {
        SourceManager sources; Diagnostics diagnostics;
        auto graph=ModuleLoader(sources,diagnostics).load(path,{});
        if(!graph || !diagnostics.empty() || !installCoreErrorTypes(*graph,sources,diagnostics)) return false;
        auto names=NameResolver(diagnostics).resolve(*graph);
        if(!names.succeeded || !diagnostics.empty()) return false;
        auto generated=GenericResolver(diagnostics).resolve(*graph,names);
        if(!generated || !diagnostics.empty()) return false;
        names=std::move(*generated);
        unsigned touched=0;
        for(const auto& symbol:names.symbols)
        {
            if(!symbol.declaration || symbol.declaration->kind!="Struct") continue;
            auto& declaration=const_cast<Node&>(*symbol.declaration);
            if(identity ? !member(declaration,"CompilerTaskId") : !member(declaration,"CompilerTask") || member(declaration,"CompilerTemplate")) continue;
            auto* field=member(declaration,"Variable");
            if(!field) continue;
            mutate(declaration,*field);++touched;
        }
        if(!touched) return false;
        const auto typed=TypeChecker(diagnostics).check(*graph,names);
        return typed.succeeded && diagnostics.empty();
    }
}
int main()
{
    std::string error;
    auto directory=TemporaryDirectory::create("aevix-task-id-schema-",error);
    if(!directory) { std::fprintf(stderr,"%s\n",error.c_str()); return 2; }
    const auto input=directory->path()/"main.avx";
    { std::ofstream file(input); file << R"AVX(
import task { Task, TaskId, TaskScope, Executor, ScopeOptions };
T success<T,E>(Result<T,E> value) { switch (move value) { case Result<T,E>.Ok { T result = value }: return result; break; default: Never.unreachable(); break; } }
i32 main() {
    Executor executor = Executor.native_threads();
    TaskScope scope = success(Task.open_scope(&executor,ScopeOptions.defaults()));
    Task<i32> work = success(Task.spawn_owned(&scope,[]() -> i32 { return 42; }));
    TaskId identity = Task.id(&work);
    i32 result = success(Task.join(work));
    success(Task.close_scope(scope));
    return result;
}
)AVX"; }
    const auto path=utf8Path(input);
    expect(verify(path.c_str(),false,[](Node&,Node&){}),"canonical Task schema remains valid");
    expect(verify(path.c_str(),true,[](Node&,Node&){}),"canonical TaskId schema remains valid");
    for(const bool identity:{false,true})
    {
        const auto reject=[&](const char* name,const auto& mutation) { expect(!verify(path.c_str(),identity,mutation),name); };
        reject("private metadata field cannot be renamed",[](Node&,Node& field){field.value="native_token";});
        reject("private metadata field cannot be duplicated",[](Node& owner,Node&){
            auto added=makeNode("Variable","extra");added->children.push_back(makeNode("Type","u64"));added->children.push_back(makeNode("ArraySuffixes",""));owner.children.push_back(std::move(added));
        });
        reject("source field needs exact empty suffix metadata",[](Node&,Node& field){field.children.pop_back();});
        reject("source field cannot become an array",[](Node&,Node& field){field.children[1]->children.push_back(makeNode("ArraySuffix",""));});
        reject("source field cannot have initializer",[](Node&,Node& field){field.children.push_back(makeNode("Literal","0"));});
        reject("source field cannot become public",[](Node&,Node& field){field.children.push_back(makeNode("Modifier","public"));});
        reject("source field cannot become mutable",[](Node&,Node& field){field.children.push_back(makeNode("Modifier","mut"));});
        reject("source field cannot become persist",[](Node&,Node& field){field.children.push_back(makeNode("Modifier","persist"));});
        if(identity) {
            reject("identity source type must be canonical u64",[](Node&,Node& field){field.children[0]->value="usize";});
            reject("identity marker must be unique",[](Node& owner,Node&){owner.children.push_back(makeNode("CompilerTaskId",""));});
            reject("identity marker must not carry payload",[](Node& owner,Node&){member(owner,"CompilerTaskId")->value="token";});
        }
    }
    std::printf("%u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
