// ========================
//
// Created By : miruku_sora2026
//
// File name : bench_backend.h
//
// Description :
//     청크 전체 커널 대신 원소별 콜백을 받는 공통 병렬 호출 경계를 정의한다.
//     Define a common parallel boundary receiving per-element callbacks rather than whole-chunk kernels.
//
// ========================
#ifndef AEVIX_CPU4096_BACKEND_H
#define AEVIX_CPU4096_BACKEND_H
#include <stdint.h>
typedef void (*BenchElementCallback)(void*);
#ifdef __cplusplus
extern "C" {
#endif
int bench_parallel_for_each(void* data, uint64_t length, uint64_t stride,
                            BenchElementCallback callback, uint32_t workers);
#ifdef __cplusplus
}
#endif
#endif
