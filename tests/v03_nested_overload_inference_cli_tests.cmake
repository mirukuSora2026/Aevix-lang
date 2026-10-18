# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_nested_overload_inference_cli_tests.cmake
#
# Description :
#     중첩된 오버로드 반환형도 실제 호출과 같은 규칙으로 제네릭에 추론되는지 검사한다.
#     Check generic inference from nested overload returns using ordinary call ranking.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
function(nested_run name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(nested_accept name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    nested_run("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    nested_run("${name} AOT" 0 "" --run "${TEST_DIR}/${name}.avx")
    nested_run("${name} AOT O2" 0 "" --run -O2 "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        nested_run("${name} JIT" 0 "" --jit "${TEST_DIR}/${name}.avx")
        nested_run("${name} JIT O2" 0 "" --jit -O2 "${TEST_DIR}/${name}.avx")
    endif()
endfunction()
function(nested_reject name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    nested_run("reject ${name}" 1 "[Aa]mbiguous|infer|matching|requires|does not satisfy" --check "${TEST_DIR}/${name}.avx")
endfunction()

file(WRITE "${TEST_DIR}/foreign.avx" [=[
public enum Issue implements Copy { Failure; };
public Result<f32, Issue> choose(f32 value) { return Result.ok(value); }
public Result<f64, Issue> choose(f64 value) { return Result.ok(value); }
public Result<i8, Issue> narrow(i8 value) { return Result.ok(value); }
public Result<i32, Issue> narrow(i32 value) { return Result.ok(value); }
public i32 simple(i32 value) { return value + 10; }
public i64 simple(i64 value) { return value + 20; }
public i32 options(i32 value, i32 step = 1) { return value + step; }
public i64 options(i64 value, i64 step = 1) { return value + step; }
]=])

nested_accept(imported_results [=[
import foreign;
T success<T>(Result<T, foreign.Issue> result) {
    switch (move result) {
        case Result<T, foreign.Issue>.Ok { T found = value }: return found; break;
        case Result<T, foreign.Issue>.Err: Never.unreachable(); break;
    }
}
T identity<T>(T value) { return value; }
i32 main() {
    if (success(foreign.choose(4.0f32)) != 4.0f32) return 1;
    if (success<f64>(foreign.choose(4.0)) != 4.0) return 2;
    if (success<i8>(foreign.narrow(3i8)) != 3i8) return 3;
    if (success(foreign.narrow(3)) != 3) return 4;
    if (identity(foreign.simple(1)) != 11) return 5;
    if (identity(foreign.simple(1i64)) != 21i64) return 6;
    if (identity(foreign.options(step = 4, value = 3)) != 7) return 7;
    if (identity(foreign.options(3i64)) != 4i64) return 8;
    return 0;
}
]=])

nested_accept(ranking_and_methods [=[
T identity<T>(T value) { return value; }
i32 tied(i32 value) { return 3; }
T tied<T>(T value) { return value; }
i32 packed(i32 value) { return value + 1; }
i32 packed(i32 values[]) { return 30; }
struct Select {
    public i32 choose(&Self self, i32 value) { return value + 2; }
    public i64 choose(&Self self, i64 value) { return value + 3; }
};
i32 main() {
    Select selected = Select {};
    if (identity(tied(7)) != 3) return 1;
    if (identity(tied<i32>(7)) != 7) return 2;
    if (identity(packed(7)) != 8) return 3;
    if (identity(packed(7, 8)) != 30) return 4;
    if (identity(selected.choose(7)) != 9) return 5;
    if (identity<i64>(selected.choose(7i64)) != 10i64) return 6;
    return 0;
}
]=])

nested_accept(real_math_results [=[
import math;
T success<T>(Result<T, math.FloatMathError> result) {
    switch (move result) {
        case Result<T, math.FloatMathError>.Ok { T found = value }: return found; break;
        case Result<T, math.FloatMathError>.Err: Never.unreachable(); break;
    }
}
i32 main() {
    if (success(math.checked_sqrt(9.0f32)) != 3.0f32) return 1;
    if (success<f64>(math.checked_sqrt(4.0)) != 2.0) return 2;
    return 0;
}
]=])

nested_reject(ambiguous_literal [=[
T identity<T>(T value) { return value; }
i8 choose(i8 value) { return value; }
i64 choose(i64 value) { return value; }
i32 main() { rsn result = identity(choose(1)); return 0; }
]=])
nested_reject(default_count_tie [=[
T identity<T>(T value) { return value; }
i32 choose(i32 value) { return value; }
i32 choose(i32 value, i32 step = 1) { return value + step; }
i32 main() { return identity(choose(1)); }
]=])
nested_reject(incompatible_result [=[
import foreign;
T success<T>(Result<T, foreign.Issue> result) {
    switch (move result) {
        case Result<T, foreign.Issue>.Ok { T found = value }: return found; break;
        case Result<T, foreign.Issue>.Err: Never.unreachable(); break;
    }
}
i32 main() { rsn result = success<i32>(foreign.choose(4.0f32)); return 0; }
]=])

file(WRITE "${TEST_DIR}/other.avx" "public i32 simple(i32 value) { return value; }\n")
nested_reject(imports_do_not_merge [=[
import foreign { simple };
import other { simple };
T identity<T>(T value) { return value; }
i32 main() { return identity(simple(1)); }
]=])
message(STATUS "Nested overload generic inference passed")
