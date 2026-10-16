# ========================
#
# Created By : miruku_sora2026
#
# File name : const_generics_cli_tests.cmake
#
# Description :
#     상수 제네릭 추론·정규화, 후행 기본 타입과 값, 사용자 정의 배열 보관 타입을 실제로 실행한다.
#     Execute const-generic inference and normalization, trailing type/value defaults, and user-defined array-owning types.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR OR NOT DEFINED CLANG)
    message(FATAL_ERROR "AEVIXC, TEST_DIR and CLANG are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}/src")
function(run_case name expected)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE code
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
    if(NOT "${code}" STREQUAL "${expected}")
        message(FATAL_ERROR "${name}: exit ${code}, expected ${expected}\n${output}\n${error}")
    endif()
endfunction()
function(fixture name source)
    file(WRITE "${TEST_DIR}/src/${name}.avx" "${source}\n")
endfunction()
fixture(buffer [=[
struct Buffer<T = i32, const usize N = 4> {
    public Array<T, N> values;
    public usize len(&Self self) { return N; }
};
usize width<T, const usize N>(&Array<T, N> values) { return N; }
i32 main() {
    i32 data[4] = [1, 2, 3, 4];
    usize count = width(&data);
    Buffer buffer = Buffer { values = data };
    return count == 4 && buffer.len() == 4 ? 42 : 1;
}
]=])
fixture(defaults [=[
struct Pair<T = i32, U = T> { public T first; public U second; };
i32 answer<T = i32>() { return 42; }
usize size<const usize N = (2 + 2)>() { return N; }
T keep<T, const usize N = 4>(T value) { return value; }
i32 main() {
    Pair pair = Pair { first = 20, second = 22 };
    return pair.first + pair.second == 42 && answer() == 42 && answer<i64>() == 42 && size() == 4 && keep<i32>(42) == 42 ? 42 : 1;
}
]=])
fixture(canonical [=[
const usize COUNT = 2 * 2;
struct Buffer<T, const usize N> { public Array<T, N> values; };
usize len<T, const usize N>(&Buffer<T, N> buffer) { return N; }
usize direct<T, const usize N>(&Array<T, N> values) { return N; }
i32 main() {
    Buffer<i32, COUNT> a = Buffer<i32, 4u128> { values = [1, 2, 3, 4] };
    i32 values[4] = [1, 2, 3, 4];
    return len(&a) == 4 && direct<i32, COUNT>(&values) == 4 ? 42 : 1;
}
]=])
fixture(zero_extent [=[
struct Empty<const usize N = 0> { public i32 values[N]; };
usize len<T, const usize N>(&Array<T, N> values) { return N; }
i32 main() {
    Empty value = Empty { values = [] };
    return len(&value.values) == 0 ? 42 : 1;
}
]=])
fixture(suffix_inference [=[
usize size<T, const usize N>(T values[N]) { return N; }
i32 main() { i32 values[4] = [1, 2, 3, 4]; return size(values) == 4 ? 42 : 1; }
]=])
file(WRITE "${TEST_DIR}/src/foundation.avx" [=[
const usize DEFAULT_SIZE = 4;
public struct Buffer<T = i32, const usize N = DEFAULT_SIZE> { public Array<T, N> values; };
]=])
fixture(imported [=[
import foundation { Buffer };
i32 main() { Buffer value = Buffer { values = [20, 22, 0, 0] }; return value.values[0] + value.values[1]; }
]=])
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
foreach(name buffer defaults canonical zero_extent suffix_inference imported)
    run_case("${name} check" 0 --check "${TEST_DIR}/src/${name}.avx")
    foreach(level 0 2)
        run_case("${name} AOT O${level}" 42 --clang "${CLANG}" "-O${level}" "${TEST_DIR}/src/${name}.avx")
        if(version MATCHES "JIT enabled")
            run_case("${name} JIT O${level}" 42 --jit "-O${level}" "${TEST_DIR}/src/${name}.avx")
        endif()
    endforeach()
endforeach()
function(invalid name source)
    fixture("${name}" "${source}")
    run_case("${name}" 1 --check "${TEST_DIR}/src/${name}.avx")
endfunction()
invalid(required_after_default [=[struct A<T = i32, U> {}; i32 main() { return 0; }]=])
invalid(wrong_const_type [=[struct A<const i32 N> {}; i32 main() { A<4> value = A<4> {}; return 0; }]=])
invalid(negative_const [=[struct A<const usize N> {}; i32 main() { A<-1> value; return 0; }]=])
invalid(wide_const [=[struct A<const usize N> {}; i32 main() { A<18446744073709551616u128> value; return 0; }]=])
invalid(runtime_const [=[struct A<const usize N> {}; i32 main() { usize n = 4; A<n> value; return 0; }]=])
invalid(missing_const [=[struct A<T, const usize N> {}; i32 main() { A<i32> value; return 0; }]=])
invalid(value_for_type [=[struct A<T> {}; i32 main() { A<4> value; return 0; }]=])
invalid(cyclic_default [=[struct A<const usize N = N> {}; i32 main() { return 0; }]=])
message(STATUS "Const generic inference, defaults, canonical values, imported buffers and rejection cases passed")
