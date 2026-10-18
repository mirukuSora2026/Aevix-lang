// ========================
//
// Created By : miruku_sora2026
//
// File name : shared_timer.h
//
// Description :
//     C·Aevix AOT·JIT가 같은 공유 라이브러리의 단조 시계를 사용하게 한다.
//     Give C, Aevix AOT, and JIT one monotonic clock in the same shared library.
//
// ========================
#ifndef AEVIX_BENCH_SHARED_TIMER_H
#define AEVIX_BENCH_SHARED_TIMER_H

#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define AEVIXBENCH_CLOCK_API __attribute__((noinline, visibility("default")))
#else
#define AEVIXBENCH_CLOCK_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

AEVIXBENCH_CLOCK_API uint64_t aevixbench_clock_ns(void);
AEVIXBENCH_CLOCK_API uint64_t aevixbench_clock_resolution_ns(void);

#ifdef __cplusplus
}
#endif

#endif
