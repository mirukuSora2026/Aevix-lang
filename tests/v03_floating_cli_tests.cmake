# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_floating_cli_tests.cmake
#
# Description :
#     순수 부동소수 라이브러리의 정확한 비트 정책과 독립 전체 범위 기준값을 검사한다.
#     Check exact-bit policies of the pure floating library against independent exhaustive and rational goldens.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
find_program(PYTHON_EXECUTABLE NAMES python3 REQUIRED)
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version TIMEOUT 10)
file(SHA256 "${AEVIXC}" original_compiler)
function(floating_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 180)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(floating_modes name)
    floating_run("${name} check" 0 "ownership checked" --check "${TEST_DIR}/${name}.avx")
    foreach(optimization -O0 -O2)
        floating_run("${name} AOT ${optimization}" 0 "" --run "${optimization}" "${TEST_DIR}/${name}.avx")
        if(version MATCHES "ORC JIT enabled")
            floating_run("${name} JIT ${optimization}" 0 "" --jit "${optimization}" "${TEST_DIR}/${name}.avx")
        endif()
    endforeach()
endfunction()
execute_process(COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_floating_golden.py" --output-dir "${TEST_DIR}"
    RESULT_VARIABLE generated OUTPUT_VARIABLE summary ERROR_VARIABLE error TIMEOUT 60)
if(NOT "${generated}" STREQUAL "0")
    message(FATAL_ERROR "Independent floating oracle failed: ${error}")
endif()
message(STATUS "${summary}")
file(WRITE "${TEST_DIR}/canonical_class.avx" [=[
import floating;
import math;
import numeric;
i32 main() {
    math.FloatClass original = floating.classify(1.0f16);
    numeric.FloatClass shared = original;
    floating.FloatClass exposed = shared;
    if (Float.to_bits(math.signum(-0.0f32)) != 2147483648u32 || Float.to_bits(math.signum(0.0f32)) != 0u32) return 2;
    if (Float.to_bits(math.signum(-0.0f64)) != 9223372036854775808u64 || Float.to_bits(math.signum(0.0f64)) != 0u64) return 3;
    if (Float.to_bits(math.signum(Float.from_bits<f32>(4286578695u32))) != 4286578695u32) return 4;
    if (Float.to_bits(math.signum(Float.from_bits<f64>(18442240474082181127u64))) != 18442240474082181127u64) return 5;
    if (math.signum(-3.0f32) != -1.0f32 || math.signum(3.0f64) != 1.0f64) return 6;
    switch(exposed) {case floating.FloatClass.Normal: return 0; break; default: return 1; break;}
}
]=])
file(WRITE "${TEST_DIR}/purity.avx" [=[
import floating;
import ordering;
i32 main() {
    f32 negative_zero = Float.from_bits<f32>(2147483648u32);
    f32 nan = Float.from_bits<f32>(4286578695u32);
    f64 one = Float.from_bits<f64>(4607182418800017408u64);
    return Float.to_bits(floating.abs(nan)) == 2139095047u32 && Float.to_bits(floating.signum(negative_zero)) == 2147483648u32 && Float.to_bits(floating.next_up(one)) == 4607182418800017409u64 && ordering.is_lt(floating.total_cmp(negative_zero,0.0f32)) ? 0 : 1;
}
]=])
file(WRITE "${TEST_DIR}/worker.avx" [=[
import floating;
import ordering;
import task { Task, TaskScope, Executor, ScopeOptions };
T success<T,E>(Result<T,E> input) {switch(move input){case Result<T,E>.Ok {T value=value}:return value;break;default:Never.unreachable();break;}}
i32 main() {
    Executor executor = Executor.native_threads();
    TaskScope scope = success(Task.open_scope(&executor,ScopeOptions.defaults()));
    u64 raw = 9223372036854775808u64;
    Task<u64> work = success(Task.spawn_owned(&scope,[raw]() -> u64 {
        f64 zero = Float.from_bits<f64>(raw);
        f64 nan = Float.from_bits<f64>(18442240474082181127u64);
        if (!floating.is_nan(nan) || !ordering.is_lt(floating.total_cmp(zero,0.0f64))) return 0u64;
        return Float.to_bits(floating.next_up(zero));
    }));
    u64 found = success(Task.join(work));
    success(Task.close_scope(scope));
    return found == 1u64 ? 0 : 1;
}
]=])
foreach(name exhaustive golden constants canonical_class purity worker)
    floating_modes("${name}")
