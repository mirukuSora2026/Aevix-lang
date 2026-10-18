# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_generic_owner_contract_cli_tests.cmake
#
# Description :
#     외부 제네릭 수신자의 타입 인자와 콜백 계약을 정확히 연결하는지 검사한다.
#     Check exact owner-argument substitution in cross-module generic callback contracts.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE compiler_version TIMEOUT 10)
file(WRITE "${TEST_DIR}/foreign.avx" [=[
public struct Map<K, V> {
    public bool choose<F>(&Self self, &K first, &K second, &F compare)
        where F implements Fn<Tuple<&K, &K>, bool> { return compare(first, second); }
};
]=])
set(valid_source [=[
import foreign;
struct Holder<A, B, F> where F implements Fn<Tuple<&A, &A>, bool> {
    public foreign.Map<A, B> owner;
    public F compare;
    public bool test(&Self self, &A first, &A second) { return self.owner.choose<F>(first, second, &self.compare); }
};
i32 exercise<F>(F compare) where F implements Fn<Tuple<&i32, &i32>, bool> {
    Holder<i32, i64, F> holder = Holder<i32, i64, F> { owner = foreign.Map<i32, i64> {}, compare = compare };
    i32 one = 1;
    return holder.test(&one, &one) ? 0 : 1;
}
i32 main() { return exercise([](&i32 first, &i32 second) -> bool { return *first == *second; }); }
]=])
function(owner_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
foreach(names renamed identical)
    set(source "${valid_source}")
    if(names STREQUAL "identical")
        string(REPLACE "<A, B, F>" "<K, V, F>" source "${source}")
        string(REPLACE "&A" "&K" source "${source}")
        string(REPLACE "<A, B>" "<K, V>" source "${source}")
    endif()
    file(WRITE "${TEST_DIR}/${names}.avx" "${source}")
    owner_run("${names} check" 0 "" --check "${TEST_DIR}/${names}.avx")
    owner_run("${names} AOT" 0 "" --run "${TEST_DIR}/${names}.avx")
    owner_run("${names} AOT O2" 0 "" --run -O2 "${TEST_DIR}/${names}.avx")
    if(compiler_version MATCHES "ORC JIT enabled")
        owner_run("${names} JIT" 0 "" --jit "${TEST_DIR}/${names}.avx")
        owner_run("${names} JIT O2" 0 "" --jit -O2 "${TEST_DIR}/${names}.avx")
    endif()
endforeach()
foreach(invalid key result unbounded)
    set(constraint "where F implements Fn<Tuple<&A, &A>, bool>")
    if(invalid STREQUAL "key")
        set(constraint "where F implements Fn<Tuple<&i64, &i64>, bool>")
    elseif(invalid STREQUAL "result")
        set(constraint "where F implements Fn<Tuple<&A, &A>, i32>")
    else()
        set(constraint "")
    endif()
    file(WRITE "${TEST_DIR}/bad_${invalid}.avx" "import foreign;\nstruct Holder<A, B, F> ${constraint} { public foreign.Map<A,B> owner; public F compare; public bool test(&Self self, &A x, &A y) { return self.owner.choose<F>(x, y, &self.compare); } };\ni32 main() { return 0; }\n")
    owner_run("reject ${invalid}" 1 "contract.*implements requirement|contract.*guarantee" --check "${TEST_DIR}/bad_${invalid}.avx")
endforeach()
message(STATUS "Cross-module owner callback constraints passed")
