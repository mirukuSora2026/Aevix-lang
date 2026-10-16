// ========================
//
// Created By : miruku_sora2026
//
// File name : complex_runtime_tests.cpp
//
// Description :
//     복소수 크기와 나눗셈의 큰 값·작은 값·특수 값과 무할당 C 호출 규약을 검증한다.
//     Verify complex magnitude and division for extreme and special values through the allocation-free C ABI.
//
// ========================
#include "aevix/Runtime/ComplexRuntime.hpp"
#include <cmath>
#include <cstdio>
#include <limits>
int main()
{
    int count = 0, failures = 0;
    const auto check = [&](bool condition, const char* label) {
        ++count;
        if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
    };
    const auto inf = std::numeric_limits<double>::infinity();
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    check(aevix_complex_magnitude(3.0, 4.0) == 5.0, "3-4-5 magnitude");
    check(std::isfinite(aevix_complex_magnitude(1e308, 1e308)), "finite huge magnitude");
    check(aevix_complex_magnitude(1e-308, 1e-308) > 0.0, "nonzero tiny magnitude");
    check(aevix_complex_magnitude(-0.0, -0.0) == 0.0 && !std::signbit(aevix_complex_magnitude(-0.0, -0.0)), "positive zero magnitude");
    check(std::isinf(aevix_complex_magnitude(inf, nan)), "infinity dominates NaN magnitude");
    check(std::isnan(aevix_complex_magnitude(nan, 1.0)), "NaN magnitude");
    double real = 0.0, imag = 0.0;
    aevix_complex_divide(3, 4, 1, -2, &real, &imag);
    check(real == -1.0 && imag == 2.0, "ordinary division");
    aevix_complex_divide(1e308, 1e308, 1e308, 1e308, &real, &imag);
    check(std::fabs(real - 1.0) < 1e-15 && std::fabs(imag) < 1e-15, "huge equal division");
    aevix_complex_divide(1e-308, 1e-308, 1e-308, 1e-308, &real, &imag);
    check(real == 1.0 && imag == 0.0, "tiny equal division");
    aevix_complex_divide(3.0, 4.0, inf, inf, &real, &imag);
    check(real == 0.0 && imag == 0.0, "finite divided by infinite");
    aevix_complex_divide(inf, inf, 1.0, 1.0, &real, &imag);
    check(std::isinf(real) && std::isnan(imag), "infinite divided by finite");
    aevix_complex_divide(nan, 1.0, 1.0, 1.0, &real, &imag);
    check(std::isnan(real) && std::isnan(imag), "NaN division");
    aevix_complex_divide(-0.0, -0.0, 1.0, 0.0, &real, &imag);
    check(real == 0.0 && imag == 0.0 && std::signbit(real), "signed real zero division");
    const auto maximum = std::numeric_limits<double>::max(), minimum = std::numeric_limits<double>::denorm_min();
    aevix_complex_divide(maximum, minimum, 1.0, 0.0, &real, &imag);
    check(real == maximum && imag == minimum, "huge/tiny components divided by real one");
    aevix_complex_divide(maximum, minimum, -1.0, -0.0, &real, &imag);
    check(real == -maximum && imag == -minimum, "huge/tiny components divided by negative real one");
    aevix_complex_divide(maximum, minimum, 0.0, 1.0, &real, &imag);
    check(real == minimum && imag == -maximum, "huge/tiny components divided by imaginary one");
    aevix_complex_divide(maximum, minimum, -0.0, -1.0, &real, &imag);
    check(real == -minimum && imag == maximum, "huge/tiny components divided by negative imaginary one");
    aevix_complex_divide(1e308, 1e-308, 1.0, 1e-308, &real, &imag);
    check(std::fabs(real / 1e308 - 1.0) < 1e-15 && std::fabs(imag + 1.0) < 1e-15, "general widely separated component exponents");
    aevix_complex_divide(1e-308, 1e308, 1e-308, 1.0, &real, &imag);
    check(std::fabs(real / 1e308 - 1.0) < 1e-15 && std::fabs(imag - 1.0) < 1e-15, "swapped widely separated component exponents");
    std::printf("%d complex runtime checks, %d failures\n", count, failures);
    return failures ? 1 : 0;
}
