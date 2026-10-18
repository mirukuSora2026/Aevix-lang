# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_builtin_result_inference_cli_tests.cmake
#
# Description :
#     내장 값 연산의 중첩 추론과 같은 이름의 사용자 모듈 분리를 검사한다.
#     Check nested inference of builtin value operations without intercepting user modules.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
function(builtin_run name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(builtin_accept name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    builtin_run("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    builtin_run("${name} AOT" 0 "" --run "${TEST_DIR}/${name}.avx")
    builtin_run("${name} AOT O2" 0 "" --run -O2 "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        builtin_run("${name} JIT" 0 "" --jit "${TEST_DIR}/${name}.avx")
        builtin_run("${name} JIT O2" 0 "" --jit -O2 "${TEST_DIR}/${name}.avx")
    endif()
endfunction()
function(builtin_reject name pattern source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    builtin_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()

builtin_accept(nested_values [=[
import encoding;
T identity<T>(T value) { return value; }
T success<T,E>(Result<T,E> result) {
    switch (move result) {
        case Result<T,E>.Ok { T found = value }: return found; break;
        case Result<T,E>.Err: Never.unreachable(); break;
    }
}
Result<Vector<T>,CollectionError> allocate<T>(usize count) {
    mut Vector<T> output = []?;
    Vector.reserve(&mut output,count)?;
    return Result.ok(output);
}
i32 main() {
    mut string text = success(string.from_view("abc"));
    Vector<byte> bytes = success(encoding.encode_ascii(string.view(&text)));
    Vector<byte> reserved = success(allocate<byte>(Slice.len(string.as_bytes(string.view(&text)))));
    if (identity(Vector.len(&bytes)) != 3usize) return 1;
    if (identity(Vector.capacity(&reserved)) < 3usize) return 2;
    if (identity(string.byte_len(string.view(&text))) != 3usize) return 3;
    if (identity(string.scalar_count(&text)) != 3usize) return 4;
    if (identity(string.is_empty(&text))) return 5;
    if (!identity(string.equals(string.view(&text),"abc"))) return 6;
    Tuple<i32,bool> pair = Tuple.make(7,true);
    if (identity(Tuple.len(&pair)) != 2usize || identity(Tuple.is_empty(&pair))) return 7;
    unit cleared = identity(string.clear(&mut text));
    if (!identity(string.is_empty(&text))) return 8;
    return 0;
}
]=])

# 사용자 모듈의 실제 반환형이 usize/string_view 등으로 대체되면 안 된다.
# Never replace actual user-module return types with usize/string_view builtin shapes.
set(shadow "${TEST_DIR}/shadow")
file(MAKE_DIRECTORY "${shadow}")
file(WRITE "${shadow}/string.avx" [=[
public i32 view(i32 value) { return value + 1; }
public i32 as_bytes(i32 value) { return value + 2; }
public i32 byte_len(i32 value) { return value + 3; }
public i32 scalar_count(i32 value) { return value + 4; }
public i32 is_empty(i32 value) { return value + 5; }
public i32 equals(i32 first,i32 second) { return first + second; }
public i32 clear(i32 value) { return value + 6; }
]=])
file(WRITE "${shadow}/Slice.avx" "public i32 len(i32 value) { return value + 7; }\n")
file(WRITE "${shadow}/Vector.avx" "public i32 len(i32 value) { return value + 8; } public i32 capacity(i32 value) { return value + 9; }\n")
file(WRITE "${shadow}/Tuple.avx" "public i32 len(i32 value) { return value + 10; } public i32 is_empty(i32 value) { return value + 11; }\n")
file(WRITE "${shadow}/main.avx" [=[
import string;
import Slice;
import Vector;
import Tuple;
T identity<T>(T value) { return value; }
i32 main() {
    if (identity(string.view(1)) != 2 || identity(string.as_bytes(1)) != 3) return 1;
    if (identity(string.byte_len(1)) != 4 || identity(string.scalar_count(1)) != 5) return 2;
    if (identity(string.is_empty(1)) != 6 || identity(string.equals(1,2)) != 3 || identity(string.clear(1)) != 7) return 3;
    if (identity(Slice.len(1)) != 8 || identity(Vector.len(1)) != 9 || identity(Vector.capacity(1)) != 10) return 4;
    if (identity(Tuple.len(1)) != 11 || identity(Tuple.is_empty(1)) != 12) return 5;
    return 0;
}
]=])
foreach(mode check run)
    builtin_run("shadow ${mode}" 0 "" --${mode} "${shadow}/main.avx")
endforeach()
builtin_run("shadow AOT O2" 0 "" --run -O2 "${shadow}/main.avx")
if(version MATCHES "ORC JIT enabled")
    builtin_run("shadow JIT" 0 "" --jit "${shadow}/main.avx")
    builtin_run("shadow JIT O2" 0 "" --jit -O2 "${shadow}/main.avx")
endif()

# 다른 모듈에서만 가져온 사용자 string은 현재 모듈의 내장 결정을 바꾸지 않는다.
# A user string imported only transitively cannot change this module's canonical builtin binding.
file(WRITE "${shadow}/holder.avx" "import string; public i32 value() { return string.view(1); }\n")
file(WRITE "${shadow}/transitive.avx" [=[
import holder;
T identity<T>(T value) { return value; }
i32 main() { return identity(string.byte_len(identity(string.view("abc")))) == 3usize && holder.value() == 2 ? 0 : 1; }
]=])
builtin_run("transitive check" 0 "" --check "${shadow}/transitive.avx")
builtin_run("transitive AOT" 0 "" --run "${shadow}/transitive.avx")
if(version MATCHES "ORC JIT enabled")
    builtin_run("transitive JIT" 0 "" --jit "${shadow}/transitive.avx")
endif()

builtin_reject(bad_text "Text operations require|requires" [=[
T identity<T>(T value) { return value; }
i32 main() { rsn invalid = identity(string.view(1)); return 0; }
]=])
builtin_reject(bad_slice "Slice.len requires" [=[
T identity<T>(T value) { return value; }
i32 main() { rsn invalid = identity(Slice.len(1)); return 0; }
]=])
builtin_reject(readonly_clear "exclusive|&mut" [=[
T identity<T>(T value) { return value; }
T success<T,E>(Result<T,E> input) { switch(move input) { case Result<T,E>.Ok { T value=value }: return value; break; default: Never.unreachable(); break; } }
i32 main() { string text=success(string.from_view("abc")); rsn invalid=identity(string.clear(&text)); return 0; }
]=])
message(STATUS "Builtin value result inference and namespace identity passed")
