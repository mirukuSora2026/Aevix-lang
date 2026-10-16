// ========================
//
// Created By : miruku_sora2026
//
// File name : ComplexRuntime.hpp
//
// Description :
//     복소수의 안정적인 크기와 나눗셈에 쓰는 단순 C 호출 규약을 정의한다.
//     Define the scalar C ABI for stable complex magnitude and division.
//
// ========================
#pragma once
extern "C"
{
    double aevix_complex_magnitude(double real, double imag) noexcept;
    void aevix_complex_divide(double real, double imag, double divisorReal, double divisorImag,
                              double* outputReal, double* outputImag) noexcept;
}
