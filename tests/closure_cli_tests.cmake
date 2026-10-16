# ========================
#
# Created By : miruku_sora2026
#
# File name : closure_cli_tests.cmake
#
# Description :
#     명시적 캡처와 정적 호출 및 제네릭 Fn 계약을 실행하고 대여·이동 오류를 검사한다.
#     Execute explicit captures and generic Fn contracts, checking borrow and move violations.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
file(WRITE "${TEST_DIR}/main.avx" [=[
T map<T, F>(T value, &F callback) where F implements Fn<Tuple<T>, T> {
    return callback(value);
}
i32 update<F>(&mut F callback, i32 value) where F implements FnMut<Tuple<i32>, i32> {
    return callback(value);
}
usize consume<F>(F callback, usize value) where F implements FnOnce<Tuple<usize>, usize> {
    return callback(value);
}
Result<usize, TextError> owned_case() {
    string text = string.from_view("abc")?;
    rsn once = [text](usize extra) -> usize { return string.byte_len(&text) + extra; };
    return Result.ok(consume(once, 2));
}
i32 main() {
    i32 factor = 3;
    rsn multiply = [factor](i32 value) -> i32 { return value * factor; };
    if (map(4, &multiply) != 12 || multiply(2) != 6) return 1;
    i32 base = 10;
    rsn shared = [&base](i32 value) -> i32 { return value + base; };
    if (map(2, &shared) != 12) return 2;
    mut i32 total = 1;
    mut rsn change = [&mut total](i32 delta) -> i32 { total += delta; return total; };
    if (update(&mut change, 2) != 3 || update(&mut change, 4) != 7) return 3;
    if (total != 7) return 4;
    rsn identity = [](i32 input) -> i32 { return input; };
    if (map(9, &identity) != 9) return 5;
    i32 data[2] = [8, 9];
    rsn array_capture = [data](i32 input) -> i32 { return data[0] + input; };
    if (array_capture(1) != 9 || array_capture(2) != 10) return 6;
    mut i32 original = 1;
    mut rsn private_counter = [original]() -> i32 { original += 1; return original; };
    if (private_counter() != 2 || private_counter() != 3 || original != 1) return 10;
    i32 read = 4;
    mut i32 write = 1;
    mut rsn mixed = [&read, &mut write](i32 value) -> i32 { write += read + value; return write; };
    if (mixed(2) != 7) return 11;
    rsn nested = [factor](i32 value) -> i32 {
        rsn inner = [factor](i32 input) -> i32 { return factor + input; };
        return inner(value);
    };
    if (nested(4) != 7) return 12;
    Result<usize, TextError> owned = owned_case();
    switch (move owned) {
        case Result<usize, TextError>.Ok { usize length = value }: if (length != 5) return 7; break;
        case Result<usize, TextError>.Err: return 8; break;
    }
    return 0;
}
]=])
function(run_closure name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${result}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${result}: ${output}${error}")
    endif()
endfunction()
run_closure(check 0 "" --check "${TEST_DIR}/main.avx")
run_closure(air 0 "Closure|__invoke" --emit-air "${TEST_DIR}/main.avx")
run_closure(run 0 "" --run "${TEST_DIR}/main.avx")
run_closure(run_optimized 0 "" --run -O2 "${TEST_DIR}/main.avx")
file(WRITE "${TEST_DIR}/noheap.avx" "i32 main() { i32 offset = 3; rsn callback = [offset](i32 value) -> i32 { return value + offset; }; return callback(4) == 7 ? 0 : 1; }\n")
execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/noheap.avx" RESULT_VARIABLE noheap_result OUTPUT_VARIABLE noheap_ir ERROR_VARIABLE noheap_error TIMEOUT 30)
if(NOT noheap_result EQUAL 0 OR "${noheap_ir}" MATCHES "@malloc|@calloc|@realloc")
    message(FATAL_ERROR "Scalar closure introduced hidden heap allocation: ${noheap_error}")
endif()
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
if(version MATCHES "ORC JIT enabled")
    run_closure(jit 0 "" --jit "${TEST_DIR}/main.avx")
endif()
file(WRITE "${TEST_DIR}/member_and_shared.avx" [=[
struct Holder<F> { public F callback; };
struct Point implements Copy {
    public i32 x;
    public i32 read(&Self self) { return self.x; }
    public void write(&mut Self self, i32 value) { self.x = value; return; }
};
i32 invoke<F>(&F callback, i32 value) where F implements Fn<Tuple<i32>, i32> { return callback(value); }
i32 stored<F>(F callback) where F implements Fn<Tuple<i32>, i32> {
    Holder<F> holder = Holder<F> { callback = callback };
    return holder.callback(2) == 42 && (holder.callback)(1) == 41 ? 0 : 1;
}
i32 main() {
    Point point = Point { x = 40 };
    rsn callback = [point](i32 delta) -> i32 { return point.read() + delta; };
    if (stored(callback) != 0) return 1;
    mut rsn update = [point](i32 value) -> i32 { point.write(value); return point.read(); };
    if (update(42) != 42) return 3;
    rsn shared = [point](i32 delta) -> i32 { return point.read() + delta; };
    return invoke(&shared, 2) == 42 ? 0 : 2;
}
]=])
run_closure(member_shared_check 0 "" --check "${TEST_DIR}/member_and_shared.avx")
run_closure(member_shared_run 0 "" --run "${TEST_DIR}/member_and_shared.avx")
run_closure(member_shared_optimized 0 "" --run -O2 "${TEST_DIR}/member_and_shared.avx")
if(version MATCHES "ORC JIT enabled")
    run_closure(member_shared_jit 0 "" --jit "${TEST_DIR}/member_and_shared.avx")
endif()
file(WRITE "${TEST_DIR}/mutable_receiver_immutable_callback.avx" "struct Point implements Copy { public i32 x; public void write(&mut Self self, i32 value) { self.x = value; return; } }; i32 main() { Point point = Point { x = 1 }; rsn callback = [point](i32 value) -> i32 { point.write(value); return value; }; return callback(2); }\n")
run_closure(mutable_receiver_immutable_callback 1 "overload|mutab|borrow|match" --check "${TEST_DIR}/mutable_receiver_immutable_callback.avx")
file(WRITE "${TEST_DIR}/uncaptured.avx" "i32 main() { i32 outside = 3; rsn callback = [](i32 value) -> i32 { return value + outside; }; return callback(1); }\n")
run_closure(uncaptured 1 "Unknown|capture|resolve|name" --check "${TEST_DIR}/uncaptured.avx")
file(WRITE "${TEST_DIR}/shared_write.avx" "i32 main() { mut i32 outside = 3; mut rsn callback = [&outside](i32 value) -> i32 { outside += value; return outside; }; return callback(1); }\n")
run_closure(shared_write 1 "writ|mutab|borrow|modif" --check "${TEST_DIR}/shared_write.avx")
file(WRITE "${TEST_DIR}/immutable_callback.avx" "i32 main() { mut i32 outside = 3; rsn callback = [&mut outside](i32 value) -> i32 { outside += value; return outside; }; return callback(1); }\n")
run_closure(immutable_callback 1 "overload|mutab|borrow|match" --check "${TEST_DIR}/immutable_callback.avx")
file(WRITE "${TEST_DIR}/capture_conflict.avx" "i32 main() { mut i32 outside = 3; rsn callback = [&outside](i32 value) -> i32 { return outside + value; }; outside = 8; return callback(1); }\n")
run_closure(capture_conflict 1 "borrow|loan|modif" --check "${TEST_DIR}/capture_conflict.avx")
file(WRITE "${TEST_DIR}/once_twice.avx" "Result<i32, TextError> bad() { string text = string.from_view(\"owned\")?; rsn callback = [text](i32 value) -> i32 { return value; }; i32 first = callback(1); return Result.ok(callback(2) + first); } i32 main() { return 0; }\n")
run_closure(once_twice 1 "moved|unavailable|ownership" --check "${TEST_DIR}/once_twice.avx")
file(WRITE "${TEST_DIR}/once_escape.avx" "Result<i32, TextError> bad() { string text = string.from_view(\"owned\")?; rsn callback = [text]() -> &string { return &text; }; &string dangling = callback(); return Result.ok(0); } i32 main() { return 0; }\n")
run_closure(once_escape 1 "local|outlive|reference|borrow" --check "${TEST_DIR}/once_escape.avx")
file(WRITE "${TEST_DIR}/bad_contract.avx" "i32 twice<F>(F callback) where F implements FnOnce<Tuple<i32>, i32> { i32 first = callback(1); return callback(2) + first; } i32 main() { return 0; }\n")
run_closure(bad_contract 1 "moved|move|consume" --check "${TEST_DIR}/bad_contract.avx")
file(WRITE "${TEST_DIR}/parallel_capture.avx" "import parallel; i32 main() { mut i32 values[2] = [0; 2]; i32 offset = 2; rsn callback = [offset](&mut i32 item) -> void { *item += offset; return; }; parallel.for_each(Slice.from_array_mut(&mut values), callback); return 0; }\n")
run_closure(parallel_capture 1 "worker|function" --check "${TEST_DIR}/parallel_capture.avx")
message(STATUS "Closures, callable contracts, and lifetime checks passed")
