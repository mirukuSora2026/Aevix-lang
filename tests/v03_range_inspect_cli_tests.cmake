# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_range_inspect_cli_tests.cmake
#
# Description :
#     원래 범위 경계·양의 간격·방향 조회와 모든 정수 폭의 빈 범위 의미를 검사한다.
#     Check original bounds, unsigned magnitudes, direction, and empty-range semantics at every integer width.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(header [=[
T success<T,E>(Result<T,E> input) {
    switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; default: Never.unreachable(); break; }
}
T identity<T>(T value) { return value; }
]=])
set(program "${header}")
set(calls "")
function(range_width type low high unsigned magnitude)
    set(body [=[
i32 inspect_@TYPE@() {
    @TYPE@ low = @LOW@;
    @TYPE@ high = @HIGH@;
    @UNSIGNED@ magnitude = @MAGNITUDE@;
    Range<@TYPE@> wide = success(Range.with_step(Range.closed(low,high),magnitude));
    if (identity(Range.start(&wide)) != low || identity(Range.end(range = &wide)) != high) return 1;
    @UNSIGNED@ step = Range.step_magnitude(&wide);
    if (step != magnitude || Range.step(&wide) != magnitude || !Range.is_inclusive(&wide) || Range.is_descending(&wide) || Range.is_empty(&wide)) return 2;
    mut i32 visits = 0;
    for (@TYPE@ current in wide) visits++;
    if (visits != 2 || Range.start(&wide) != low || Range.end(&wide) != high || Range.step_magnitude(&wide) != magnitude) return 3;
    Range<@TYPE@> reverse = success(Range.with_step(Range.descending_closed(high,low),magnitude));
    if (Range.start(&reverse) != high || Range.end(&reverse) != low || !Range.is_descending(&reverse) || !Range.is_inclusive(&reverse) || Range.is_empty(&reverse)) return 4;
    Range<@TYPE@> empty_up = Range.new(high,low);
    Range<@TYPE@> empty_down = Range.descending(low,high);
    if (!Range.is_empty(&empty_up) || !Range.is_empty(&empty_down) || Range.start(&empty_up) != high || Range.end(&empty_up) != low || Range.is_descending(&empty_up) || !Range.is_descending(&empty_down)) return 5;
    Range<@TYPE@> equal_open = Range.new(low,low);
    Range<@TYPE@> equal_closed = Range.closed(low,low);
    Range<@TYPE@> equal_reverse_open = Range.descending(high,high);
    Range<@TYPE@> equal_reverse_closed = Range.descending_closed(high,high);
    if (!Range.is_empty(&equal_open) || Range.is_empty(&equal_closed) || !Range.is_empty(&equal_reverse_open) || Range.is_empty(&equal_reverse_closed)) return 6;
    mut Range<@TYPE@> editable = Range.new(low,high);
    @TYPE@ saved = Range.start(&mut editable);
    editable = Range.new(high,low);
    return saved == low && Range.is_empty(&editable) ? 0 : 7;
}
]=])
    foreach(field TYPE LOW HIGH UNSIGNED MAGNITUDE)
        string(TOLOWER "${field}" variable)
        string(REPLACE "@${field}@" "${${variable}}" body "${body}")
    endforeach()
    set(program "${program}\n${body}" PARENT_SCOPE)
    set(calls "${calls}if (inspect_${type}() != 0) return ${test_index};\n" PARENT_SCOPE)
    math(EXPR next "${test_index} + 1")
    set(test_index "${next}" PARENT_SCOPE)
