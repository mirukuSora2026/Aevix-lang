// ========================
//
// Created By : miruku_sora2026
//
// File name : ComplexRuntime.cpp
//
// Description :
//     표준 수치 알고리즘으로 복소수 크기와 나눗셈을 계산하며 할당이나 예외를 사용하지 않는다.
//     Calculate complex magnitude and division with standard numerical algorithms, without allocation or exceptions.
//
// ========================
#include "aevix/Runtime/ComplexRuntime.hpp"
#include <cmath>
#include <complex>
extern "C" double aevix_complex_magnitude(double real, double imag) noexcept
{
    return std::hypot(real, imag);
}
extern "C" void aevix_complex_divide(double real, double imag, double divisorReal, double divisorImag,
                                    double* outputReal, double* outputImag) noexcept
{
    if (std::isfinite(real) && std::isfinite(imag) && std::isfinite(divisorReal) && std::isfinite(divisorImag)
        && (divisorReal != 0.0 || divisorImag != 0.0))
    {
        struct Product { double fraction; int exponent; };
        const auto product = [](double left, double right) noexcept {
            int leftExponent = 0, rightExponent = 0;
            const auto leftFraction = std::frexp(left, &leftExponent);
            const auto rightFraction = std::frexp(right, &rightExponent);
            return Product{leftFraction * rightFraction, leftExponent + rightExponent};
        };
        int realExponent = 0, imagExponent = 0;
        std::frexp(divisorReal, &realExponent); std::frexp(divisorImag, &imagExponent);
        const auto denominatorExponent = divisorReal == 0.0 ? imagExponent : divisorImag == 0.0 ? realExponent
            : realExponent > imagExponent ? realExponent : imagExponent;
        const auto scaledReal = std::scalbn(divisorReal, -denominatorExponent);
        const auto scaledImag = std::scalbn(divisorImag, -denominatorExponent);
        const auto denominator = scaledReal * scaledReal + scaledImag * scaledImag;
        const auto quotient = [&](Product left, Product right) noexcept {
            const auto exponent = left.fraction == 0.0 ? right.exponent : right.fraction == 0.0 ? left.exponent
                : left.exponent > right.exponent ? left.exponent : right.exponent;
            const auto numerator = std::scalbn(left.fraction, left.exponent - exponent)
                + std::scalbn(right.fraction, right.exponent - exponent);
            return std::scalbn(numerator / denominator, exponent - 2 * denominatorExponent);
        };
        // 각 출력의 두 곱을 별도로 정규화해 큰 성분 옆의 작은 성분도 보존한다.
        // Normalize each output's products separately, preserving tiny components beside large ones.
        *outputReal = quotient(product(real, divisorReal), product(imag, divisorImag));
        *outputImag = quotient(product(imag, divisorReal), product(-real, divisorImag));
        return;
    }
    const auto value = std::complex<double>(real, imag) / std::complex<double>(divisorReal, divisorImag);
    *outputReal = value.real();
    *outputImag = value.imag();
}
