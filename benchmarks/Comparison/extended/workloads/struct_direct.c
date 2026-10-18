// ========================
//
// Created By : miruku_sora2026
//
// File name : struct_direct.c
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
#include "shared_timer.h"
typedef struct { uint64_t gain; } Scale;
static uint64_t apply(const Scale *self, uint64_t value) { return value*3+self->gain; }
static void kernel(const uint64_t *input, uint64_t gain, uint64_t *output) {
    Scale op={gain}; 
    for (size_t repeat=0; repeat<64; ++repeat)
        for (size_t index=0; index<4096; ++index) output[index] += apply(&op,input[index]);
}
int main(void) {
    uint64_t seeds[2];
    if (scanf("%" SCNu64 "%" SCNu64, &seeds[0], &seeds[1]) != 2) return 91;
    uint64_t values[4096]={0},right[4096]={0},output[4096]={0};
    for (size_t iteration=0; iteration<2; ++iteration) {
        uint64_t seed=seeds[iteration]%97;
        for (size_t index=0; index<4096; ++index) {
            values[index]=(index*17+seed*13)%251;
            right[index]=(index*7+seed*3)%19;
        }
        for (size_t index=0; index<4096; ++index) output[index]=0;
        uint64_t started=aevixbench_clock_ns();
        kernel(values,seed+1,output);
        uint64_t stopped=aevixbench_clock_ns();
        if (stopped<started) return 93;
        if (iteration==1) {
            uint64_t sum=0,weighted=0;
            for(size_t index=0; index<4096; ++index) { sum+=output[index]; weighted=(weighted+(index+1)*output[index])%1000000007; }
            printf("SAMPLE struct_direct serial 0 %" PRIu64 " %" PRIu64 " %" PRIu64 " 1\n",stopped-started,sum,weighted);
        }
    }
    printf("VERIFY struct_direct serial ");
    for(size_t index=0; index<4096; ++index) printf("%s%" PRIu64,index?",":"",output[index]);
    puts("");
    if (fflush(stdout)!=0) return 94;
    return 0;
}
