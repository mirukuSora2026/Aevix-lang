// ========================
//
// Created By : miruku_sora2026
//
// File name : bench_runner.h
//
// Description :
//     회차별 입력·타이머·체크섬·전체 값 출력을 C와 C++에서 동일하게 유지한다.
//     Keep per-iteration input, timing, checksums, and full-value output identical in C and C++.
//
// ========================
#ifndef AEVIX_CPU4096_RUNNER_H
#define AEVIX_CPU4096_RUNNER_H
#include "bench_backend.h"
#include "bench_kernels.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int bench_number(const char* text, uint64_t* result) {
    if (!text || !*text) return 0;
    uint64_t value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return 0;
        const uint64_t digit = (uint64_t)(*text - '0');
        if (value > (UINT64_C(9007199254740991) - digit) / 10) return 0;
        value = value * 10 + digit;
    }
    *result = value;
    return 1;
}
static int bench_seed(uint64_t fallback, uint64_t* result) {
    if (isatty(fileno(stdin))) { *result = fallback; return 1; }
    char line[128];
    if (!fgets(line, sizeof line, stdin)) {
        if (ferror(stdin)) return 0;
        *result = fallback;
        return 1;
    }
    if (!strchr(line, '\n') && !feof(stdin)) return 0;
    char* first = line;
    while (*first && isspace((unsigned char)*first)) ++first;
    char* last = first + strlen(first);
    while (last > first && isspace((unsigned char)last[-1])) --last;
    *last = '\0';
    if (!bench_number(first, result)) return 0;
    *result %= BENCH_ELEMENTS;
    return 1;
}
static uint64_t bench_now_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        fputs("Benchmark error: monotonic clock failed\n", stderr);
        exit(1);
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}
static uint32_t bench_logical_workers(void) {
    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    const uint64_t positive = count > 0 ? (uint64_t)count : UINT64_C(1);
    const uint64_t limited = positive < 32 ? positive : 32;
    return (uint32_t)(limited < BENCH_ELEMENTS ? limited : BENCH_ELEMENTS);
}
static int bench_main(int argc, char** argv) {
    if (argc < 3 || argc > 6) {
        fputs("Usage: benchmark CASE MODE [REPEATS=5] [WARMUPS=1] [SEED=0]\n", stderr);
        return 1;
    }
    BenchCase selected;
    if (!strcmp(argv[1], "lcg_1")) selected = BENCH_LCG_1;
    else if (!strcmp(argv[1], "lcg_256")) selected = BENCH_LCG_256;
    else if (!strcmp(argv[1], "lcg_16384")) selected = BENCH_LCG_16384;
    else if (!strcmp(argv[1], "prime_search")) selected = BENCH_PRIME_SEARCH;
    else { fputs("Benchmark error: unknown case\n", stderr); return 1; }
    const int parallel = !strcmp(argv[2], "parallel_fresh");
    if (!parallel && strcmp(argv[2], "serial")) {
        fputs("Benchmark error: unknown mode\n", stderr);
        return 1;
    }
    uint64_t repeats = BENCH_DEFAULT_SAMPLES, warmups = BENCH_DEFAULT_WARMUPS, fallback = 0;
    if ((argc > 3 && !bench_number(argv[3], &repeats)) || !repeats ||
        (argc > 4 && !bench_number(argv[4], &warmups)) ||
        (argc > 5 && !bench_number(argv[5], &fallback)) || repeats > UINT64_MAX - warmups) {
        fputs("Benchmark error: invalid repeats, warmups, or seed\n", stderr);
        return 1;
    }
    fallback %= BENCH_ELEMENTS;
    const uint32_t workers = parallel ? bench_logical_workers() : 1;
    const BenchCallback callback = bench_callback(selected);
    uint64_t* values = (uint64_t*)malloc((size_t)BENCH_ELEMENTS * sizeof(uint64_t));
    if (!values) { fputs("Benchmark error: input allocation failed\n", stderr); return 1; }
    for (uint64_t iteration = 0; iteration < warmups + repeats; ++iteration) {
        uint64_t seed = 0;
        if (!bench_seed(fallback, &seed)) {
            fputs("Benchmark error: invalid stdin seed\n", stderr);
            free(values);
            return 1;
        }
        // 입력과 초기화는 매 회차 타이머보다 먼저 수행한다.
        // Read fresh input and initialize before each measured interval.
        for (uint64_t i = 0; i < BENCH_ELEMENTS; ++i) values[i] = (i + seed) % BENCH_ELEMENTS;
        const uint64_t start = bench_now_ns();
        int failure = 0;
        if (parallel) failure = bench_parallel_for_each(values, BENCH_ELEMENTS, sizeof(uint64_t), callback, workers);
        else bench_serial(values, selected);
        const uint64_t elapsed = bench_now_ns() - start;
        if (failure) {
            fprintf(stderr, "Benchmark error: fresh worker startup/join failed (%d)\n", failure);
            free(values);
            return 1;
        }
        if (iteration >= warmups) {
            uint64_t total = 0, weighted = 0;
            for (uint64_t i = 0; i < BENCH_ELEMENTS; ++i) {
                total += values[i];
                weighted = (weighted + (i + 1) * values[i]) % BENCH_CHECKSUM_MODULUS;
            }
            printf("SAMPLE %s %s %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %u\n",
                argv[1], argv[2], iteration - warmups, elapsed, total, weighted, (unsigned)workers);
        }
    }
    printf("VERIFY %s %s ", argv[1], argv[2]);
    for (uint64_t i = 0; i < BENCH_ELEMENTS; ++i) printf("%s%" PRIu64, i ? "," : "", values[i]);
    putchar('\n');
    free(values);
    return 0;
}
#endif
