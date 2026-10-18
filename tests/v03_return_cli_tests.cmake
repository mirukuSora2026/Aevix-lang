# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_return_cli_tests.cmake
#
# Description :
#     명시적 반환의 bool 정수 적응과 일반 타입 호환성의 분리를 AOT·JIT로 검사한다.
#     Check return-only bool integer adaptation and unchanged general compatibility through AOT and JIT.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")

function(run_return name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${result}" STREQUAL "${expected}" OR
        (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${result}: ${output}${error}")
    endif()
endfunction()

set(program "")
set(assertions "")
foreach(type i8 u8 i16 u16 i32 u32 i64 u64 i128 u128 isize usize int)
    string(APPEND program "${type} yes_${type}() { return 3 > 2; }\n${type} no_${type}() { return 1 > 2; }\n")
    string(APPEND assertions "if (yes_${type}() != 1 || no_${type}() != 0) return 2;\n")
endforeach()
string(APPEND program [=[
type Small = i8;
type Wide = u128;
Small alias_small() { return true; }
Wide alias_wide() { return false; }
bool unchanged(bool value) { return value; }
u8 literal_context() { return 255; }
i64 wide_literal_context() { return 2147483648; }
i8 comparison_context() { return 1000 > 999; }
u8 nested_comparison_context() { return !(1000 < 999) && (false ? false : 1000 == 1000); }
i32 explicit_dereference(&bool value) { return *value; }
i32 increment_once(&mut i32 count) { *count += 1; return *count == 1; }
i32 fixed_result<T>(T value) { return true; }
T generic_outer<T>(T value) {
    rsn encode = [](bool input) -> i32 { return input; };
    encode(true);
    return value;
}
interface Check {
    public i16 check(&Self self);
    public i16 default_check(&Self self) { return true; }
};
struct Item implements Check {
    public i32 number;
    public i16 check(&Self self) { return self.number > 0; }
};
i32 main() {
]=])
string(APPEND program "${assertions}")
string(APPEND program [=[
    if (alias_small() != 1 || alias_wide() != 0) return 3;
    if (!unchanged(true) || unchanged(false)) return 4;
    if (literal_context() != 255 || wide_literal_context() != 2147483648) return 5;
    if (comparison_context() != 1 || nested_comparison_context() != 1) return 12;
    bool flag = true;
    if (explicit_dereference(&flag) != 1 || fixed_result(8) != 1 || generic_outer(8) != 8) return 6;
    mut i32 count = 0;
    if (increment_once(&mut count) != 1 || count != 1) return 7;
    Item item = Item { number = 1 };
    if (item.check() != 1 || item.default_check() != 1) return 8;
    &dyn Check view = &item;
    if (view.check() != 1 || view.default_check() != 1) return 9;
    rsn encode = [](bool value) -> u128 { return value; };
    if (encode(true) != 1 || encode(false) != 0) return 10;
    rsn compare = [](i32 left, i32 right) -> i32 { return left > right; };
    if (compare(3, 2) != 1 || compare(1, 2) != 0) return 11;
    return false;
}
]=])
file(WRITE "${TEST_DIR}/main.avx" "${program}")
run_return(check 0 "" --check "${TEST_DIR}/main.avx")
run_return(air 0 "numeric_convert.*return.bool_to_integer" --emit-air "${TEST_DIR}/main.avx")
run_return(llvm 0 "zext i1 .* to i128" --emit-llvm "${TEST_DIR}/main.avx")
run_return(aot 0 "" --run "${TEST_DIR}/main.avx")
run_return(aot_optimized 0 "" --run -O2 "${TEST_DIR}/main.avx")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
if(version MATCHES "ORC JIT enabled")
    run_return(jit 0 "" --jit "${TEST_DIR}/main.avx")
endif()
file(WRITE "${TEST_DIR}/true_main.avx" "i32 main() { return true; }\n")
run_return(true_exit_aot 1 "" --run "${TEST_DIR}/true_main.avx")
if(version MATCHES "ORC JIT enabled")
    run_return(true_exit_jit 1 "" --jit "${TEST_DIR}/true_main.avx")
endif()

function(invalid_return name text pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${text}\n")
    run_return("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()

# 반환 경계 외의 문맥과 컨테이너·참조에는 새로운 암시적 변환을 주지 않는다.
# Do not introduce implicit conversion outside the return boundary or into containers and references.
invalid_return(initialization "i32 main() { i32 value = true; return 0; }" "[Ii]nitializer")
invalid_return(assignment "i32 main() { mut i32 value = 0; value = true; return 0; }" "Assignment")
invalid_return(argument "i32 accept(i32 value) { return value; } i32 main() { return accept(true); }" "overload|argument|match")
invalid_return(overload "i32 choose(i32 value) { return value; } i32 choose(f64 value) { return 0; } i32 main() { return choose(true); }" "overload|argument|match")
invalid_return(arithmetic "i32 main() { return true + 1; }" "Binary|Operator")
invalid_return(condition "i32 main() { if (1) return 0; return 1; }" "Condition")
invalid_return(borrowed "i32 bad(&bool value) { return value; } i32 main() { return 0; }" "Return value")
invalid_return(float_result "f64 bad() { return true; } i32 main() { return 0; }" "Return value")
invalid_return(byte_result "byte bad() { return true; } i32 main() { return 0; }" "Return value")
invalid_return(char_result "char bad() { return true; } i32 main() { return 0; }" "Return value")
invalid_return(enum_result "enum Number { Zero; }; Number bad() { return true; } i32 main() { return 0; }" "Return value")
invalid_return(optional_result "Optional<i32> bad() { return true; } i32 main() { return 0; }" "Return value")
invalid_return(result_result "Result<i32, i32> bad() { return true; } i32 main() { return 0; }" "Return value")
invalid_return(simd_result "import simd { Simd }; Simd<i32, 4> bad() { return true; } i32 main() { return 0; }" "Return value")
invalid_return(void_result "void bad() { return true; } i32 main() { return 0; }" "void function")

# 우연히 bool로 구체화되어도 일반 T에 대한 계약이 유효해지지 않는다.
# A favorable bool specialization cannot make an invalid arbitrary-T contract legal.
invalid_return(generic_destination "T bad<T>() { return true; } i32 main() { return bad<i32>(); }" "Generic contract")
invalid_return(generic_source "i32 bad<T>(T value) { return value; } i32 main() { return bad(true); }" "Generic contract")
invalid_return(generic_source_unused "i32 bad<T>(T value) { return value; } i32 main() { return 0; }" "Generic contract")
invalid_return(generic_missing "T missing<T>() { return true; } i32 main() { return missing(); }" "Generic contract|infer")
message(STATUS "Return-only boolean integer adaptation passed")
