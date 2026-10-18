// ========================
//
// Created By : miruku_sora2026
//
// File name : slice_reduce_smoke.c
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
typedef struct { const uint64_t *data; size_t length; } Slice;
static uint64_t identity(uint64_t value) { return value; }
static uint64_t project(uint64_t value) { return value * 3 + 1; }
static void kernel(const uint64_t *values, uint64_t *output) {
    Slice input={values,16};
    for (size_t repeat=0; repeat<2; ++repeat)
        for (size_t index=0; index<16; ++index) output[index%16] += project(identity(input.data[index]));
}
int main(void) {
    uint64_t seeds[2];
    if (scanf("%" SCNu64 "%" SCNu64, &seeds[0], &seeds[1]) != 2) return 91;
    uint64_t values[16]={0},right[16]={0},output[16]={0};
    for (size_t iteration=0; iteration<2; ++iteration) {
        uint64_t seed=seeds[iteration]%97;
        for (size_t index=0; index<16; ++index) {
            values[index]=(index*17+seed*13)%251;
            right[index]=(index*7+seed*3)%19;
        }
        for (size_t index=0; index<16; ++index) output[index]=0;
        kernel(values,output);
        if (iteration==0) { const uint64_t expected[16]={2,104,206,308,410,512,614,716,818,920,1022,1124,1226,1328,1430,26}; for(size_t index=0;index<16;++index) if(output[index]!=expected[index]) return 1; }
        if (iteration==1) { const uint64_t expected[16]={80,182,284,386,488,590,692,794,896,998,1100,1202,1304,1406,2,104}; for(size_t index=0;index<16;++index) if(output[index]!=expected[index]) return 1; }
    }
    return 0;
}