endforeach()
# 모듈 상수 추론은 실제 선언만 사용하고 비공개·간접 의존 경로를 공개하지 않는다.
# Module-constant inference uses actual declarations without exposing private or transitive-only paths.
set(declarations "${TEST_DIR}/declarations")
file(MAKE_DIRECTORY "${declarations}")
file(WRITE "${declarations}/float_values.avx" "public const f16 HALF=1.0f16;public const f64 DOUBLE=2.0f64;private const f32 HIDDEN=3.0f32;\n")
file(WRITE "${declarations}/relay.avx" "import float_values;public u16 bits<T>(T ignored){return Float.to_bits(float_values.HALF);}\n")
file(WRITE "${declarations}/main.avx" "import float_values;import relay;T identity<T>(T input){return input;}i32 main(){return identity(Float.to_bits(float_values.HALF))==15360u16 && Float.to_bits(identity(float_values.DOUBLE))==4611686018427387904u64 && relay.bits(0)==15360u16?0:1;}\n")
floating_run("qualified constants check" 0 "ownership checked" --check "${declarations}/main.avx")
foreach(optimization -O0 -O2)
    floating_run("qualified constants AOT ${optimization}" 0 "" --run "${optimization}" "${declarations}/main.avx")
    if(version MATCHES "ORC JIT enabled")
        floating_run("qualified constants JIT ${optimization}" 0 "" --jit "${optimization}" "${declarations}/main.avx")
    endif()
endforeach()
file(WRITE "${declarations}/private.avx" "import float_values;i32 main(){rsn invalid=Float.to_bits(float_values.HIDDEN);return 0;}\n")
floating_run("private constant remains private" 1 "private|Unknown|visible|access" --check "${declarations}/private.avx")
file(WRITE "${declarations}/holder.avx" "import float_values;public i32 present(){return 1;}\n")
file(WRITE "${declarations}/transitive.avx" "import holder;i32 main(){rsn invalid=Float.to_bits(float_values.HALF);return 0;}\n")
floating_run("transitive constant not imported" 1 "Unknown|import|visible|access" --check "${declarations}/transitive.avx")
execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/purity.avx" RESULT_VARIABLE emitted OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 180)
if(NOT "${emitted}" STREQUAL "0" OR "${ir}" MATCHES "@(malloc|calloc|realloc|sin|cos|pow|fabs|copysign|lgamma)[(]")
    message(FATAL_ERROR "Pure floating helpers unexpectedly depend on allocation/C math: ${error}")
endif()
foreach(case integer mixed)
    if(case STREQUAL "integer")
        set(expression "floating.next_up(3)")
    else()
        set(expression "floating.copy_sign(1.0f32,1.0f64)")
    endif()
    file(WRITE "${TEST_DIR}/invalid_${case}.avx" "import floating;i32 main(){rsn invalid=${expression};return 0;}\n")
    floating_run("reject ${case}" 1 "overload|matching|type|requires|Float|float" --check "${TEST_DIR}/invalid_${case}.avx")
endforeach()
file(SHA256 "${AEVIXC}" final_compiler)
if(NOT original_compiler STREQUAL final_compiler)
    message(FATAL_ERROR "Compiler binary changed during floating library gate")
endif()
message(STATUS "Pure floating library passed exhaustive16-bit records, rational goldens, exact constants, canonical FloatClass, and worker execution")
