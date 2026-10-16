# ========================
#
# Created By : miruku_sora2026
#
# File name : closure_parser_tests.cmake
#
# Description :
#     클로저의 독립적인 오류 전파·삼항식·중첩 구문을 실행하고 실제 물음표 모호성은 거절한다.
#     Execute independent closure propagation, ternaries, and nesting while rejecting genuine question-mark ambiguity.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR OR NOT DEFINED CLANG)
    message(FATAL_ERROR "AEVIXC, TEST_DIR and CLANG are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")

function(run_case name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE code
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
    if(NOT "${code}" STREQUAL "${expected}" OR
        (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${code}\n${output}\n${error}")
    endif()
endfunction()

set(result_entry [=[
i32 main() {
    Result<i32, TextError> result = run();
    switch (move result) {
        case Result<i32, TextError>.Ok { i32 answer = value }: return answer; break;
        default: return 99; break;
    }
}
]=])
file(WRITE "${TEST_DIR}/propagation.avx" [=[
Result<i32, TextError> ok(i32 value) { return Result.ok(value); }
Result<i32, TextError> run() {
    rsn callback = []() -> Result<i32, TextError> {
        i32 value = ok(40)?;
        return Result.ok(value + 2);
    };
    return callback();
}
]=] "${result_entry}")
file(WRITE "${TEST_DIR}/ternary.avx" [=[
i32 main() {
    rsn callback = [](bool choose) -> i32 { return choose ? 42 : 7; };
    return callback(true);
}
]=])
file(WRITE "${TEST_DIR}/nested.avx" [=[
Result<i32, TextError> ok(i32 value) { return Result.ok(value); }
Result<i32, TextError> run() {
    rsn outer = [](bool choose) -> Result<i32, TextError> {
        rsn inner = [](i32 input) -> Result<i32, TextError> {
            i32 value = ok(input)?;
            return Result.ok(value + 2);
        };
        i32 input = choose ? 40 : 5;
        i32 answer = inner(input)?;
        return Result.ok(answer);
    };
    return outer(true);
}
]=] "${result_entry}")
file(WRITE "${TEST_DIR}/default_argument.avx" [=[
i32 main() {
    rsn callback = [](i32 value = (true ? 42 : 1)) -> i32 { return value; };
    return callback();
}
]=])

execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
foreach(name propagation ternary nested default_argument)
    run_case("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    foreach(level 0 2)
        run_case("${name} AOT O${level}" 42 "" --run --clang "${CLANG}"
            "-O${level}" "${TEST_DIR}/${name}.avx")
        if(version MATCHES "JIT enabled")
            run_case("${name} JIT O${level}" 42 "" --jit
                "-O${level}" "${TEST_DIR}/${name}.avx")
        endif()
    endforeach()
endforeach()

file(WRITE "${TEST_DIR}/ambiguous.avx" [=[
i32 main() {
    rsn callback = []() -> i32 { return a ? + b ? + c : d; };
    return 0;
}
]=])
run_case("ambiguous closure body" 1 "Ambiguous '\\?' expression"
    --check "${TEST_DIR}/ambiguous.avx")
file(WRITE "${TEST_DIR}/outer_ambiguous.avx"
    "i32 main() { return a ? + b ? + c : d; }\n")
run_case("ambiguous enclosing expression" 1 "Ambiguous '\\?' expression"
    --check "${TEST_DIR}/outer_ambiguous.avx")
message(STATUS "Closure propagation, ternaries, nested expressions and ambiguity rejection passed")
