// ========================
//
// Created By : miruku_sora2026
//
// File name : interface_dyn_smoke.c
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
typedef struct { uint64_t gain; } Scale;
static uint64_t apply(const Scale *self, uint64_t value) { return value*3+self->gain; }
typedef struct { const Scale *self; uint64_t (*apply)(const Scale *, uint64_t); } Projector;
static uint64_t invoke(Projector op, uint64_t value) { return op.apply(op.self,value); }
static void kernel(const uint64_t *input, uint64_t gain, uint64_t *output) {
    Scale op={gain}; Projector view={&op,apply};
    for (size_t repeat=0; repeat<2; ++repeat)
        for (size_t index=0; index<16; ++index) output[index] += invoke(view,input[index]);
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
        kernel(values,seed+1,output);
        if (iteration==0) { const uint64_t expected[16]={2,104,206,308,410,512,614,716,818,920,1022,1124,1226,1328,1430,26}; for(size_t index=0;index<16;++index) if(output[index]!=expected[index]) return 1; }
        if (iteration==1) { const uint64_t expected[16]={82,184,286,388,490,592,694,796,898,1000,1102,1204,1306,1408,4,106}; for(size_t index=0;index<16;++index) if(output[index]!=expected[index]) return 1; }
    }
    return 0;
}
