// ========================
//
// Created By : miruku_sora2026
//
// File name : clock_calibration.c
//
// Description :
//     네이티브 호출만으로 공통 시계의 해상도와 연속 호출 간격을 기록한다.
//     Record shared-clock resolution and consecutive-call deltas using only native calls.
//
// ========================
#include "shared_timer.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    uint64_t deltas[4096];
    const uint64_t resolution = aevixbench_clock_resolution_ns();
    for (unsigned index = 0; index < 4096; ++index) {
        const uint64_t first = aevixbench_clock_ns();
        const uint64_t second = aevixbench_clock_ns();
        if (second < first) abort();
        deltas[index] = second - first;
    }
    printf("CLOCK CLOCK_MONOTONIC_RAW %" PRIu64 " 4096\n", resolution);
    fputs("DELTAS ", stdout);
    for (unsigned index = 0; index < 4096; ++index)
        printf("%s%" PRIu64, index ? "," : "", deltas[index]);
    putchar('\n');
    return fflush(stdout) == 0 ? 0 : 1;
}
