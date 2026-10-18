# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_text_cli_tests.cmake
#
# Description :
#     소스 문자열 라이브러리의 모든 공개 함수와 뷰의 수명 제한을 검사한다.
#     Exercise every public source text helper and reject escaping borrowed views.
#
# ========================
file(MAKE_DIRECTORY "${TEST_DIR}")
file(WRITE "${TEST_DIR}/main.avx" [=[
import text;

usize index_or(Optional<usize> position) {
    switch (move position) {
        case Optional<usize>.Some { usize offset = value }: return offset; break;
        case Optional<usize>.None: return 999; break;
    }
}
Result<i32, TextError> exercise() {
    if (text.byte_len("한😀") != 7 || text.scalar_count("한😀") != 2) return Result.ok(1);
    if (!text.is_empty("") || text.is_empty(" ") || !text.equal("한", "한") || text.equal("한", "글")) return Result.ok(2);
    if (!text.is_ascii("abc\n") || text.is_ascii("한")) return Result.ok(3);
    if (!text.starts_with("한글", "한") || text.starts_with("a", "ab")) return Result.ok(4);
    if (!text.ends_with("한글", "글") || text.ends_with("a", "ab")) return Result.ok(5);
    if (index_or(text.find("a한b한", "한")) != 1 || index_or(text.rfind("a한b한", "한")) != 5) return Result.ok(6);
    if (index_or(text.find_from("a한b한", "한", 2)) != 5 || index_or(text.find_from("a", "a", 2)) != 999) return Result.ok(7);
    if (index_or(text.find("abc", "")) != 0 || index_or(text.rfind("abc", "")) != 3 || index_or(text.find("", "x")) != 999) return Result.ok(8);
    if (!text.contains("a한b", "한") || text.contains("a", "b")) return Result.ok(9);
    if (text.count("aaaaa", "aa") != 2 || text.count("abc", "") != 0) return Result.ok(10);
    string_view start = text.trim_start_ascii(" \t한  ")?;
    string_view end = text.trim_end_ascii("  한\r\n")?;
    string_view both = text.trim_ascii(" \t한\r\n")?;
    if (!text.equal(start, "한  ") || !text.equal(end, "  한") || !text.equal(both, "한")) return Result.ok(11);
    Optional<Tuple<string_view, string_view>> split = text.split_once("left=한", "=")?;
    switch (move split) {
        case Optional<Tuple<string_view, string_view>>.Some { Tuple<string_view, string_view> pair = value }:
            if (!text.equal(*Tuple.get<0>(&pair), "left") || !text.equal(*Tuple.get<1>(&pair), "한")) return Result.ok(12);
            break;
        case Optional<Tuple<string_view, string_view>>.None: return Result.ok(13); break;
    }
    Optional<Tuple<string_view, string_view>> missing = text.split_once("abc", "=")?;
    if (!Optional.is_none(&missing)) return Result.ok(14);
    string joined = text.concat("한", "😀")?;
    string repeated = text.repeat("한", 3)?;
    string replaced = text.replace("한a한b", "한", "😀")?;
    string unchanged = text.replace("abc", "", "x")?;
    if (!string.equals(&joined, "한😀") || !string.equals(&repeated, "한한한") || !string.equals(&replaced, "😀a😀b") || !string.equals(&unchanged, "abc")) return Result.ok(15);
    Result<string, TextError> overflow = text.repeat("ab", 18446744073709551615);
    if (!Result.is_err(&overflow)) return Result.ok(16);
    return Result.ok(0);
}
i32 main() {
    Result<i32, TextError> outcome = exercise();
    switch (move outcome) {
        case Result<i32, TextError>.Ok { i32 code = value }: return code; break;
        case Result<i32, TextError>.Err: return 99; break;
    }
}
]=])
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(modes --check --run)
if(version MATCHES "ORC JIT enabled")
    list(APPEND modes --jit)
endif()
foreach(mode IN LISTS modes)
    execute_process(COMMAND "${AEVIXC}" "${mode}" "${TEST_DIR}/main.avx"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "0")
        message(FATAL_ERROR "text ${mode}: ${status}: ${output}${error}")
    endif()
endforeach()
file(WRITE "${TEST_DIR}/escape.avx" [=[
import text;
Result<string_view, TextError> escape() {
    string owner = string.from_view("unsafe")?;
    return text.trim_ascii(string.view(&owner));
}
i32 main() { return 0; }
]=])
execute_process(COMMAND "${AEVIXC}" --check "${TEST_DIR}/escape.avx"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if("${status}" STREQUAL "0" OR NOT "${output}${error}" MATCHES "borrow|Borrow|local|Local|outlive|lifetime|escape")
    message(FATAL_ERROR "text view escape was not rejected: ${status}: ${output}${error}")
endif()
message(STATUS "All source text functions and borrowed-view lifetime checks passed")
