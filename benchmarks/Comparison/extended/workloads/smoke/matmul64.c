// ========================
//
// Created By : miruku_sora2026
//
// File name : matmul64_smoke.c
//
// Description :
//     Aevix와 동일한 정수 입력·커널·시계 경계를 사용하는 생성된 비교 프로그램.
//     Generated C comparison with the same integer inputs, kernel, and clock boundaries.
//
// ========================
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static void kernel(const uint64_t *left, const uint64_t *right, uint64_t *output) {
    for (size_t row=0; row<4; ++row)
        for (size_t column=0; column<4; ++column) {
            uint64_t total=0;
            for (size_t inner=0; inner<4; ++inner)
                total += left[row*4+inner] * right[inner*4+column];
            output[row*4+column]=total;
        }
}
int main(void) {
    uint64_t seeds[2];
    if (scanf("%" SCNu64 "%" SCNu64, &seeds[0], &seeds[1]) != 2) return 91;
    uint64_t values[16]={0},right[16]={0},output[16]={0};
    for (size_t iteration=0; iteration<2; ++iteration) {
        uint64_t seed=seeds[iteration]%97;
        for (size_t index=0; index<16; ++index) {
            values[index]=(index*11+seed*13)%17;
            right[index]=(index*7+seed*3)%19;
        }
        for (size_t index=0; index<16; ++index) output[index]=0;
        kernel(values,right,output);
        if (iteration==0) { const uint64_t expected[16]={317,446,157,286,378,359,378,169,286,323,208,188,177,389,259,224}; for(size_t index=0;index<16;++index) if(output[index]!=expected[index]) return 1; }
        if (iteration==1) { const uint64_t expected[16]={257,355,358,323,95,249,308,139,341,466,496,431,179,360,446,247}; for(size_t index=0;index<16;++index) if(output[index]!=expected[index]) return 1; }
    }
    return 0;
}
