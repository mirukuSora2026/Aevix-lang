# ========================
#
# Created By : miruku_sora2026
#
# File name : complex_cli_tests.cmake
#
# Description :
#     명시적으로 가져온 복소수와 봉인된 실수 계약을 실제 컴파일·실행 경로에서 검증한다.
#     Verify explicitly imported complex values and the sealed floating contract through actual compilation and execution.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR OR NOT DEFINED CLANG)
    message(FATAL_ERROR "AEVIXC, TEST_DIR and CLANG are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
function(run_case label expected)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
    if(NOT "${code}" STREQUAL "${expected}")
        message(FATAL_ERROR "${label}: exit ${code}, expected ${expected}\n${output}\n${error}")
    endif()
endfunction()
function(fixture name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
endfunction()
fixture(basic [=[
import complex { Complex, complex, ComplexError };
i32 main() {
    complex a = complex.new(3.0, 4.0);
    complex b = complex.new(1.0, -2.0);
    complex sum = complex.add(a, b);
    complex product = complex.mul(a, b);
    if (sum.real != 4.0 || sum.imag != 2.0) return 1;
    if (product.real != 11.0 || product.imag != -2.0) return 2;
    if (complex.norm(a) != 5.0 || complex.norm_squared(a) != 25.0) return 3;
    if (!complex.eq(complex.conjugate(a), complex.new(3.0, -4.0))) return 4;
    if (!complex.eq(complex.neg(a), complex.new(-3.0, -4.0))) return 5;
    if (!complex.eq(complex.sub(a, b), complex.new(2.0, 6.0))) return 6;
    if (!complex.eq(complex.imaginary_unit(), complex.new(0.0, 1.0))) return 7;
    if (!complex.eq(complex.zero(), complex.new(0.0, 0.0))) return 8;
    Result<complex, ComplexError> result = complex.div(a, b);
    switch (result) {
        case Result<complex, ComplexError>.Ok { complex value = value }:
            if (value.real != -1.0 || value.imag != 2.0) return 9;
            break;
        case Result<complex, ComplexError>.Err: return 10; break;
    }
    Result<complex, ComplexError> inverse = complex.reciprocal(complex.one());
    switch (inverse) {
        case Result<complex, ComplexError>.Ok { complex value = value }:
            return complex.eq(value, complex.one()) ? 42 : 11;
            break;
        case Result<complex, ComplexError>.Err: return 12; break;
    }
}
]=])
fixture(errors [=[
import complex { complex, ComplexError };
i32 main() {
    Result<complex, ComplexError> result = complex.div(complex.one(), complex.new(-0.0, 0.0));
    switch (result) {
        case Result<complex, ComplexError>.Ok: return 1; break;
        case Result<complex, ComplexError>.Err { ComplexError problem = error }:
            switch (problem) { case ComplexError.ZeroDivisor: return 42; break; }
            break;
    }
}
]=])
fixture(formats [=[
import complex { Complex, ComplexError };
i32 main() {
    Complex<f16> half = Complex<f16>.new(3.0f16, 4.0f16);
    Complex<bf16> brain = Complex<bf16>.new(3.0bf16, 4.0bf16);
    Complex<f32> single = Complex<f32>.new(3.0f32, 4.0f32);
    if (Complex<f16>.magnitude(half) != 5.0f16 || Complex<bf16>.norm(brain) != 5.0bf16) return 1;
    if (Complex<f32>.magnitude(single) != 5.0f32) return 2;
    if (Complex<f16>.new(1.0001f16, 0.0f16).real != 1.0f16) return 3;
    if (Complex<bf16>.new(1.0001bf16, 0.0bf16).real != 1.0bf16) return 4;
    Result<Complex<bf16>, ComplexError> brain_quotient = Complex<bf16>.div(Complex<bf16>.one(), Complex<bf16>.new(3.0bf16, 0.0bf16));
    switch (brain_quotient) {
        case Result<Complex<bf16>, ComplexError>.Ok { Complex<bf16> value = value }:
            if (value.real != 0.333984375bf16 || value.imag != 0.0bf16) return 7;
            break;
        case Result<Complex<bf16>, ComplexError>.Err: return 8; break;
    }
    Result<Complex<f16>, ComplexError> quotient = Complex<f16>.div(half, Complex<f16>.new(1.0f16, -2.0f16));
    switch (quotient) {
        case Result<Complex<f16>, ComplexError>.Ok { Complex<f16> value = value }:
            return value.real == -1.0f16 && value.imag == 2.0f16 ? 42 : 5;
            break;
        case Result<Complex<f16>, ComplexError>.Err: return 6; break;
    }
}
]=])
fixture(extremes [=[
import complex { Complex, complex, ComplexError };
Result<i32, ComplexError> run() {
    complex huge = complex.new(1e308, 1e308);
    complex tiny = complex.new(1e-308, 1e-308);
    if (!complex.is_finite(complex.new(complex.magnitude(huge), 0.0))) return Result.ok(1);
    if (complex.magnitude(tiny) == 0.0) return Result.ok(2);
    complex large_ratio = complex.div(huge, huge)?;
    if (large_ratio.real < 0.999999999999 || large_ratio.real > 1.000000000001) return Result.ok(3);
    if (large_ratio.imag < -1e-14 || large_ratio.imag > 1e-14) return Result.ok(4);
    complex small_ratio = complex.div(tiny, tiny)?;
    if (small_ratio.real < 0.999999999999 || small_ratio.real > 1.000000000001) return Result.ok(5);
    Complex<f32> large32 = Complex<f32>.new(1e30f32, 1e30f32);
    Complex<f32> small32 = Complex<f32>.new(1e-30f32, 1e-30f32);
    if (!Complex<f32>.is_finite(Complex<f32>.new(Complex<f32>.magnitude(large32), 0.0f32))) return Result.ok(6);
    if (Complex<f32>.magnitude(small32) == 0.0f32) return Result.ok(7);
    Complex<f32> ratio32 = Complex<f32>.div(large32, large32)?;
    return Result.ok(ratio32.real > 0.99999f32 && ratio32.real < 1.00001f32 ? 42 : 8);
}
i32 main() {
    Result<i32, ComplexError> result = run();
    switch (result) {
        case Result<i32, ComplexError>.Ok { i32 status = value }: return status; break;
        case Result<i32, ComplexError>.Err: return 9; break;
    }
}
]=])
fixture(specials [=[
import complex { Complex, complex, ComplexError };
Result<i32, ComplexError> run() {
    f64 zero = 0.0;
    f64 inf = 1.0 / zero;
    f64 nan = zero / zero;
    if (complex.is_finite(complex.new(inf, 0.0))) return Result.ok(1);
    if (!complex.is_nan(complex.new(nan, 1.0))) return Result.ok(2);
    if (complex.magnitude(complex.new(inf, nan)) != inf) return Result.ok(3);
    complex divided = complex.div(complex.new(3.0, 4.0), complex.new(inf, inf))?;
    if (divided.real != 0.0 || divided.imag != 0.0) return Result.ok(4);
    complex invalid = complex.div(complex.new(nan, 1.0), complex.one())?;
    if (!complex.is_nan(invalid)) return Result.ok(5);
    f64 negative = 1.0 / complex.conjugate(complex.new(0.0, 0.0)).imag;
    if (negative >= 0.0) return Result.ok(6);
    f32 zero32 = 0.0f32;
    f32 inf32 = 1.0f32 / zero32;
    f32 nan32 = zero32 / zero32;
    if (Complex<f32>.magnitude(Complex<f32>.new(inf32, nan32)) != inf32) return Result.ok(7);
    return Result.ok(42);
}
i32 main() {
    Result<i32, ComplexError> result = run();
    switch (result) {
        case Result<i32, ComplexError>.Ok { i32 status = value }: return status; break;
        case Result<i32, ComplexError>.Err: return 8; break;
    }
}
]=])
fixture(floating_contract [=[
import numeric { Floating };
type Scalar = f64;
T twice<T>(T value) where T implements Floating { return value + value; }
T negative<T>(T value) where T implements Floating { return -value; }
i32 main() {
    Scalar value = 21.0;
    return twice<Scalar>(value) == 42.0 && negative(2.0f32) == -2.0f32 ? 42 : 1;
}
]=])
fixture(full_import [=[
import complex;
import numeric;
T add<T>(T left, T right) where T implements numeric.Floating { return left + right; }
i32 main() {
    complex.Complex<f64> value = complex.Complex<f64>.new(3.0, 4.0);
    return add(complex.Complex<f64>.magnitude(value), 37.0) == 42.0 ? 42 : 1;
}
]=])
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
foreach(name basic errors formats extremes specials floating_contract full_import)
    run_case("${name} check" 0 --check "${TEST_DIR}/${name}.avx")
    foreach(level 0 2)
        run_case("${name} AOT O${level}" 42 --clang "${CLANG}" "-O${level}" "${TEST_DIR}/${name}.avx")
        if(version MATCHES "JIT enabled")
            run_case("${name} JIT O${level}" 42 --jit "-O${level}" "${TEST_DIR}/${name}.avx")
        endif()
    endforeach()
endforeach()
function(invalid name source)
    fixture("${name}" "${source}")
    run_case("${name}" 1 --check "${TEST_DIR}/${name}.avx")
endfunction()
invalid(no_import [=[i32 main() { Complex<f64> value; return 0; }]=])
invalid(integer_format [=[import complex { Complex }; i32 main() { Complex<i32> value; return 0; }]=])
invalid(forged_contract [=[import numeric { Floating }; struct Fake implements Floating {}; i32 main() { return 0; }]=])
invalid(forged_generic_contract [=[import numeric { Floating }; struct Fake<T> implements Floating {}; i32 main() { return 0; }]=])
invalid(forged_alias_contract [=[import numeric { Floating }; type Format = Floating; struct Fake implements Format {}; i32 main() { return 0; }]=])
invalid(fake_name_arithmetic [=[interface Floating {}; T twice<T>(T value) where T implements Floating { return value + value; } i32 main() { return 0; }]=])
invalid(copy_only_arithmetic [=[T twice<T>(T value) where T implements Copy { return value + value; } i32 main() { return twice(21); }]=])
invalid(bitwise_float [=[import numeric { Floating }; T bits<T>(T value) where T implements Floating { return value & value; } i32 main() { return 0; }]=])
invalid(private_helper [=[import complex { complex }; i32 main() { complex value = complex.divide_components(complex.one(), complex.one()); return 0; }]=])
invalid(no_order [=[import complex { complex }; i32 main() { return complex.one() < complex.one() ? 0 : 1; }]=])
message(STATUS "Complex arithmetic, stable magnitude/division, all component formats, special values and sealed numeric bounds passed")