endfunction()
set(test_index 1)
range_width(i8 -128 127 u8 255)
range_width(u8 0 255 u8 255)
range_width(i16 -32768 32767 u16 65535)
range_width(u16 0 65535 u16 65535)
range_width(i32 -2147483648 2147483647 u32 4294967295)
range_width(u32 0 4294967295 u32 4294967295)
range_width(i64 -9223372036854775808 9223372036854775807 u64 18446744073709551615)
range_width(u64 0 18446744073709551615 u64 18446744073709551615)
range_width(i128 -170141183460469231731687303715884105728 170141183460469231731687303715884105727 u128 340282366920938463463374607431768211455)
range_width(u128 0 340282366920938463463374607431768211455 u128 340282366920938463463374607431768211455)
range_width(isize -9223372036854775808 9223372036854775807 usize 18446744073709551615)
range_width(usize 0 18446744073709551615 usize 18446744073709551615)
file(WRITE "${TEST_DIR}/all_widths.avx" "${program}\ni32 main(){${calls}return 0;}\n")
function(range_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(range_modes name path)
    range_run("${name} check" 0 "" --check "${path}" ${ARGN})
    foreach(opt -O0 -O2)
        range_run("${name} AOT ${opt}" 0 "" --run "${opt}" "${path}" ${ARGN})
        if(version MATCHES "ORC JIT enabled")
            range_run("${name} JIT ${opt}" 0 "" --jit "${opt}" "${path}" ${ARGN})
        endif()
    endforeach()
endfunction()
range_modes(all_widths "${TEST_DIR}/all_widths.avx")
file(WRITE "${TEST_DIR}/descriptor.avx" "${header}\n" [=[
i32 reusable(&mut Range<i32> view) {
    i32 first = Range.start(view);
    i32 last = Range.end(view);
    return first + last + (Range.is_empty(view) ? 1000 : 0);
}
i32 main() {
    mut Range<i32> projected = Range.new(2,5);
    if (reusable(&mut projected) != 7 || Range.end(&projected) != 5) return 5;
    Range<i32> original = success(Range.with_step(0..=10,4u32));
    mut i32 visits = 0;
    mut i32 last = 0;
    for (i32 current in original) { visits++; last = current; }
    if (visits != 3 || last != 8 || Range.start(&original) != 0 || Range.end(&original) != 10 || Range.step(&original) != 4u32 || Range.is_empty(&original)) return 1;
    for (i32 current in original) { break; }
    if (Range.start(&original) != 0 || Range.end(&original) != 10) return 2;
    Range<i32> copy = original;
    for (i32 current in copy) { continue; }
    if (Range.start(&copy) != 0 || Range.is_empty(&copy)) return 3;
    Range<i32> half = 0..10;
    return !Range.is_inclusive(&half) && !Range.is_descending(&half) && Range.step(&half) == 1u32 ? 0 : 4;
}
]=])
range_modes(descriptor "${TEST_DIR}/descriptor.avx")
function(range_reject name expression pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${header}\ni32 main(){Range<i32> value=Range.new(0,3);rsn invalid=${expression};return 0;}\n")
    range_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()
range_reject(no_borrow "Range.start(value)" "Range|borrow")
range_reject(wrong_input "Range.is_empty(&7)" "Range|borrow|place")
range_reject(no_input "Range.end()" "Range|argument")
range_reject(extra_input "Range.step(&value,&value)" "Range|argument")
range_reject(explicit_types "Range.start<i32>(&value)" "Range|type|argument")
range_reject(namespace_types "Range<i32>.start(&value)" "Range|type|argument|Unsupported")
range_reject(extra_namespace_types "Range<i32,i64>.start(&value)" "Range|type|argument|Unsupported")
range_reject(wrong_name "Range.end(other=&value)" "Range|argument")
file(WRITE "${TEST_DIR}/signed_magnitude.avx" "i32 main(){Range<i32> value=Range.new(0,3);i32 wrong=Range.step_magnitude(&value);return 0;}\n")
range_run("signed magnitude rejected" 1 "requires i32.*received u32" --check "${TEST_DIR}/signed_magnitude.avx")
set(shadow "${TEST_DIR}/shadow")
file(MAKE_DIRECTORY "${shadow}")
file(WRITE "${shadow}/Range.avx" "public i32 start<T>(i32 input){return input+1;}public i32 end(i32 input){return input+2;}\n")
file(WRITE "${shadow}/direct.avx" "import Range;T identity<T>(T value){return value;}i32 relay<T>(i32 input){return Range.start<T>(input);}i32 main(){return identity(Range.start<i32>(41))==42 && relay<i32>(41)==42 && identity(Range.end(40))==42?0:1;}\n")
range_modes(user_module "${shadow}/direct.avx" --module-root "${shadow}")
file(WRITE "${shadow}/holder.avx" "import Range;public i32 value(){return Range.start<i32>(41);}\n")
file(WRITE "${shadow}/transitive.avx" "import holder;i32 main(){Range<i32> value=Range.new(7,10);return Range.start(&value)==7 && Range.end(&value)==10 && !Range.is_empty(&value) && holder.value()==42?0:1;}\n")
range_modes(transitive_only "${shadow}/transitive.avx" --module-root "${shadow}")
message(STATUS "Range descriptor inspection passed every integer width, original-bound preservation, and namespace controls")
