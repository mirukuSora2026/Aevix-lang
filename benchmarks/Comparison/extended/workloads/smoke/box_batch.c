// ========================
//
// Created By : miruku_sora2026
//
// File name : box_batch_smoke.c
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
static void kernel(const uint64_t *input, uint64_t *output) {
    for (size_t index=0; index<16; ++index) {
        uint64_t *owner=malloc(sizeof(*owner));
        if (!owner) abort();
        *owner=input[index];
        output[index]=*owner*3+1;
        free(owner);
    }
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
        if (iteration==0) { const uint64_t expected[16]={1,52,103,154,205,256,307,358,409,460,511,562,613,664,715,13}; for(size_t index=0;index<16;++index) if(output[index]!=expected[index]) return 1; }
        if (iteration==1) { const uint64_t expected[16]={40,91,142,193,244,295,346,397,448,499,550,601,652,703,1,52}; for(size_t index=0;index<16;++index) if(output[index]!=expected[index]) return 1; }
    }
    return 0;
}
