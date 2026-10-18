# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_library_generic_pressure_cli_tests.cmake
#
# Description :
#     AST 할당 형태를 바꾸어도 올바른 제네릭 Vector 라이브러리 호출이 항상 검사·실행되는지 확인한다.
#     Verify valid generic Vector library calls always check and execute across varied AST allocations.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR OR NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "AEVIXC, TEST_DIR, and SOURCE_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE compiler_version TIMEOUT 10)
file(READ "${SOURCE_DIR}/tests/v03_library_vector.avx" fixture)
if(NOT fixture MATCHES "^import vector;")
    message(FATAL_ERROR "Expected the real Vector library fixture to import vector first")
endif()

function(require_pressure_success label)
    execute_process(COMMAND "${AEVIXC}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${result}" STREQUAL "0")
        message(FATAL_ERROR "${label}: valid source must always succeed; got ${result}: ${output}${error}")
    endif()
endfunction()

# 이전 호출 AST의 해제와 이후 제네릭 복제가 서로 다른 주소 재사용 패턴을 만들게 한다.
# Vary address reuse between erased call syntax and later generic clones.
foreach(count 0 1 2 4 8 16 32)
    set(declarations "")
    if(count GREATER 0)
        math(EXPR last "${count} - 1")
        foreach(index RANGE 0 ${last})
            string(APPEND declarations "i32 pressure_value_${index}() { Optional<i32> item = Optional.some(${index}); return Optional.is_some(&item) ? ${index} : -1; }\n")
        endforeach()
    endif()
    string(REPLACE "import vector;" "import vector;\n${declarations}" source "${fixture}")
    set(path "${TEST_DIR}/pressure_${count}.avx")
    file(WRITE "${path}" "${source}")
    require_pressure_success("AST pressure ${count} check" --check "${path}")
    require_pressure_success("AST pressure ${count} AOT" --run -O2 "${path}")
endforeach()

# 같은 이름의 프로젝트 모듈 함수가 내장 Vector 연산으로 바뀌면 안 된다.
# A same-named project-module function must not be rewritten as a builtin Vector operation.
set(namespace_dir "${TEST_DIR}/namespace")
file(MAKE_DIRECTORY "${namespace_dir}")
file(WRITE "${namespace_dir}/Vector.avx" [=[
public i32 reserve(i32 value) { return value + 1; }
public i32 reserve(&mut Vector<i32> values, usize count) { return 17; }
]=])
file(WRITE "${namespace_dir}/Slice.avx" [=[
public i32 slice(i32 value) { return value + 2; }
public i32 slice(i32 value, usize first, usize end) { return 31; }
]=])
file(WRITE "${namespace_dir}/string.avx" [=[
public i32 from_view(string_view text) { return 37; }
]=])
file(WRITE "${namespace_dir}/main.avx" [=[
import Vector;
import Slice;
import string;
Result<i32, CollectionError> exercise() {
    if (Vector.reserve(7) != 8) return Result.ok(1);
    mut Vector<i32> values = []?;
    i32 selected = Vector.reserve(&mut values, 7);
    if (selected != 17) return Result.ok(2);
    if (Slice.slice(7) != 9 || Slice.slice(7, 0, 2) != 31) return Result.ok(3);
    if (string.from_view("text") != 37) return Result.ok(4);
    return Result.ok(0);
}
i32 main() {
    Result<i32, CollectionError> checked = exercise();
    switch (move checked) {
        case Result<i32, CollectionError>.Ok { i32 code = value }: return code; break;
        case Result<i32, CollectionError>.Err: return 99; break;
    }
}
]=])
require_pressure_success("project Vector namespace check" --check "${namespace_dir}/main.avx")
require_pressure_success("project Vector namespace AOT" --run "${namespace_dir}/main.avx")

# 다른 모듈에서 호출해도 가져온 함수의 출처를 잃지 않아야 한다.
# Preserve imported function identity when reached through another module.
file(WRITE "${namespace_dir}/bridge.avx" [=[
import Vector;
import Slice;
import string;
public Result<i32, CollectionError> exercise() {
    mut Vector<i32> values = []?;
    i32 selected = Vector.reserve(&mut values, 7);
    if (Vector.reserve(7) != 8 || selected != 17) return Result.ok(5);
    if (Slice.slice(7) != 9 || Slice.slice(7, 0, 2) != 31) return Result.ok(6);
    if (string.from_view("text") != 37) return Result.ok(7);
    return Result.ok(0);
}
]=])
file(WRITE "${namespace_dir}/transitive.avx" [=[
import bridge;
i32 main() {
    Result<i32, CollectionError> checked = bridge.exercise();
    switch (move checked) {
        case Result<i32, CollectionError>.Ok { i32 code = value }: return code; break;
        case Result<i32, CollectionError>.Err: return 99; break;
    }
}
]=])
require_pressure_success("transitive project namespaces check" --check "${namespace_dir}/transitive.avx")
require_pressure_success("transitive project namespaces AOT" --run "${namespace_dir}/transitive.avx")

