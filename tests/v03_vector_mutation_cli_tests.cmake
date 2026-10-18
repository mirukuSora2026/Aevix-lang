# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_vector_mutation_cli_tests.cmake
#
# Description :
#     동적 배열의 소유권 추출, 삽입 실패 복구, 정리와 대여 충돌을 검사한다.
#     Check vector owner extraction, insertion recovery, cleanup, and loan conflicts.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
function(vector_run name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(vector_accept name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    vector_run("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    vector_run("${name} AOT" 0 "" --run "${TEST_DIR}/${name}.avx")
    vector_run("${name} optimized AOT" 0 "" --run -O2 "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        vector_run("${name} JIT" 0 "" --jit "${TEST_DIR}/${name}.avx")
        vector_run("${name} optimized JIT" 0 "" --jit -O2 "${TEST_DIR}/${name}.avx")
    endif()
endfunction()
function(vector_reject name source pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    vector_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()

vector_accept(scalars [=[
T present<T>(Optional<T> input) {
    switch (move input) { case Optional<T>.Some { T value = value }: return value; break; case Optional<T>.None: Never.unreachable(); break; }
}
T require_bounds_value<T>(Result<T, BoundsError> input) {
    switch (move input) { case Result<T, BoundsError>.Ok { T value = value }: return value; break; case Result<T, BoundsError>.Err: Never.unreachable(); break; }
}
Result<i32, CollectionError> run() {
    mut Vector<i32> values = [1, 2, 3, 4]?;
    usize capacity = Vector.capacity(&values);
    if (present(Vector.pop(&mut values)) != 4 || Vector.len(&values) != 3) return Result.ok(1);
    if (require_bounds_value(Vector.remove(&mut values, 0)) != 1 || values[0] != 2 || values[1] != 3) return Result.ok(2);
    Result<i32, BoundsError> absent = Vector.remove(&mut values, 2);
    if (!Result.is_err(&absent) || Vector.len(&values) != 2) return Result.ok(3);
    Result<unit, InputError<i32>> inserted = Vector.insert(&mut values, 1, 9);
    if (Result.is_err(&inserted) || Vector.len(&values) != 3 || values[0] != 2 || values[1] != 9 || values[2] != 3) return Result.ok(4);
    if (require_bounds_value(Vector.swap_remove(&mut values, 0)) != 2 || values[0] != 3 || values[1] != 9) return Result.ok(5);
    Vector.truncate(&mut values, 99);
    if (Vector.len(&values) != 2) return Result.ok(6);
    Vector.truncate(&mut values, 1);
    if (Vector.len(&values) != 1 || values[0] != 3) return Result.ok(7);
    Vector.clear(&mut values);
    if (Vector.len(&values) != 0 || Vector.capacity(&values) != capacity) return Result.ok(8);
    Optional<i32> missing = Vector.pop(&mut values);
    if (!Optional.is_none(&missing)) return Result.ok(9);
    Result<unit, InputError<i32>> end = Vector.insert(&mut values, 0, 7);
    if (Result.is_err(&end) || values[0] != 7) return Result.ok(10);
    return Result.ok(0);
}
i32 main() { Result<i32, CollectionError> result = run(); switch (move result) { case Result<i32, CollectionError>.Ok { i32 code = value }: return code; break; case Result<i32, CollectionError>.Err: return 99; break; } }
]=])

vector_accept(owning_text [=[
string owned(string_view input) {
    Result<string, TextError> result = string.from_view(input);
    switch (move result) { case Result<string, TextError>.Ok { string value = value }: return value; break; case Result<string, TextError>.Err: Never.unreachable(); break; }
}
Result<i32, CollectionError> run() {
    mut Vector<string> values = [owned("first"), owned("second"), owned("third")]?;
    Result<unit, InputError<string>> invalid = Vector.insert(&mut values, 4, owned("unused"));
    switch (move invalid) {
        case Result<unit, InputError<string>>.Ok: return Result.ok(1); break;
        case Result<unit, InputError<string>>.Err { InputError<string> error = error }:
            switch (move error) {
                case InputError<string>.OutOfBounds { string recovered = input }: if (string.byte_len(&recovered) != 6) return Result.ok(2); break;
                case InputError<string>.AllocationFailure: return Result.ok(3); break;
                case InputError<string>.SizeOverflow: return Result.ok(4); break;
            }
            break;
    }
    if (Vector.len(&values) != 3 || string.byte_len(&values[1]) != 6) return Result.ok(5);
    Result<unit, InputError<string>> inserted = Vector.insert(&mut values, 1, owned("middle"));
    if (Result.is_err(&inserted) || string.byte_len(&values[2]) != 6) return Result.ok(6);
    Result<string, BoundsError> removed = Vector.remove(&mut values, 0);
    switch (move removed) { case Result<string, BoundsError>.Ok { string value = value }: if (string.byte_len(&value) != 5) return Result.ok(7); break; case Result<string, BoundsError>.Err: return Result.ok(8); break; }
    Optional<string> last = Vector.pop(&mut values);
    switch (move last) { case Optional<string>.Some { string value = value }: if (string.byte_len(&value) != 5) return Result.ok(9); break; case Optional<string>.None: return Result.ok(10); break; }
    Vector.truncate(&mut values, 0);
    return Result.ok(Vector.len(&values) == 0 ? 0 : 11);
}
i32 main() { Result<i32, CollectionError> result = run(); switch (move result) { case Result<i32, CollectionError>.Ok { i32 code = value }: return code; break; case Result<i32, CollectionError>.Err: return 99; break; } }
]=])

vector_accept(custom_drop [=[
mut i32 trace = 0;
struct Token implements Drop { public i32 id; public void drop(&mut Self self) { trace = trace * 10 + self.id; return; } };
Result<i32, CollectionError> run() {
    mut Vector<Token> values = [Token { id = 1 }, Token { id = 2 }, Token { id = 3 }, Token { id = 4 }]?;
    Vector.truncate(&mut values, 2);
    if (trace != 43) return Result.ok(1);
    {
        Result<Token, BoundsError> extracted = Vector.remove(&mut values, 0);
        if (trace != 43 || Vector.len(&values) != 1) return Result.ok(2);
        Vector.clear(&mut values);
        if (trace != 432) return Result.ok(3);
    }
    if (trace != 4321) return Result.ok(4);
    Vector.clear(&mut values);
    return Result.ok(trace == 4321 ? 0 : 5);
}
i32 main() { Result<i32, CollectionError> result = run(); switch (move result) { case Result<i32, CollectionError>.Ok { i32 code = value }: return code; break; case Result<i32, CollectionError>.Err: return 99; break; } }
]=])

vector_accept(reference_payload [=[
Result<i32, CollectionError> run() {
    mut i32 first = 1;
    mut i32 second = 2;
    mut Vector<&mut i32> values = [&mut first, &mut second]?;
    Optional<&mut i32> last = Vector.pop(&mut values);
    if (Vector.len(&values) != 1) return Result.ok(1);
    switch (move last) { case Optional<&mut i32>.Some { &mut i32 value = value }: *value = 9; break; case Optional<&mut i32>.None: return Result.ok(2); break; }
    Vector.clear(&mut values);
    return Result.ok(first == 1 && second == 9 ? 0 : 3);
}
i32 main() { Result<i32, CollectionError> result = run(); switch (move result) { case Result<i32, CollectionError>.Ok { i32 code = value }: return code; break; case Result<i32, CollectionError>.Err: return 99; break; } }
]=])

vector_reject(shared_receiver [=[Result<i32, CollectionError> run() { Vector<i32> values = []?; Vector.pop(&values); return Result.ok(0); } i32 main() { return 0; }]=] "exclusive|Result|type")
vector_reject(live_element_clear [=[Result<i32, CollectionError> run() { mut Vector<i32> values = [1]?; &i32 loan = &values[0]; Vector.clear(&mut values); return Result.ok(*loan); } i32 main() { return 0; }]=] "borrow|loan")
vector_reject(live_element_remove [=[Result<i32, CollectionError> run() { mut Vector<i32> values = [1]?; &i32 loan = &values[0]; Vector.remove(&mut values, 0); return Result.ok(*loan); } i32 main() { return 0; }]=] "borrow|loan")
vector_reject(live_element_insert [=[Result<i32, CollectionError> run() { mut Vector<i32> values = [1]?; &i32 loan = &values[0]; Vector.insert(&mut values, 0, 2); return Result.ok(*loan); } i32 main() { return 0; }]=] "borrow|loan")
vector_reject(short_insert [=[Result<i32, CollectionError> run() { i32 outer = 1; mut Vector<&i32> values = [&outer]?; { i32 inner = 2; Vector.insert(&mut values, 0, &inner); } return Result.ok(*values[0]); } i32 main() { return 0; }]=] "outlive|local|reference")
vector_reject(moved_input [=[Result<i32, CollectionError> run() { mut Vector<Vector<i32>> values = []?; Vector<i32> row = [1]?; Vector.insert(&mut values, 1, row); return Result.ok(row[0]); } i32 main() { return 0; }]=] "move|unavailable")
vector_reject(drop_global_loan [=[
mut i32 count = 0;
struct Item implements Drop { public void drop(&mut Self self) { count++; return; } };
Result<i32, CollectionError> run() { mut Vector<Item> values = [Item {}]?; &i32 loan = &count; Vector.clear(&mut values); return Result.ok(*loan); }
i32 main() { return 0; }
]=] "borrow|loan")
message(STATUS "Vector mutation ownership, bounds, cleanup, and recovery checks passed")
