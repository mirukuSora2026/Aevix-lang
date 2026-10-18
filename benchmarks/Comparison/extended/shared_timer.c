// ========================
//
// Created By : miruku_sora2026
//
// File name : shared_timer.c
//
// Description :
//     CLOCK_MONOTONIC_RAW를 나노초로 반환하며 실패·범위 오류는 즉시 중단한다.
//     Return CLOCK_MONOTONIC_RAW nanoseconds, aborting on clock or representation failure.
//
// ========================
#include "shared_timer.h"
#include <stdlib.h>
#include <time.h>

#ifndef CLOCK_MONOTONIC_RAW
#error "The shared benchmark timer requires CLOCK_MONOTONIC_RAW."
#endif

static uint64_t checked_nanoseconds(struct timespec value) {
    if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000L) abort();
    const uint64_t seconds = (uint64_t)value.tv_sec;
    const uint64_t nanoseconds = (uint64_t)value.tv_nsec;
    if (seconds > (UINT64_MAX - nanoseconds) / UINT64_C(1000000000)) abort();
    return seconds * UINT64_C(1000000000) + nanoseconds;
}

AEVIXBENCH_CLOCK_API uint64_t aevixbench_clock_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0) abort();
    return checked_nanoseconds(value);
}

AEVIXBENCH_CLOCK_API uint64_t aevixbench_clock_resolution_ns(void) {
    struct timespec value;
    if (clock_getres(CLOCK_MONOTONIC_RAW, &value) != 0) abort();
    const uint64_t resolution = checked_nanoseconds(value);
    if (!resolution) abort();
    return resolution;
}
