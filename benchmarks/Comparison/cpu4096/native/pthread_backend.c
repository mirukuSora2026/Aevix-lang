// ========================
//
// Created By : miruku_sora2026
//
// File name : pthread_backend.c
//
// Description :
//     새 pthread들을 시작하고 호출자 구간을 처리한 뒤 모두 합류한다.
//     Start fresh pthread workers, execute the caller chunk, and join every started worker.
//
// ========================
#include "bench_backend.h"
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct BenchChunk {
    unsigned char* data;
    uint64_t begin, end, stride;
    BenchElementCallback callback;
} BenchChunk;

static void bench_execute(const BenchChunk* chunk) {
    for (uint64_t i = chunk->begin; i < chunk->end; ++i)
        chunk->callback(chunk->data + i * chunk->stride);
}
static void* bench_entry(void* opaque) { bench_execute((const BenchChunk*)opaque); return NULL; }

int bench_parallel_for_each(void* data, uint64_t length, uint64_t stride,
                            BenchElementCallback callback, uint32_t workers) {
    if (!workers || workers > 32 || workers > length || !data || !stride || !callback) return -1;
    BenchChunk chunks[32];
    pthread_t threads[31];
    const uint64_t quotient = length / workers, remainder = length % workers;
    uint64_t begin = 0;
    for (uint32_t i = 0; i < workers; ++i) {
        const uint64_t end = begin + quotient + (i < remainder ? 1 : 0);
        chunks[i] = (BenchChunk){(unsigned char*)data, begin, end, stride, callback};
        begin = end;
    }
    uint32_t started = 0;
    int failure = 0;
    for (; started + 1 < workers; ++started) {
        failure = pthread_create(&threads[started], NULL, bench_entry, &chunks[started]);
        if (failure) break;
    }
    if (!failure) bench_execute(&chunks[workers - 1]);
    for (uint32_t i = 0; i < started; ++i) {
        const int joined = pthread_join(threads[i], NULL);
        if (joined) {
            fputs("Benchmark error: pthread join failed before quiescence\n", stderr);
            abort();
        }
    }
    return failure;
}
