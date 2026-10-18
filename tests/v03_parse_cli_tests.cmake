# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_parse_cli_tests.cmake
#
# Description :
#     타입별 파싱의 Result·오류 전파·문자열 대여와 거부 규칙을 AOT·JIT에서 검사한다.
#     Check typed parse Results, error propagation, string loans, and rejections through AOT and JIT.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
function(run_parse name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR
        (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
file(WRITE "${TEST_DIR}/types.avx" "public type Index = i16;\n")
set(program "import types { Index };\n")
set(assertions "")
foreach(type IN ITEMS i8 u8 i16 u16 i32 u32 i64 u64 i128 u128 isize usize int f32 f64 float)
    if(type MATCHES "^(f32|f64|float)$")
        set(wanted "42.0")
    else()
        set(wanted "42")
    endif()
    string(APPEND program "Result<i32, ParseError> test_${type}() { ${type} value = ${type}.parse(\"42\")?; return Result.ok(value == ${wanted} ? 0 : 1); }\n")
    string(APPEND assertions "if (test_${type}()? != 0) return Result.ok(1);\n")
endforeach()
string(APPEND program [=[
type Small = i16;
type Another = Small;
Result<i32, ParseError> checks() {
]=])
string(APPEND program "${assertions}")
string(APPEND program [=[
    Small alias = Small.parse("-32768")?;
    Another nested_alias = Another.parse("17")?;
    Index imported_alias = Index.parse("18")?;
    types.Index qualified_alias = types.Index.parse("19")?;
    if (nested_alias != 17 || imported_alias != 18 || qualified_alias != 19) return Result.ok(4);
    i128 minimum = i128.parse("-170141183460469231731687303715884105728")?;
    u128 maximum = u128.parse("340282366920938463463374607431768211455")?;
    if (alias != -32768 || minimum != -170141183460469231731687303715884105728i128 || maximum != 340282366920938463463374607431768211455u128) return Result.ok(2);
    bool yes = bool.parse("true")?;
    bool no = bool.parse("false")?;
    char letter = char.parse("한")?;
    byte octet = byte.parse("255")?;
    if (!yes || no || letter != '한' || Byte.to_u8(octet) != 255u8) return Result.ok(3);
    return Result.ok(0);
}
i32 main() {
    Result<i32, ParseError> result = checks();
    switch (result) {
        case Result<i32, ParseError>.Ok { i32 code = value }: return code; break;
        case Result<i32, ParseError>.Err: return 99; break;
    }
}
]=])
# 소유 문자열 생성은 TextError를 사용하므로 별도 함수에서 대여를 검사한다.
# Owned text construction uses TextError, so test its borrow in a separate function.
file(WRITE "${TEST_DIR}/main.avx" "${program}")
run_parse(check 0 "" --check "${TEST_DIR}/main.avx")
run_parse(air 0 "parse_intrinsic" --emit-air "${TEST_DIR}/main.avx")
run_parse(llvm 0 "aevix_parse_scalar" --emit-llvm "${TEST_DIR}/main.avx")
run_parse(aot 0 "" --run "${TEST_DIR}/main.avx")
run_parse(aot_optimized 0 "" --run -O2 "${TEST_DIR}/main.avx")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
if(version MATCHES "ORC JIT enabled")
    run_parse(jit 0 "" --jit "${TEST_DIR}/main.avx")
endif()

function(parse_error name type text variant)
    file(WRITE "${TEST_DIR}/${name}.avx" "i32 main() { Result<${type}, ParseError> result = ${type}.parse(\"${text}\"); switch (move result) { case Result<${type}, ParseError>.Ok: return 1; break; case Result<${type}, ParseError>.Err { ParseError problem = error }: switch (problem) { case ParseError.${variant}: return 0; break; default: return 2; break; } break; } }\n")
    run_parse("${name} AOT" 0 "" --run "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        run_parse("${name} JIT" 0 "" --jit "${TEST_DIR}/${name}.avx")
    endif()
endfunction()
parse_error(empty i32 "" EmptyInput)
parse_error(whitespace i32 " 1" InvalidSyntax)
parse_error(overflow i8 "128" OutOfRange)
parse_error(unsigned_negative u8 "-1" InvalidSyntax)
parse_error(nonfinite f64 "nan" NonFinite)
parse_error(float_overflow f32 "1e9999" OutOfRange)
parse_error(scalar char "ab" InvalidScalar)
parse_error(upper_bool bool "True" InvalidSyntax)

file(WRITE "${TEST_DIR}/borrow.avx" [=[
i32 main() {
    Result<string, TextError> owner = string.from_view("19");
    switch (move owner) {
        case Result<string, TextError>.Ok { string text = value }:
            Result<i32, ParseError> parsed = i32.parse(&text);
            switch (parsed) {
                case Result<i32, ParseError>.Ok { i32 value = value }: return value == 19 ? 0 : 1; break;
                case Result<i32, ParseError>.Err: return 2; break;
            }
            break;
        case Result<string, TextError>.Err: return 3; break;
    }
}
]=])
run_parse(borrow 0 "" --run "${TEST_DIR}/borrow.avx")
if(version MATCHES "ORC JIT enabled")
    run_parse(borrow_jit 0 "" --jit "${TEST_DIR}/borrow.avx")
endif()
function(invalid_parse name text pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${text}\n")
    run_parse("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()
invalid_parse(number "i32 main() { i32.parse(10); return 0; }" "parse requires string_view")
invalid_parse(arity "i32 main() { i32.parse(\"1\", \"2\"); return 0; }" "requires one text")
invalid_parse(half "i32 main() { f16.parse(\"1\"); return 0; }" "supports integer types")
invalid_parse(brain "i32 main() { bf16.parse(\"1\"); return 0; }" "supports integer types")
invalid_parse(string "i32 main() { string.parse(\"1\"); return 0; }" "supports integer types")
invalid_parse(no_implicit_unwrap "i32 main() { i32 value = i32.parse(\"1\"); return value; }" "[Ii]nitializer")
invalid_parse(unborrowed "i32 bad(string text) { i32.parse(text); return 0; } i32 main() { return 0; }" "parse requires string_view")
message(STATUS "Typed scalar parse Results and errors passed")
