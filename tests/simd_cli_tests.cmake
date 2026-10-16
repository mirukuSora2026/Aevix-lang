# ========================
#
# Created By : miruku_sora2026
#
# File name : simd_cli_tests.cmake
#
# Description :
#     실제 CPU SIMD의 AOT·JIT 실행, 마스크, 경계 및 오버플로 정지와 이식 가능한 객체 출력을 검사한다.
#     Verify real CPU SIMD AOT/JIT execution, masks, bounds and overflow traps, and portable object emission.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR OR NOT DEFINED CLANG)
    message(FATAL_ERROR "AEVIXC, TEST_DIR and CLANG are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}/src")
function(run_case label expected)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} WORKING_DIRECTORY "${TEST_DIR}"
        RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
    if(NOT "${code}" STREQUAL "${expected}")
        message(FATAL_ERROR "${label}: exit ${code}, expected ${expected}\n${output}\n${error}")
    endif()
endfunction()
function(fixture name text)
    file(WRITE "${TEST_DIR}/src/${name}.avx" "${text}\n")
endfunction()
fixture(arithmetic [=[
import simd { Simd, SimdMask };
const usize LANES = 4;
i32 main() {
    i32 values[4] = [1, 2, 3, 4];
    Simd<i32, LANES> a = Simd.from_array(&values);
    Simd<i32, 4> b = Simd.splat<i32, LANES>(8);
    return Simd.sum(Simd.add(a, b));
}
]=])
fixture(mask_store [=[
import simd { Simd, SimdMask };
i32 main() {
    i32 values[4] = [1, 2, 3, 4];
    Simd<i32, 4> a = Simd.from_array(&values);
    Simd<i32, 4> b = Simd.mul(a, Simd.splat<i32, 4>(2));
    SimdMask<4> mask = Simd.gt(b, Simd.splat<i32, 4>(4));
    Simd<i32, 4> selected = Simd.select(mask, b, Simd.splat<i32, 4>(1));
    mut i32 output[5] = [0, 0, 0, 0, 0];
    Simd.store(&mut output, 1, selected);
    Simd<i32, 4> loaded = Simd.load<i32, 4>(&output, 1);
    i32 copy[4] = Simd.to_array(loaded);
    bool complete = SimdMask.all(SimdMask.or(mask, SimdMask.not(mask)));
    bool mixed = SimdMask.any(mask) && !SimdMask.all(mask);
    return complete && mixed && copy[0] == 1 && copy[3] == 8 && Simd.sum(loaded) == 16 ? 42 : 1;
}
]=])
fixture(float_ops [=[
import simd { Simd, SimdMask };
i32 main() {
    f32 values[4] = [1.0, 4.0, 9.0, 16.0];
    Simd<f32, 4> roots = Simd.sqrt(Simd.from_array(&values));
    Simd<f32, 4> halves = Simd.div(roots, Simd.splat<f32, 4>(2.0));
    return Simd.sum(halves) == 5.0 && Simd.extract(roots, 3) == 4.0 ? 42 : 1;
}
]=])
fixture(nan_masks [=[
import simd { Simd, SimdMask };
i32 main() {
    Simd<f32, 4> invalid = Simd.sqrt(Simd.splat<f32, 4>(-1.0));
    return SimdMask.all(Simd.ne(invalid, invalid)) && !SimdMask.any(Simd.eq(invalid, invalid)) ? 42 : 1;
}
]=])
fixture(qualified [=[
import simd;
i32 main() {
    simd.Simd<i32, 4> value = simd.Simd.splat<i32, 4>(10);
    return simd.Simd.sum(value) + 2;
}
]=])
fixture(small_lanes [=[
import simd { Simd };
i32 main() {
    Simd<i8, 16> bytes = Simd.splat<i8, 16>(1);
    i8 total = Simd.sum(bytes);
    Simd<u16, 8> wide = Simd.splat<u16, 8>(2);
    return total == 16 && Simd.sum(wide) == 16 ? 42 : 1;
}
]=])
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
foreach(name arithmetic mask_store float_ops nan_masks qualified small_lanes)
    foreach(level 0 2)
        run_case("${name} AOT O${level}" 42 --clang "${CLANG}" "-O${level}" "${TEST_DIR}/src/${name}.avx")
        if(version MATCHES "JIT enabled")
            run_case("${name} JIT O${level}" 42 --jit "-O${level}" "${TEST_DIR}/src/${name}.avx")
        endif()
    endforeach()
