# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_library_cli_tests.cmake
#
# Description :
#     실제 Aevix 라이브러리 함수를 AOT·JIT로 실행하고 수명·복사·콜백 계약을 검사한다.
#     Execute actual Aevix library functions with AOT/JIT and check lifetime, copy, and callback contracts.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR OR NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "AEVIXC, TEST_DIR, and SOURCE_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)

function(run_library label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${result}" STREQUAL "${expected}" OR
        (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${result}: ${output}${error}")
    endif()
endfunction()

foreach(name core algorithm vector ownership mutation slice)
    set(source "${SOURCE_DIR}/tests/v03_library_${name}.avx")
    run_library("${name} check" 0 "" --check "${source}")
    run_library("${name} AOT" 0 "" --run "${source}")
    run_library("${name} AOT O2" 0 "" --run -O2 "${source}")
    if(version MATCHES "ORC JIT enabled")
        run_library("${name} JIT" 0 "" --jit "${source}")
        run_library("${name} JIT O2" 0 "" --jit -O2 "${source}")
    endif()
endforeach()

function(reject_library name source pattern)
    set(path "${TEST_DIR}/${name}.avx")
    file(WRITE "${path}" "${source}\n")
    run_library("reject ${name}" 1 "${pattern}" --check "${path}")
endfunction()

reject_library(optional_borrow_escape [=[
import optional;
Optional<&i32> escape() {
    Optional<i32> local = Optional.some(1);
    return optional.as_ref(&local);
}
i32 main() { return 0; }
]=] "local|borrow|reference|loan")

reject_library(result_borrow_escape [=[
import result;
Result<&i32, &i32> escape() {
    Result<i32, i32> local = Result.ok(1);
    return result.as_ref(&local);
}
i32 main() { return 0; }
]=] "local|borrow|reference|loan")

reject_library(optional_copy_owning [=[
import optional;
struct Item { public i32 value; };
i32 main() {
    Optional<Item> input = Optional.some(Item { value = 1 });
    Optional<Item> duplicate = optional.copy(&input);
    return 0;
}
]=] "Copy|implements|requirements")

reject_library(result_copy_owning_error [=[
import result;
struct Item { public i32 value; };
i32 main() {
    Result<i32, Item> input = Result.err(Item { value = 1 });
    Result<i32, Item> duplicate = result.copy(&input);
    return 0;
}
]=] "Copy|implements|requirements")

reject_library(callback_wrong_argument [=[
import optional;
i32 main() {
    Optional<i32> input = Optional.some(1);
    Optional<i32> output = optional.map<i32, i32>(input, [](bool item) -> i32 { return item ? 1 : 0; });
    return 0;
}
]=] "implements|requirements|matching|argument")

reject_library(callback_moved_twice [=[
import optional;
i32 main() {
    rsn callback = [](i32 item) -> i32 { return item; };
    Optional<i32> first = Optional.some(1);
    Optional<i32> second = Optional.some(2);
    Optional<i32> mapped_first = optional.map<i32, i32>(first, callback);
    Optional<i32> mapped_second = optional.map<i32, i32>(second, callback);
    return 0;
}
]=] "moved|unavailable|ownership")

reject_library(sort_owning [=[
import algorithm;
struct Item { public i32 value; };
i32 main() {
    mut Item items[2] = [Item { value = 2 }, Item { value = 1 }];
    rsn compare = [](&Item left, &Item right) -> i32 { return left.value < right.value ? -1 : 1; };
    algorithm.sort_by(Slice.from_array_mut(&mut items), &compare);
    return 0;
}
]=] "Copy|implements|requirements")

reject_library(optional_insert_escape [=[
import optional;
&mut i32 escape() {
    mut Optional<i32> local = Optional.none();
    return optional.insert(&mut local, 1);
}
i32 main() { return 0; }
]=] "local|borrow|reference|loan")

reject_library(optional_insert_alias [=[
import optional;
i32 main() {
    mut Optional<i32> owner = Optional.some(1);
    &mut i32 view = optional.get_or_insert(&mut owner, 2);
    Optional<i32> extracted = optional.take(&mut owner);
    return *view;
}
]=] "borrow|reference|loan|mutable")

reject_library(slice_element_escape [=[
import slice;
Optional<&i32> escape() {
    i32 values[1] = [1];
    return slice.first(Slice.from_array(&values));
}
i32 main() { return 0; }
]=] "local|borrow|reference|loan")

reject_library(array_copy_owner [=[
import array;
struct Owner { public i32 item; };
i32 main() {
    Owner values[1] = [Owner { item = 1 }];
    Owner duplicate[1] = array.copy(&values);
    return 0;
}
]=] "Copy|implements|requirements")

reject_library(array_intrinsic_copy_owner [=[
struct Owner { public i32 item; };
i32 main() {
    Owner values[1] = [Owner { item = 1 }];
    Owner duplicate[1] = Array.from_copy(&values);
    return 0;
}
]=] "Copy")

reject_library(array_intrinsic_named_argument [=[
i32 main() {
    i32 values[1] = [1];
    i32 duplicate[1] = Array.from_copy(unknown = &values);
    return 0;
}
]=] "positional")

message(STATUS "Source library branches, ownership, and rejection contracts passed")
