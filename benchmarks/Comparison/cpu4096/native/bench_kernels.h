// ========================
//
// Created By : miruku_sora2026
//
// File name : bench_kernels.h
//
// Description :
//     공유 명세에서 생성된 상수로 정확한 정수 커널과 순차 순회를 정의한다.
//     Define exact integer kernels and serial traversal using shared-spec generated constants.
//
// ========================
#ifndef AEVIX_CPU4096_KERNELS_H
#define AEVIX_CPU4096_KERNELS_H
#include <stdint.h>
#include "bench_spec.h"

typedef enum BenchCase {
    BENCH_LCG_1, BENCH_LCG_256, BENCH_LCG_16384, BENCH_PRIME_SEARCH
} BenchCase;
typedef void (*BenchCallback)(void*);

static inline uint64_t bench_lcg(uint64_t input, uint64_t rounds) {
    uint64_t value = input % BENCH_LCG_MODULUS + 1;
    for (uint64_t round = 0; round < rounds; ++round)
        value = (value * BENCH_LCG_MULTIPLIER + 1) % BENCH_LCG_MODULUS;
    return value;
}
static inline uint64_t bench_prime(uint64_t input) {
    uint64_t candidate = UINT64_C(1000000001) + (input % BENCH_ELEMENTS) * UINT64_C(1024);
    for (;;) {
        uint64_t divisor = 3;
        while (divisor * divisor <= candidate) {
            if (candidate % divisor == 0) break;
            divisor += 2;
        }
        if (divisor * divisor > candidate) return candidate;
        candidate += 2;
    }
}

// 순차 경로는 커널을 직접 호출하며 병렬 경로만 원소 콜백 주소를 전달한다.
// Serial traversal calls known kernels directly; only parallel traversal passes an element callback.
static void bench_lcg_1_callback(void* value) {
    *(uint64_t*)value = bench_lcg(*(uint64_t*)value, BENCH_ROUNDS_LCG_1);
}
static void bench_lcg_256_callback(void* value) {
    *(uint64_t*)value = bench_lcg(*(uint64_t*)value, BENCH_ROUNDS_LCG_256);
}
static void bench_lcg_16384_callback(void* value) {
    *(uint64_t*)value = bench_lcg(*(uint64_t*)value, BENCH_ROUNDS_LCG_16384);
}
static void bench_prime_callback(void* value) { *(uint64_t*)value = bench_prime(*(uint64_t*)value); }

static inline BenchCallback bench_callback(BenchCase selected) {
    switch (selected) {
        case BENCH_LCG_1: return bench_lcg_1_callback;
        case BENCH_LCG_256: return bench_lcg_256_callback;
        case BENCH_LCG_16384: return bench_lcg_16384_callback;
        case BENCH_PRIME_SEARCH: return bench_prime_callback;
    }
    return (BenchCallback)0;
}
static inline void bench_serial(uint64_t* values, BenchCase selected) {
    switch (selected) {
        case BENCH_LCG_1:
            for (uint64_t i = 0; i < BENCH_ELEMENTS; ++i) values[i] = bench_lcg(values[i], BENCH_ROUNDS_LCG_1);
            break;
        case BENCH_LCG_256:
            for (uint64_t i = 0; i < BENCH_ELEMENTS; ++i) values[i] = bench_lcg(values[i], BENCH_ROUNDS_LCG_256);
            break;
        case BENCH_LCG_16384:
            for (uint64_t i = 0; i < BENCH_ELEMENTS; ++i) values[i] = bench_lcg(values[i], BENCH_ROUNDS_LCG_16384);
            break;
        case BENCH_PRIME_SEARCH:
            for (uint64_t i = 0; i < BENCH_ELEMENTS; ++i) values[i] = bench_prime(values[i]);
            break;
    }
}
#endif
