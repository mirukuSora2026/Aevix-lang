# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_map_entry_cli_tests.cmake
#
# Description :
#     해시 항목 대여의 실제 실행·정확한 정리·할당 실패 복구와 배타성 위반을 검사한다.
#     Check actual hash-entry loans, exact cleanup, allocation failure recovery, and exclusivity violations.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
set(source_dir "${TEST_DIR}/sources")
file(MAKE_DIRECTORY "${source_dir}")
# 이전 실행이 만든 표준 모듈과 같은 이름의 시험 파일만 정리한다.
# Remove only the old generated fixture that shadows the registered standard module.
file(REMOVE "${source_dir}/map_entry.avx")
file(SHA256 "${AEVIXC}" compiler_sha)
message(STATUS "MapEntry source gates use compiler SHA256 ${compiler_sha}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)

function(entry_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 180)
    if(NOT "${result}" STREQUAL "${expected}" OR
       (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${result}\n${output}${error}")
    endif()
endfunction()
foreach(name IN ITEMS map_entry map_entry_ownership map_entry_text)
    set(fixture "entry_usage")
    if(name STREQUAL "map_entry_ownership")
        set(fixture "entry_ownership_usage")
    elseif(name STREQUAL "map_entry_text")
        set(fixture "entry_text_usage")
    endif()
    configure_file("${CMAKE_CURRENT_LIST_DIR}/v03_${name}.avx" "${source_dir}/${fixture}.avx" COPYONLY)
    entry_run("${name} check" 0 "" --check "${source_dir}/${fixture}.avx")
    entry_run("${name} AOT O0" 0 "" --run -O0 "${source_dir}/${fixture}.avx")
    entry_run("${name} AOT O2" 0 "" --run -O2 "${source_dir}/${fixture}.avx")
    if(version MATCHES "ORC JIT enabled")
        entry_run("${name} JIT O0" 0 "" --jit -O0 "${source_dir}/${fixture}.avx")
        entry_run("${name} JIT O2" 0 "" --jit -O2 "${source_dir}/${fixture}.avx")
    endif()
endforeach()

set(prefix [=[
import map { Map };
import map_entry;
import map_entry { MapEntry };
Map<i32, i32> make_map() {
    Result<Map<i32, i32>, CollectionError> made = Map<i32, i32>.new();
    switch (move made) { case Result<Map<i32, i32>, CollectionError>.Ok { Map<i32, i32> value = value }: return value; break; case Result<Map<i32, i32>, CollectionError>.Err: Never.unreachable(); break; }
}
]=])
function(entry_reject name pattern source)
    file(WRITE "${source_dir}/${name}.avx" "${prefix}\n${source}\n")
    entry_run("${name}" 1 "${pattern}" --check "${source_dir}/${name}.avx")
endfunction()
entry_reject(entry_keeps_owner "Cannot modify, move, or exclusively borrow a place while it is borrowed" [=[
i32 main() {
    mut Map<i32, i32> owner = make_map();
    rsn entry = map_entry.entry_with(&mut owner, 1, 1u64, [](&i32 a, &i32 b) -> bool { return *a == *b; });
    owner.clear();
    return entry.is_occupied() ? 1 : 0;
}
]=])
entry_reject(returned_value_keeps_owner "Cannot modify, move, or exclusively borrow a place while it is borrowed" [=[
i32 main() {
    mut Map<i32, i32> owner = make_map();
    rsn entry = map_entry.entry_with(&mut owner, 1, 1u64, [](&i32 a, &i32 b) -> bool { return *a == *b; });
    rsn result = entry.or_insert(2);
    owner.clear();
    return Result.is_ok(&result) ? 0 : 1;
}
]=])
entry_reject(keys_are_immutable "Cannot modify an immutable binding or a read-only borrow" [=[
i32 main() {
    mut Map<i32, i32> owner = make_map();
    rsn entry = map_entry.entry_with(&mut owner, 1, 1u64, [](&i32 a, &i32 b) -> bool { return *a == *b; });
    *entry.key() = 4;
    return 0;
}
]=])
entry_reject(entry_cannot_escape "Cannot return.*local" [=[
MapEntry<i32, i32, F> escaped<F>(F same) where F implements Fn<Tuple<&i32, &i32>, bool> {
    mut Map<i32, i32> owner = make_map();
    return map_entry.entry_with(&mut owner, 1, 1u64, same);
}
i32 main() { rsn entry = escaped([](&i32 a, &i32 b) -> bool { return *a == *b; }); return entry.is_occupied() ? 1 : 0; }
]=])
entry_reject(comparator_capture_keeps_owner "Cannot modify, move, or exclusively borrow a place while it is borrowed" [=[
i32 main() {
    mut Map<i32, i32> owner = make_map();
    mut i32 salt = 0;
    rsn entry = map_entry.entry_with(&mut owner, 1, 1u64, [&salt](&i32 a, &i32 b) -> bool { return *a + salt == *b + salt; });
    salt = 1;
    return entry.is_occupied() ? 1 : 0;
}
]=])
entry_reject(private_representation "Cannot access private.*Handle" [=[
MapEntry<i32, i32, F> forged<F>(&mut Map<i32, i32> owner, F same) where F implements Fn<Tuple<&i32, &i32>, bool> {
    return MapEntry<i32, i32, F>.Handle { owner = owner, incoming = 1, same = same, code = 1u64, occupied = false, slot = 0 };
}
i32 main() { return 0; }
]=])

entry_reject(short_reference_cannot_escape_entry "outlives|local.*reference|borrowed|escaping" [=[
Result<i32, CollectionError> exercise() {
    mut Map<i32, &i32> owner = Map<i32, &i32>.new()?;
    {
        i32 local = 7;
        rsn entry = map_entry.entry_with(&mut owner, 1, 1u64, [](&i32 a, &i32 b) -> bool { return *a == *b; });
        rsn inserted = entry.or_insert(&local);
        if (!Result.is_ok(&inserted)) return Result.ok(90);
    }
    i32 key = 1;
    rsn same = [](&i32 a, &i32 b) -> bool { return *a == *b; };
    Optional<& &i32> retained = owner.get_with(&key, 1u64, &same);
    switch (move retained) { case Optional<& &i32>.Some { & &i32 value = value }: return Result.ok(**value); break; case Optional<& &i32>.None: return Result.ok(91); break; }
}
i32 main() { Result<i32, CollectionError> result = exercise(); switch (move result) { case Result<i32, CollectionError>.Ok { i32 code = value }: return code; break; case Result<i32, CollectionError>.Err: return 92; break; } }
]=])
entry_reject(inserted_reference_keeps_owner "Cannot modify, move, or exclusively borrow a place while it is borrowed|borrowed.*escaping" [=[
Result<i32, CollectionError> exercise() {
    mut i32 local = 7;
    mut Map<i32, &i32> owner = Map<i32, &i32>.new()?;
    {
        rsn entry = map_entry.entry_with(&mut owner, 1, 1u64, [](&i32 a, &i32 b) -> bool { return *a == *b; });
        rsn inserted = entry.or_insert(&local);
        if (!Result.is_ok(&inserted)) return Result.ok(90);
    }
    local = 9;
    i32 key = 1;
    rsn same = [](&i32 a, &i32 b) -> bool { return *a == *b; };
    Optional<& &i32> retained = owner.get_with(&key, 1u64, &same);
    switch (move retained) { case Optional<& &i32>.Some { & &i32 value = value }: return Result.ok(**value); break; case Optional<& &i32>.None: return Result.ok(91); break; }
}
i32 main() { Result<i32, CollectionError> result = exercise(); switch (move result) { case Result<i32, CollectionError>.Ok { i32 code = value }: return code; break; case Result<i32, CollectionError>.Err: return 92; break; } }
]=])

# 생성된 IR의 할당 기호만 바꿔 소스 코드의 알려진 효과·대여 계약을 보존한다.
# Redirect only emitted IR allocation symbols, preserving the source's known effect and loan contracts.
if(DEFINED CLANG AND EXISTS "${CLANG}" AND DEFINED RUNTIME AND EXISTS "${RUNTIME}")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/v03_map_entry_failure.avx" "${source_dir}/failure.avx" COPYONLY)
    entry_run("allocator failure source check" 0 "" --check "${source_dir}/failure.avx")
    execute_process(COMMAND "${AEVIXC}" --emit-llvm "${source_dir}/failure.avx"
        RESULT_VARIABLE emitted OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 180)
    if(NOT emitted EQUAL 0)
        message(FATAL_ERROR "Cannot emit allocator failure fixture: ${error}")
    endif()
    string(REPLACE "@malloc(" "@entry_test_allocate(" ir "${ir}")
    string(REPLACE "@free(" "@entry_test_free(" ir "${ir}")
    string(REPLACE "@main(" "@entry_test_main(" ir "${ir}")
    file(WRITE "${TEST_DIR}/failure.ll" "${ir}")
    file(WRITE "${TEST_DIR}/failure.c" [=[
#include <stddef.h>
#include <stdlib.h>
static int attempts;
static int frees;
void *entry_test_allocate(size_t size) { (void)size; attempts++; return NULL; }
void entry_test_free(void *pointer) { if (pointer) frees++; free(pointer); }
extern int entry_test_main(void);
int main(void) {
    int result = entry_test_main();
    if (result) return result;
    return attempts == 3 && frees == 0 ? 0 : 93;
}
]=])
    set(extra_libraries -pthread)
    if(APPLE)
        list(APPEND extra_libraries -lc++)
    elseif(UNIX)
        list(APPEND extra_libraries -lstdc++ -lm)
    endif()
    if(WIN32)
        set(failure_binary "${TEST_DIR}/failure.exe")
    else()
        set(failure_binary "${TEST_DIR}/failure")
    endif()
    execute_process(COMMAND "${CLANG}" "${TEST_DIR}/failure.ll" "${TEST_DIR}/failure.c"
        "${RUNTIME}" ${extra_libraries} -o "${failure_binary}"
        RESULT_VARIABLE linked OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
    if(NOT linked EQUAL 0)
        message(FATAL_ERROR "Cannot link deterministic allocator failure fixture: ${output}${error}")
    endif()
    execute_process(COMMAND "${failure_binary}" RESULT_VARIABLE ran OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
    if(NOT "${ran}" STREQUAL "0")
        message(FATAL_ERROR "Allocator failure recovery failed (${ran}): ${output}${error}")
    endif()
else()
    message(STATUS "MapEntry allocator-failure native gate requires CLANG and RUNTIME; normal source/ownership gates still ran.")
endif()
message(STATUS "MapEntry source methods, linear loans, exact Drop, and applicable allocator recovery gates passed.")