# 전이 의존성이 이름을 사용한다고 해서 진입 모듈의 내장 이름을 가리면 안 된다.
# A transitive dependency using a name must not shadow a builtin in the entry module.
set(visibility_dir "${TEST_DIR}/transitive_visibility")
file(MAKE_DIRECTORY "${visibility_dir}")
file(WRITE "${visibility_dir}/string.avx" [=[
public i32 from_view(string_view text) { return 37; }
]=])
file(WRITE "${visibility_dir}/Slice.avx" [=[
public i32 slice(i32 value) { return value + 2; }
]=])
file(WRITE "${visibility_dir}/holder.avx" [=[
import string;
import Slice;
public i32 read() { return string.from_view("custom") + Slice.slice(1); }
]=])
file(WRITE "${visibility_dir}/main.avx" [=[
import holder;
Result<i32, TextError> text_case() {
    string value = string.from_view("builtin")?;
    return Result.ok(string.equals(&value, "builtin") ? 0 : 1);
}
Result<i32, BoundsError> slice_case() {
    i32 values[3] = [1, 2, 3];
    Slice<i32> view = Slice.from_array(&values);
    Slice<i32> part = Slice.slice(view, 1, 3)?;
    return Result.ok(Slice.len(part) == 2 && part[0] + part[1] == 5 ? 0 : 2);
}
i32 main() {
    if (holder.read() != 40) return 3;
    Result<i32, TextError> text = text_case();
    switch (move text) {
        case Result<i32, TextError>.Ok { i32 code = value }: if (code != 0) return code; break;
        case Result<i32, TextError>.Err: return 4; break;
    }
    Result<i32, BoundsError> slice = slice_case();
    switch (move slice) {
        case Result<i32, BoundsError>.Ok { i32 code = value }: return code; break;
        case Result<i32, BoundsError>.Err: return 5; break;
    }
}
]=])
require_pressure_success("unimported transitive namespaces check" --check "${visibility_dir}/main.avx")
require_pressure_success("unimported transitive namespaces AOT" --run "${visibility_dir}/main.avx")

# 반환 타입 추론도 이름 철자가 아닌 실제 프로젝트 함수의 선언을 따라야 한다.
# Return-type deduction must follow the actual project declaration rather than builtin spelling.
set(deduction_dir "${TEST_DIR}/namespace_deduction")
file(MAKE_DIRECTORY "${deduction_dir}")
file(WRITE "${deduction_dir}/Byte.avx" [=[
public i32 from_u8(i32 value) { return value + 1; }
]=])
file(WRITE "${deduction_dir}/Slice.avx" [=[
public i32 from_array(i32 value) { return value + 2; }
public i32 len(i32 value) { return value + 17; }
]=])
file(WRITE "${deduction_dir}/Tuple.avx" [=[
public i32 make(i32 value) { return value + 3; }
]=])
file(WRITE "${deduction_dir}/Range.avx" [=[
public i32 new(i32 first, i32 second) { return first + second; }
]=])
file(WRITE "${deduction_dir}/Unit.avx" [=[
public i32 value() { return 7; }
]=])
file(WRITE "${deduction_dir}/Never.avx" [=[
public i32 unreachable() { return 23; }
]=])
file(WRITE "${deduction_dir}/Vector.avx" [=[
public i32 len(i32 value) { return value + 11; }
public i32 capacity(i32 value) { return value + 13; }
]=])
file(WRITE "${deduction_dir}/string.avx" [=[
public i32 from_view(string_view text) { return 9; }
]=])
file(WRITE "${deduction_dir}/main.avx" [=[
import Byte;
import Slice;
import Tuple;
import Range;
import Unit;
import Never;
import Vector;
import string;
T identity<T>(T value) { return value; }
i32 main() {
    if (Byte.from_u8(1) != 2) return 1;
    if (Slice.from_array(2) != 4) return 2;
    if (Tuple.make(3) != 6) return 3;
    if (Range.new(3, 4) != 7) return 4;
    if (Unit.value() != 7) return 5;
    if (string.from_view("x") != 9) return 6;
    if (identity(Byte.from_u8(1)) != 2) return 7;
    if (identity(Slice.from_array(2)) != 4) return 8;
    if (identity(Tuple.make(3)) != 6) return 9;
    if (identity(Range.new(3, 4)) != 7) return 10;
    if (identity(Unit.value()) != 7) return 11;
    if (identity(string.from_view("x")) != 9) return 12;
    if (identity<i32>(Byte.from_u8(1)) != 2) return 13;
    if (identity<i32>(Slice.from_array(2)) != 4) return 14;
    if (identity<i32>(Tuple.make(3)) != 6) return 15;
    if (identity<i32>(Range.new(3, 4)) != 7) return 16;
    if (identity<i32>(Unit.value()) != 7) return 17;
    if (identity<i32>(string.from_view("x")) != 9) return 18;
    if (Never.unreachable() != 23) return 19;
    if (identity(Never.unreachable()) != 23) return 20;
    if (identity<i32>(Never.unreachable()) != 23) return 21;
    if (Vector.len(1) != 12 || Vector.capacity(1) != 14 || Slice.len(1) != 18) return 22;
    if (identity(Vector.len(1)) != 12 || identity(Vector.capacity(1)) != 14 || identity(Slice.len(1)) != 18) return 23;
    if (identity<i32>(Vector.len(1)) != 12 || identity<i32>(Vector.capacity(1)) != 14 || identity<i32>(Slice.len(1)) != 18) return 24;
    return 0;
}
]=])
require_pressure_success("project namespace deduction check" --check "${deduction_dir}/main.avx")
require_pressure_success("project namespace deduction AOT" --run "${deduction_dir}/main.avx")
require_pressure_success("project namespace deduction AOT O2" --run -O2 "${deduction_dir}/main.avx")
if(compiler_version MATCHES "ORC JIT enabled")
    foreach(source "${namespace_dir}/main.avx" "${namespace_dir}/transitive.avx" "${visibility_dir}/main.avx" "${deduction_dir}/main.avx")
        require_pressure_success("project namespace JIT" --jit "${source}")
        require_pressure_success("project namespace JIT O2" --jit -O2 "${source}")
    endforeach()
endif()
message(STATUS "Generic source-library calls passed all seven AST allocation pressure variants")
