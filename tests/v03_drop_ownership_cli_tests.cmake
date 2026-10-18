# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_drop_ownership_cli_tests.cmake
#
# Description :
#     사용자 소멸자의 보관 대여·암시적 호출 효과·persist 종료 제한과 부분 이동 금지를 검사한다.
#     Check retained destructor loans, implicit call effects, persist shutdown restrictions, and partial move rejection.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)

function(run_cleanup label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${result}" STREQUAL "${expected}" OR
        (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${result}: ${output}${error}")
    endif()
endfunction()

set(counter [=[
struct Counter implements Drop {
    public &mut i32 target;
    public void drop(&mut Counter self) { *self.target += 1; return; }
};
]=])

function(accept_cleanup name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${counter}\n${source}\n")
    run_cleanup("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    run_cleanup("${name} AOT" 0 "" --run "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        run_cleanup("${name} JIT" 0 "" --jit "${TEST_DIR}/${name}.avx")
    endif()
endfunction()

accept_cleanup(scope_release [=[
i32 main() {
    mut i32 count = 0;
    { Counter value = Counter { target = &mut count }; }
    return count == 1 ? 0 : 1;
}
]=])

accept_cleanup(move_and_callee [=[
void consume(Counter value) { return; }
i32 main() {
    mut i32 count = 0;
    {
        Counter first = Counter { target = &mut count };
        Counter second = first;
        consume(second);
    }
    return count == 1 ? 0 : 1;
}
]=])

accept_cleanup(nested_fields [=[
struct Holder { public Counter value; };
i32 main() {
    mut i32 count = 0;
    { Holder item = Holder { value = Counter { target = &mut count } }; }
    return count == 1 ? 0 : 1;
}
]=])

accept_cleanup(pure_persist_cleanup [=[
struct Pure implements Drop {
    public i32 value;
    public void drop(&mut Pure self) { self.value = 0; return; }
};
persist Pure global = Pure { value = 3 };
i32 main() { return global.value == 3 ? 0 : 1; }
]=])

function(reject_cleanup name source pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${counter}\n${source}\n")
    run_cleanup("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()

reject_cleanup(unused_owner_retains_loan [=[
i32 main() {
    mut i32 count = 0;
    Counter unused = Counter { target = &mut count };
    return count;
}
]=] "borrow|loan")

reject_cleanup(nested_owner_retains_loan [=[
struct Holder { public Counter value; };
i32 main() {
    mut i32 count = 0;
    Holder unused = Holder { value = Counter { target = &mut count } };
    count += 1;
    return 0;
}
]=] "borrow|loan")

reject_cleanup(array_owner_retains_loan [=[
i32 main() {
    mut i32 count = 0;
    Counter unused[1] = [Counter { target = &mut count }];
    count += 1;
    return 0;
}
]=] "borrow|loan")

reject_cleanup(borrowed_aggregate_retains_fields [=[
struct Referent { public &mut i32 target; };
struct Reader implements Drop {
    public &Referent source;
    public void drop(&mut Reader self) { *self.source.target += 1; return; }
};
i32 main() {
    mut i32 count = 0;
    Referent referent = Referent { target = &mut count };
    Reader unused = Reader { source = &referent };
    return count;
}
]=] "borrow|loan")

reject_cleanup(derived_reference_outlives_drop [=[
i32 main() {
    mut i32 count = 0;
    &i32 escaped;
    {
        Counter owner = Counter { target = &mut count };
        escaped = &*owner.target;
    }
    return *escaped;
}
]=] "destructor|destroy|borrow|loan")

reject_cleanup(by_value_parameter_reference [=[
&i32 expose(Counter owner) { return &*owner.target; }
i32 main() {
    mut i32 count = 0;
    Counter owner = Counter { target = &mut count };
    &i32 borrowed = expose(owner);
    return *borrowed;
}
]=] "destructor|destroy|borrow|loan")

set(global_effect [=[
mut i32 shared = 0;
void change() { shared += 1; return; }
struct GlobalWriter implements Drop {
    public void drop(&mut GlobalWriter self) { change(); return; }
};
]=])

reject_cleanup(implicit_global_effect "${global_effect}
i32 main() {
    &i32 borrowed = &shared;
    { GlobalWriter value = GlobalWriter {}; }
    return *borrowed;
}" "borrow|loan")

reject_cleanup(transitive_cleanup_effect "${global_effect}
void helper() { GlobalWriter value = GlobalWriter {}; return; }
i32 main() {
    &i32 borrowed = &shared;
    helper();
    return *borrowed;
}" "borrow|loan")

reject_cleanup(initializer_cleanup_effect "${global_effect}
i32 initialize() { GlobalWriter value = GlobalWriter {}; return 0; }
i32 read_local() { persist i32 value = initialize(); return value; }
i32 main() {
    &i32 borrowed = &shared;
    read_local();
    return *borrowed;
}" "borrow|loan")

reject_cleanup(persist_transitive_access "${global_effect}
persist GlobalWriter value = GlobalWriter {};
i32 main() { return 0; }" "persist destructor")

reject_cleanup(persist_nested_access "${global_effect}
struct Holder { public GlobalWriter value; };
persist Holder persisted = Holder { value = GlobalWriter {} };
i32 main() { return 0; }" "persist destructor")

reject_cleanup(persist_unknown_effect [=[
extern "C" void foreign_cleanup(i32 value);
struct External implements Drop {
    public i32 id;
    public void drop(&mut External self) { foreign_cleanup(self.id); return; }
};
persist External persisted = External { id = 0 };
i32 main() { return 0; }
]=] "persist destructor")

reject_cleanup(partial_consuming_enum [=[
struct Leaf {};
enum Owner implements Drop {
    Empty;
    Value { public Leaf leaf; };
    public void drop(&mut Owner self) { return; }
};
i32 main() {
    Owner owner = Owner.Value { leaf = Leaf {} };
    switch (move owner) {
        case Owner.Empty: return 0; break;
        case Owner.Value { Leaf extracted = leaf }: return 0; break;
    }
}
]=] "implementing Drop")

message(STATUS "Destructor ownership and implicit cleanup effects passed")
