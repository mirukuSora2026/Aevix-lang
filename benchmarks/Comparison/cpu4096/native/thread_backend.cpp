// ========================
//
// Created By : miruku_sora2026
//
// File name : thread_backend.cpp
//
// Description :
//     새 std::thread 작업자와 호출자가 같은 원소 콜백을 구간별로 실행한다.
//     Run the same element callback on disjoint chunks using fresh std::thread workers and the caller.
//
// ========================
#include "bench_backend.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <thread>

namespace {
    struct Chunk final {
        std::byte* data;
        std::uint64_t begin, end, stride;
        BenchElementCallback callback;
    };
    void execute(const Chunk* chunk) {
        for (auto i = chunk->begin; i < chunk->end; ++i)
            chunk->callback(chunk->data + i * chunk->stride);
    }
}
extern "C" int bench_parallel_for_each(void* data, std::uint64_t length, std::uint64_t stride,
                                      BenchElementCallback callback, std::uint32_t workers) {
    if (!workers || workers > 32 || workers > length || !data || !stride || !callback) return -1;
    std::array<Chunk, 32> chunks{};
    std::array<std::thread, 31> threads;
    const auto quotient = length / workers, remainder = length % workers;
    std::uint64_t begin = 0;
    for (std::uint32_t i = 0; i < workers; ++i) {
        const auto end = begin + quotient + (i < remainder ? 1 : 0);
        chunks[i] = {static_cast<std::byte*>(data), begin, end, stride, callback};
        begin = end;
    }
    std::uint32_t started = 0;
    int failure = 0;
    try {
        for (; started + 1 < workers; ++started) threads[started] = std::thread(execute, &chunks[started]);
        execute(&chunks[workers - 1]);
    } catch (const std::exception&) {
        failure = -2;
    }
    for (std::uint32_t i = 0; i < started; ++i) {
        try { threads[i].join(); }
        catch (const std::exception&) { std::terminate(); }
    }
    return failure;
}