endforeach()
execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/src/arithmetic.avx"
    RESULT_VARIABLE code OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 30)
if(NOT "${code}" STREQUAL "0" OR NOT ir MATCHES "<4 x i32>" OR NOT ir MATCHES "llvm.sadd.with.overflow.v4i32")
    message(FATAL_ERROR "SIMD did not emit real checked LLVM vector operations\n${ir}\n${error}")
endif()
foreach(target arm64-apple-macosx aarch64-unknown-linux-gnu x86_64-unknown-linux-gnu x86_64-pc-windows-msvc)
    run_case("SIMD object ${target}" 0 --build -c -O2 --clang "${CLANG}" --target "${target}"
        -o "${TEST_DIR}/${target}.o" "${TEST_DIR}/src/arithmetic.avx")
endforeach()
set(trap_names overflow_add overflow_sub overflow_mul overflow_sum division_zero division_min load_bounds store_bounds extract_bounds)
set(trap_expressions
    "Simd.add(Simd.splat<i8, 2>(127), Simd.splat<i8, 2>(1))"
    "Simd.sub(Simd.splat<u8, 2>(0), Simd.splat<u8, 2>(1))"
    "Simd.mul(Simd.splat<i8, 2>(64), Simd.splat<i8, 2>(2))"
    "Simd.sum(Simd.splat<i8, 2>(100))"
    "Simd.div(Simd.splat<i32, 4>(1), Simd.splat<i32, 4>(0))"
    "Simd.div(Simd.splat<i8, 2>(-128), Simd.splat<i8, 2>(-1))"
    "Simd.load<i32, 4>(&values, 1)"
    "Simd.store(&mut values, 1, Simd.splat<i32, 4>(1))"
    "Simd.extract(Simd.splat<i32, 4>(1), 4)")
list(LENGTH trap_names trap_count)
math(EXPR last_trap "${trap_count} - 1")
foreach(index RANGE ${last_trap})
    list(GET trap_names ${index} name)
    list(GET trap_expressions ${index} expression)
    fixture("${name}" "import simd { Simd }; i32 main() { mut i32 values[4] = [0, 0, 0, 0]; ${expression}; return 0; }")
    foreach(level 0 2)
        set(program "${TEST_DIR}/${name}-${level}")
        if(CMAKE_HOST_WIN32)
            string(APPEND program ".exe")
        endif()
        run_case("${name} builds O${level}" 0 --build "-O${level}" --clang "${CLANG}" -o "${program}" "${TEST_DIR}/src/${name}.avx")
        execute_process(COMMAND "${program}" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
        if("${status}" STREQUAL "0" OR "${status}" MATCHES "timeout")
            message(FATAL_ERROR "${name}: expected prompt safe trap, got ${status}\n${output}\n${error}")
        endif()
    endforeach()
endforeach()
foreach(type "Simd<i32, 3>" "Simd<i32, 8>" "Simd<f16, 4>" "Simd<char, 4>" "SimdMask<3>")
    fixture(invalid "import simd { Simd, SimdMask }; i32 main() { ${type} value; return 0; }")
    run_case("invalid SIMD ${type}" 1 --check "${TEST_DIR}/src/invalid.avx")
endforeach()
fixture(no_import "i32 main() { Simd<i32, 4> value; return 0; }")
run_case(missing_import 1 --check "${TEST_DIR}/src/no_import.avx")
message(STATUS "SIMD native execution, checked lanes, masks, bounds, imports, and target objects passed")
