# ========================
#
# Created By : miruku_sora2026
#
# File name : slice_cli_tests.cmake
#
# Description :
#     검사된 부분 뷰의 AOT·JIT 결과와 경계 오류 및 대여 수명 제한을 확인한다.
#     Check AOT/JIT subviews, bound errors, and borrowed lifetime restrictions.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
file(WRITE "${TEST_DIR}/main.avx" [=[
Result<i32, BoundsError> read_views() {
    i32 values[4] = [1, 2, 3, 4];
    Slice<i32> full = Slice.from_array(&values);
    Slice<i32> middle = Slice.slice(full, 1, 3)?;
    Slice<i32> empty = Slice.slice(Slice.from_array(&values), 4, 4)?;
    if (Slice.len(middle) != 2 || Slice.len(empty) != 0) return Result.ok(1);
    return Result.ok(middle[0] + middle[1] == 5 ? 0 : 2);
}
Result<i32, BoundsError> write_views() {
    mut i32 values[4] = [1, 2, 3, 4];
    {
        MutSlice<i32> full = Slice.from_array_mut(&mut values);
        MutSlice<i32> middle = Slice.slice_mut(full, 1, 3)?;
        middle[0] = 20;
        middle[1] = 30;
    }
    return Result.ok(values[1] + values[2] == 50 ? 0 : 3);
}
i32 check_error(bool reversed) {
    i32 values[4] = [1, 2, 3, 4];
    Slice<i32> full = Slice.from_array(&values);
    usize start = reversed ? 3 : 0;
    usize end = reversed ? 1 : 5;
    Result<Slice<i32>, BoundsError> result = Slice.slice(full, start, end);
    switch (move result) {
        case Result<Slice<i32>, BoundsError>.Ok: return 4; break;
        case Result<Slice<i32>, BoundsError>.Err { BoundsError problem = error }:
            switch (problem) {
                case BoundsError.OutOfBounds: return reversed ? 5 : 0; break;
                case BoundsError.ReversedRange: return reversed ? 0 : 6; break;
            }
            break;
    }
}
i32 main() {
    Result<i32, BoundsError> first = read_views();
    Result<i32, BoundsError> second = write_views();
    if (!Result.is_ok(&first) || !Result.is_ok(&second)) return 7;
    switch (move first) {
        case Result<i32, BoundsError>.Ok { i32 value = value }: if (value != 0) return value; break;
        case Result<i32, BoundsError>.Err: return 8; break;
    }
    switch (move second) {
        case Result<i32, BoundsError>.Ok { i32 value = value }: if (value != 0) return value; break;
        case Result<i32, BoundsError>.Err: return 9; break;
    }
    return check_error(false) + check_error(true);
}
]=])
function(run_slice name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
    if(NOT "${result}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${result}: ${output}${error}")
    endif()
endfunction()
run_slice(check 0 "" --check "${TEST_DIR}/main.avx")
run_slice(ir 0 "slice|extractvalue" --emit-llvm "${TEST_DIR}/main.avx")
run_slice(run 0 "" --run "${TEST_DIR}/main.avx")
run_slice(run_optimized 0 "" --run -O2 "${TEST_DIR}/main.avx")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
if(version MATCHES "ORC JIT enabled")
    run_slice(jit 0 "" --jit "${TEST_DIR}/main.avx")
endif()
file(WRITE "${TEST_DIR}/escape.avx" "Result<Slice<i32>, BoundsError> bad() { i32 values[2] = [1, 2]; return Slice.slice(Slice.from_array(&values), 0, 1); } i32 main() { return 0; }\n")
run_slice(escape 1 "local|reference|borrow" --check "${TEST_DIR}/escape.avx")
file(WRITE "${TEST_DIR}/conflict.avx" "Result<i32, BoundsError> bad() { mut i32 values[2] = [1, 2]; Slice<i32> part = Slice.slice(Slice.from_array(&values), 0, 1)?; values[0] = 4; return Result.ok(part[0]); } i32 main() { return 0; }\n")
run_slice(conflict 1 "borrow|loan|conflict" --check "${TEST_DIR}/conflict.avx")
file(WRITE "${TEST_DIR}/moved.avx" "Result<i32, BoundsError> bad() { mut i32 values[2] = [1, 2]; MutSlice<i32> full = Slice.from_array_mut(&mut values); MutSlice<i32> part = Slice.slice_mut(full, 0, 1)?; full[0] = 4; return Result.ok(part[0]); } i32 main() { return 0; }\n")
run_slice(moved 1 "moved|live|ownership|borrow" --check "${TEST_DIR}/moved.avx")
file(WRITE "${TEST_DIR}/reallocation.avx" "Result<i32, CollectionError> bad() { mut i32 values[] = [1, 2]?; Slice<i32> full = Slice.from_vector(&values); Result<Slice<i32>, BoundsError> part = Slice.slice(full, 0, 1); Vector.reserve(&mut values, 50)?; if (Result.is_ok(&part)) return Result.ok(1); return Result.ok(0); } i32 main() { return 0; }\n")
run_slice(reallocation 1 "borrow|loan|conflict" --check "${TEST_DIR}/reallocation.avx")
message(STATUS "Checked Slice subviews and lifetime checks passed")
