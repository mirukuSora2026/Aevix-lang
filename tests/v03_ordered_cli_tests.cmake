# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_ordered_cli_tests.cmake
#
# Description :
#     AVL 순서·회전·삭제·대여·이동 전용 정리를 실제 CPU 소스 호출로 검사한다.
#     Check AVL order, rotations, deletion, loans, and move-only cleanup through actual source calls.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
set(source_dir "${TEST_DIR}/sources")
file(MAKE_DIRECTORY "${source_dir}")
file(SHA256 "${AEVIXC}" compiler_sha)
message(STATUS "Ordered source gates use compiler SHA256 ${compiler_sha}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)

function(ordered_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 180)
    if(NOT "${result}" STREQUAL "${expected}" OR
       (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${result}\n${output}${error}")
    endif()
endfunction()

foreach(name collections ownership)
    configure_file("${CMAKE_CURRENT_LIST_DIR}/v03_ordered_${name}.avx" "${source_dir}/${name}.avx" COPYONLY)
    ordered_run("${name} check" 0 "" --check "${source_dir}/${name}.avx")
    ordered_run("${name} AOT O0" 0 "" --run -O0 "${source_dir}/${name}.avx")
    ordered_run("${name} AOT O2" 0 "" --run -O2 "${source_dir}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        ordered_run("${name} JIT O0" 0 "" --jit -O0 "${source_dir}/${name}.avx")
        ordered_run("${name} JIT O2" 0 "" --jit -O2 "${source_dir}/${name}.avx")
    endif()
endforeach()

function(ordered_reject name pattern source)
    file(WRITE "${source_dir}/${name}.avx" "${source}\n")
    ordered_run("${name}" 1 "${pattern}" --check "${source_dir}/${name}.avx")
endfunction()
ordered_reject(iterator_keeps_owner "Cannot modify, move, or exclusively borrow a place while it is borrowed" [=[
import ordered_map { OrderedMap, Iterator };
i32 main() {
    mut OrderedMap<i32, i32> owner = OrderedMap<i32, i32>.new();
    mut Iterator<i32, i32> cursor = owner.iter();
    owner.clear();
    cursor.next();
    return 0;
}
]=])
ordered_reject(value_keeps_owner "Cannot modify, move, or exclusively borrow a place while it is borrowed" [=[
import ordered_map { OrderedMap };
i32 main() {
    mut OrderedMap<i32, i32> owner = OrderedMap<i32, i32>.new();
    rsn compare = [](&i32 left, &i32 right) -> i32 { return *left < *right ? -1 : *left > *right ? 1 : 0; };
    i32 key = 0;
    Optional<&i32> value = owner.get_with(&key, &compare);
    owner.clear();
    switch (move value) { case Optional<&i32>.Some { &i32 item = value }: return *item; break; case Optional<&i32>.None: return 0; break; }
}
]=])
ordered_reject(keys_are_immutable "Cannot modify an immutable binding or a read-only borrow" [=[
import ordered_map { OrderedMap };
i32 main() {
    OrderedMap<i32, i32> owner = OrderedMap<i32, i32>.new();
    Optional<Tuple<&i32, &i32>> item = owner.first_key_value();
    switch (move item) {
        case Optional<Tuple<&i32, &i32>>.Some { Tuple<&i32, &i32> pair = value }: &i32 key = *Tuple.get<0>(&pair); *key = 3; break;
        case Optional<Tuple<&i32, &i32>>.None: break;
    }
    return 0;
}
]=])
ordered_reject(private_nodes "Cannot access private declaration 'ordered_map.Node'" [=[
import ordered_map;
i32 main() { ordered_map.Node<i32, i32> node; return 0; }
]=])
ordered_reject(iterator_cannot_escape "Cannot return.*local" [=[
import ordered_map { OrderedMap, Iterator };
Iterator<i32, i32> dangling() {
    OrderedMap<i32, i32> owner = OrderedMap<i32, i32>.new();
    return owner.iter();
}
i32 main() { return 0; }
]=])
# 두 번째 노드 할당만 실패시켜 기존 트리와 입력 소유자를 함께 보존하는지 검사한다.
# Reject only the second node allocation, preserving both the existing tree and incoming owners.
if(DEFINED CLANG AND EXISTS "${CLANG}" AND DEFINED RUNTIME AND EXISTS "${RUNTIME}")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/v03_ordered_failure.avx" "${source_dir}/ordered_failure_usage.avx" COPYONLY)
    ordered_run("node allocation failure source check" 0 "" --check "${source_dir}/ordered_failure_usage.avx")
    execute_process(COMMAND "${AEVIXC}" --emit-llvm "${source_dir}/ordered_failure_usage.avx"
        RESULT_VARIABLE emitted OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 180)
    if(NOT emitted EQUAL 0)
        message(FATAL_ERROR "Cannot emit ordered allocation failure fixture: ${error}")
    endif()
    string(REPLACE "@malloc(" "@ordered_test_allocate(" ir "${ir}")
    string(REPLACE "@free(" "@ordered_test_free(" ir "${ir}")
    string(REPLACE "@main(" "@ordered_test_main(" ir "${ir}")
    file(WRITE "${TEST_DIR}/ordered_failure.ll" "${ir}")
    file(WRITE "${TEST_DIR}/ordered_failure.c" [=[
#include <stddef.h>
#include <stdlib.h>
static int attempts, allocations, frees;
void *ordered_test_allocate(size_t size) {
    if (++attempts == 2) return NULL;
    void *pointer = malloc(size);
    if (pointer) allocations++;
    return pointer;
}
void ordered_test_free(void *pointer) { if (pointer) frees++; free(pointer); }
extern int ordered_test_main(void);
int main(void) {
    int result = ordered_test_main();
    if (result) return result;
    return attempts == 2 && allocations == 1 && frees == 1 ? 0 : 93;
}
]=])
    set(extra_libraries -pthread)
    if(APPLE)
        list(APPEND extra_libraries -lc++)
    elseif(UNIX)
        list(APPEND extra_libraries -lstdc++ -lm)
    endif()
    if(WIN32)
        set(failure_binary "${TEST_DIR}/ordered_failure.exe")
    else()
        set(failure_binary "${TEST_DIR}/ordered_failure")
    endif()
    execute_process(COMMAND "${CLANG}" "${TEST_DIR}/ordered_failure.ll" "${TEST_DIR}/ordered_failure.c"
        "${RUNTIME}" ${extra_libraries} -o "${failure_binary}"
        RESULT_VARIABLE linked OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
    if(NOT linked EQUAL 0)
        message(FATAL_ERROR "Cannot link ordered allocation failure fixture: ${output}${error}")
    endif()
    execute_process(COMMAND "${failure_binary}" RESULT_VARIABLE ran OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
    if(NOT "${ran}" STREQUAL "0")
        message(FATAL_ERROR "Ordered allocation failure recovery failed (${ran}): ${output}${error}")
    endif()
else()
    message(STATUS "Ordered node-allocation failure gate requires CLANG and RUNTIME; normal source/ownership gates still ran.")
endif()
message(STATUS "Ordered source AOT/JIT and ownership gates passed; the complete ordered catalog is a separate coverage target.")
