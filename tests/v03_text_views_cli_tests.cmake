# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_text_views_cli_tests.cmake
#
# Description :
#     UTF-8 분할·행 처리와 빌린 문자열 목록의 수명·할당 실패를 검사한다.
#     Check UTF-8 splitting, line handling, borrowed-list lifetimes, and allocation failure.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/v03_library_text_views.avx" "${TEST_DIR}/text_views_usage.avx" COPYONLY)
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
function(text_views_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR
        (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
text_views_run("borrowed text views check" 0 "ownership checked" --check "${TEST_DIR}/text_views_usage.avx")
foreach(optimization -O0 -O2)
    text_views_run("borrowed text views AOT ${optimization}" 0 "" --run "${optimization}" "${TEST_DIR}/text_views_usage.avx")
    if(version MATCHES "ORC JIT enabled")
        text_views_run("borrowed text views JIT ${optimization}" 0 "" --jit "${optimization}" "${TEST_DIR}/text_views_usage.avx")
    endif()
endforeach()
function(text_views_reject name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "import text;\n${source}\n")
    text_views_run("reject ${name}" 1 "reference.*local|borrow|outlive|escape" --check "${TEST_DIR}/${name}.avx")
endfunction()
text_views_reject(local_split_escape [=[
Result<Vector<string_view>, TextError> escape() {
    string local = string.from_view("a,b")?;
    return text.split(string.view(&local), ",");
}
i32 main() { return 0; }
]=])
text_views_reject(local_lines_escape [=[
Result<Vector<string_view>, TextError> escape() {
    string local = string.from_view("a\nb")?;
    return text.lines(string.view(&local));
}
i32 main() { return 0; }
]=])
text_views_reject(mutate_split_owner [=[
Result<i32, TextError> run() {
    mut string owner = string.from_view("a,b")?;
    Vector<string_view> parts = text.split(string.view(&owner), ",")?;
    string.append(&mut owner, "changed")?;
    return Result.ok(Vector.len(&parts) == 2 ? 0 : 1);
}
i32 main() { return 0; }
]=])
text_views_reject(prefix_escape [=[
Result<Optional<string_view>, TextError> escape() {
    string local = string.from_view("prefix:value")?;
    return text.strip_prefix(string.view(&local), "prefix:");
}
i32 main() { return 0; }
]=])
# 첫 번째와 두 번째 성장 할당을 각각 실패시켜 부분 목록의 정리와 입력 보존을 검사한다.
# Fail first and second growth allocations separately to check partial-list cleanup and input preservation.
if(DEFINED CLANG AND EXISTS "${CLANG}" AND DEFINED RUNTIME AND EXISTS "${RUNTIME}")
    file(WRITE "${TEST_DIR}/text_failure.avx" [=[
import text;
extern "C" void text_failure_point(u32 attempt);
extern "C" u32 text_outstanding();
extern "C" u32 text_attempts();
bool allocation_failed(Result<Vector<string_view>, TextError> input) {
    switch (move input) {
        case Result<Vector<string_view>, TextError>.Ok: return false; break;
        case Result<Vector<string_view>, TextError>.Err { TextError problem = error }:
            switch (problem) { case TextError.AllocationFailure: return true; break; default: return false; break; }
            break;
    }
}
i32 main() {
    mut u32 point = 1;
    while (point <= 2) {
        text_failure_point(point);
        if (!allocation_failed(text.split("a,b,c,d,e,f,g,h,i", ","))) return 1;
        if (text_outstanding() != 0 || text_attempts() != point) return 2;
        text_failure_point(point);
        if (!allocation_failed(text.lines("a\nb\nc\nd\ne\nf\ng\nh\ni"))) return 3;
        if (text_outstanding() != 0 || text_attempts() != point) return 4;
        text_failure_point(point);
        if (!allocation_failed(text.split_ascii_whitespace("a b c d e f g h i"))) return 5;
        if (text_outstanding() != 0 || text_attempts() != point) return 6;
        point++;
    }
    return 0;
}
]=])
    text_views_run("text allocation failure source check" 0 "" --check "${TEST_DIR}/text_failure.avx")
    execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/text_failure.avx"
        RESULT_VARIABLE emitted OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${emitted}" STREQUAL "0")
        message(FATAL_ERROR "Text allocation-failure IR emission failed: ${error}")
    endif()
    string(REPLACE "@malloc(" "@text_test_allocate(" ir "${ir}")
    string(REPLACE "@free(" "@text_test_free(" ir "${ir}")
    file(WRITE "${TEST_DIR}/text_failure.ll" "${ir}")
    file(WRITE "${TEST_DIR}/text_failure.c" [=[
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
static uint32_t fail_at, calls, outstanding;
void text_failure_point(uint32_t point) { fail_at = point; calls = 0; }
uint32_t text_outstanding(void) { return outstanding; }
uint32_t text_attempts(void) { return calls; }
void *text_test_allocate(size_t length) {
    if (++calls == fail_at) return NULL;
    void *value = malloc(length);
    if (value) outstanding++;
    return value;
}
void text_test_free(void *value) { if (value) outstanding--; free(value); }
]=])
    set(extra_libraries -pthread)
    if(APPLE)
        list(APPEND extra_libraries -lc++)
    elseif(UNIX)
        list(APPEND extra_libraries -lstdc++ -lm)
    endif()
    if(WIN32)
        set(failure_binary "${TEST_DIR}/text_failure.exe")
    else()
        set(failure_binary "${TEST_DIR}/text_failure")
    endif()
    execute_process(COMMAND "${CLANG}" "${TEST_DIR}/text_failure.ll" "${TEST_DIR}/text_failure.c"
        "${RUNTIME}" ${extra_libraries} -o "${failure_binary}"
        RESULT_VARIABLE linked OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
    if(NOT "${linked}" STREQUAL "0")
        message(FATAL_ERROR "Text allocation-failure fixture link failed: ${output}${error}")
    endif()
    execute_process(COMMAND "${failure_binary}" RESULT_VARIABLE ran OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
    if(NOT "${ran}" STREQUAL "0")
        message(FATAL_ERROR "Text allocation-failure cleanup failed (${ran}): ${output}${error}")
    endif()
else()
    message(STATUS "Text deterministic allocation-failure gate requires CLANG and RUNTIME")
endif()
message(STATUS "UTF-8 views, bounded splitting, line endings, whitespace, owner lifetimes and allocation cleanup passed")
