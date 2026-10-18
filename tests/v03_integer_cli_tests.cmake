# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_integer_cli_tests.cmake
#
# Description :
#     전 정수 폭의 산술·비트 계약, 독립 정밀 정수 오라클과 잘못된 호출을 검사한다.
#     Check arithmetic and bit contracts for every integer width against independent big integers.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
find_program(PYTHON_EXECUTABLE NAMES python3 REQUIRED)
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version TIMEOUT 10)
file(SHA256 "${AEVIXC}" compiler_sha)
message(STATUS "Integer source gate uses compiler SHA256 ${compiler_sha}")
set(module_flags)
if(DEFINED TEST_MODULE_ROOT)
    list(APPEND module_flags --module-root "${TEST_MODULE_ROOT}")
endif()
function(integer_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${module_flags} ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 180)
    if(NOT "${status}" STREQUAL "${expected}" OR
       (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
execute_process(COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_integer_golden.py"
    --output-dir "${TEST_DIR}" --metadata "${TEST_DIR}/integer_golden.json"
    RESULT_VARIABLE generated OUTPUT_VARIABLE summary ERROR_VARIABLE error TIMEOUT 60)
if(NOT "${generated}" STREQUAL "0")
    message(FATAL_ERROR "Independent integer oracle failed: ${error}")
endif()
message(STATUS "${summary}")
file(WRITE "${TEST_DIR}/integer_worker_named.avx" [=[
import integer;
import task { Task, TaskScope, Executor, ScopeOptions, ScopeReport };
T success<T,E>(Result<T,E> input) {
    switch(move input) { case Result<T,E>.Ok { T output=value }: return output; break; default: Never.unreachable(); break; }
}
i32 main() {
    i8 named=success(integer.checked_add(right=2i8,left=3i8));
    if(named!=5i8)return 1;
    Executor executor=Executor.native_threads();
    TaskScope scope=success(Task.open_scope(&executor,ScopeOptions.defaults()));
    Task<Result<i128,integer.IntegerArithmeticError>> child=success(Task.spawn_owned(&scope,[]()->Result<i128,integer.IntegerArithmeticError>{return integer.checked_pow(-2i128,127u32);}));
    Result<i128,integer.IntegerArithmeticError> nested=success(Task.join(child));
    i128 actual=success(nested);
    ScopeReport report=success(Task.close_scope(scope));
    return actual == -170141183460469231731687303715884105728i128 && report.completed == 1u64 ? 0 : 2;
}
]=])
foreach(fixture i8 u8 i16 u16 i32 u32 i64 u64 i128 u128 isize usize extrema worker_named)
    integer_run("${fixture} check" 0 "ownership checked" --check "${TEST_DIR}/integer_${fixture}.avx")
    foreach(optimization -O0 -O2)
        integer_run("${fixture} AOT ${optimization}" 0 "" --run "${optimization}" "${TEST_DIR}/integer_${fixture}.avx")
        if(version MATCHES "ORC JIT enabled")
            integer_run("${fixture} JIT ${optimization}" 0 "" --jit "${optimization}" "${TEST_DIR}/integer_${fixture}.avx")
        endif()
    endforeach()
endforeach()
function(integer_reject name body pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${body}\n")
    integer_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()
integer_reject(mixed_width "import integer; i32 main(){i8 a=1i8;i16 b=2i16;integer.checked_add(a,b);return 0;}" "matching overload")
integer_reject(signedness "import integer; i32 main(){i8 a=1i8;u8 b=2u8;integer.wrapping_mul(a,b);return 0;}" "matching overload")
integer_reject(float "import integer; i32 main(){f32 a=1.0f32;integer.count_ones(a);return 0;}" "matching overload")
integer_reject(boolean "import integer; i32 main(){bool a=true;integer.checked_add(a,a);return 0;}" "matching overload")
integer_reject(unsigned_negation "import integer; i32 main(){u8 a=1u8;integer.checked_neg(a);return 0;}" "matching overload")
integer_reject(private_helper "import integer; i32 main(){integer.bits(1i8);return 0;}" "private")
integer_reject(unimported "i32 main(){integer.checked_add(1i8,2i8);return 0;}" "Unknown name")
integer_reject(transitive_import "import integer; i32 main(){integer.Error value;return 0;}" "not imported")
file(WRITE "${TEST_DIR}/integer_noallocation.avx" [=[
import integer;
i32 main() { return integer.wrapping_add(127i8,1i8)==-128i8 ? 0 : 1; }
]=])
execute_process(COMMAND "${AEVIXC}" ${module_flags} --emit-llvm "${TEST_DIR}/integer_noallocation.avx"
    RESULT_VARIABLE emitted OUTPUT_VARIABLE llvm ERROR_VARIABLE error TIMEOUT 60)
if(NOT "${emitted}" STREQUAL "0")
    message(FATAL_ERROR "Integer IR emission failed: ${error}")
endif()
if(llvm MATCHES "(call|declare)[^\n]*@(aevix_[A-Za-z0-9_]*(alloc|realloc)|malloc|calloc|realloc)")
    message(FATAL_ERROR "Pure integer source unexpectedly references an allocator")
endif()
file(SHA256 "${AEVIXC}" final_compiler_sha)
if(NOT compiler_sha STREQUAL final_compiler_sha)
    message(FATAL_ERROR "The compiler changed during the Integer source gate")
endif()
message(STATUS "Integer: 12 primitive types, all 528 top-level overloads, 24 constants, direct extrema, dual-prime row fingerprints, worker Result, 8 rejections, and no-allocation IR passed")
