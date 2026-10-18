# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_raw_cli_tests.cmake
#
# Description :
#     원시 문자열의 정확한 바이트와 상수·기본값·제네릭 사용을 AOT·JIT로 검사한다.
#     Check exact raw literal bytes, constants, defaults, and generic inference through AOT and JIT.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR OR NOT DEFINED RAW_FIXTURE_TOOL)
    message(FATAL_ERROR "AEVIXC, TEST_DIR, and RAW_FIXTURE_TOOL are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")

function(run_raw name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${result}" STREQUAL "${expected}" OR
        (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${result}: ${output}${error}")
    endif()
endfunction()

execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
function(execute_raw name)
    run_raw("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    run_raw("${name} AOT" 0 "" --run "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        run_raw("${name} JIT" 0 "" --jit "${TEST_DIR}/${name}.avx")
    endif()
endfunction()

set(program [=[
const string_view global_raw = r#"global\path "quoted""#;
const string_view alias_raw = global_raw;
T identity<T>(T value) { return value; }
i32 default_path(string_view value = r"C:\temp\main.avx") {
    return string.equals(value, "C:\\temp\\main.avx") ? 0 : 1;
}
string_view read_global() { return alias_raw; }
i32 main() {
    string_view path = r"C:\temp\main.avx";
    if (string.byte_len(path) != 16 || !string.equals(path, "C:\\temp\\main.avx")) return 1;
    rsn inferred = identity(r#"say "hello""#);
    if (string.byte_len(inferred) != 11 || !string.equals(inferred, "say \"hello\"")) return 2;
    string_view marker = r##"a "# marker"##;
    if (!string.equals(marker, "a \"# marker")) return 3;
    string_view escapes = r"\n\t\u{41}";
    if (string.byte_len(escapes) != 10 || !string.equals(escapes, "\\n\\t\\u{41}")) return 4;
    if (!string.equals(r"", "") || string.byte_len(r#""#) != 0) return 5;
    string_view unicode = r"Aé한🙂";
    if (string.byte_len(unicode) != 10 || string.scalar_count(unicode) != 4) return 6;
    if (!string.equals(unicode, "A\u{e9}한🙂")) return 7;
    string_view decomposed = r"é";
    if (string.byte_len(decomposed) != 3 || !string.equals(decomposed, "e\u{301}")) return 8;
    string_view multiline = r#"first@CRLF@second@LF@third@CR@fourth"#;
    if (!string.equals(multiline, "first\r\nsecond\nthird\rfourth")) return 9;
    if (!string.equals(read_global(), "global\\path \"quoted\"") || default_path() != 0) return 10;
    string_view ordinary = "line\n\t\u{41}\0";
    if (string.byte_len(ordinary) != 8 || string.scalar_count(ordinary) != 8) return 11;
    return 0;
}
]=])
string(ASCII 13 carriage_return)
string(ASCII 10 line_feed)
string(REPLACE "@CRLF@" "${carriage_return}${line_feed}" program "${program}")
string(REPLACE "@LF@" "${line_feed}" program "${program}")
string(REPLACE "@CR@" "${carriage_return}" program "${program}")
file(WRITE "${TEST_DIR}/main.avx" "${program}")
execute_raw(main)

# 큰 구분자와 불충분한 닫는 구분자를 실행 결과까지 함께 검사한다.
# Check maximum delimiters and insufficient closing delimiters through execution.
string(REPEAT "#" 255 max_hashes)
string(REPEAT "#" 254 shorter_hashes)
file(WRITE "${TEST_DIR}/maximum.avx"
    "i32 main() { string_view text = r${max_hashes}\"before\"${shorter_hashes}inside\"${max_hashes}; return string.byte_len(text) == 267 ? 0 : 1; }\n")
execute_raw(maximum)

execute_process(COMMAND "${RAW_FIXTURE_TOOL}" --write-cli-fixtures "${TEST_DIR}"
    RESULT_VARIABLE fixture_result OUTPUT_VARIABLE fixture_output ERROR_VARIABLE fixture_error TIMEOUT 30)
if(NOT fixture_result EQUAL 0)
    message(FATAL_ERROR "Raw binary fixture generation failed: ${fixture_output}${fixture_error}")
endif()
execute_raw(embedded_nul)
foreach(index RANGE 0 4)
    run_raw("invalid UTF-8 ${index}" 1 "Invalid UTF-8 byte sequence in raw string"
        --check "${TEST_DIR}/invalid_utf8_${index}.avx")
endforeach()

file(WRITE "${TEST_DIR}/unterminated.avx" "i32 main() { string_view text = r#\"first\nsecond\"; return 0; }\n")
run_raw(unterminated 1 "Unterminated raw string literal" --check "${TEST_DIR}/unterminated.avx")
string(REPEAT "#" 256 excessive_hashes)
file(WRITE "${TEST_DIR}/excessive.avx"
    "i32 main() { string_view text = r${excessive_hashes}\"too many\"${excessive_hashes}; return 0; }\n")
run_raw(excessive 1 "Raw string delimiter exceeds 255" --check "${TEST_DIR}/excessive.avx")
file(WRITE "${TEST_DIR}/ordinary_newline.avx" "i32 main() { string_view text = \"first\nsecond\"; return 0; }\n")
run_raw(ordinary_newline 1 "Unterminated quoted literal" --check "${TEST_DIR}/ordinary_newline.avx")
message(STATUS "Raw string source bytes and AOT/JIT execution passed")
