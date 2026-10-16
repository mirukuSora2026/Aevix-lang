// ========================
//
// Created By : miruku_sora2026
//
// File name : ParallelRuntime.cpp
//
// Description :
//     동적 할당 없이 작업을 나누고 실패 시 순차 처리한 뒤 모든 작업에 합류한다.
//     Split work without heap allocation, fall back sequentially on failure, and join all workers.
//
// ========================

#include "aevix/Runtime/ParallelRuntime.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

namespace
{
    constexpr std::size_t maximumWorkers = 32;
    struct Chunk final
    {
        std::byte* data = nullptr;
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        std::uint64_t stride = 0;
        AevixParallelCallback callback = nullptr;
    };
    void execute(const Chunk& chunk)
    {
        for (auto index = chunk.begin; index < chunk.end; ++index)
            chunk.callback(chunk.data + index * chunk.stride);
    }
#if defined(_WIN32)
    using Thread = HANDLE;
    [[maybe_unused]] DWORD WINAPI entry(void* argument) { execute(*static_cast<const Chunk*>(argument)); return 0; }
    bool start(Thread& thread, Chunk& chunk)
    {
#if defined(AEVIX_PARALLEL_TEST_CREATE_FAILURE)
        (void)thread; (void)chunk; return false;
#else
        thread = CreateThread(nullptr, 0, entry, &chunk, 0, nullptr);
        return thread != nullptr;
#endif
    }
    bool join(Thread thread)
    {
        const auto waited = WaitForSingleObject(thread, INFINITE);
        const bool closed = CloseHandle(thread) != 0;
        return waited == WAIT_OBJECT_0 && closed;
    }
#else
    using Thread = pthread_t;
    [[maybe_unused]] void* entry(void* argument) { execute(*static_cast<const Chunk*>(argument)); return nullptr; }
    bool start(Thread& thread, Chunk& chunk)
    {
#if defined(AEVIX_PARALLEL_TEST_CREATE_FAILURE)
        (void)thread; (void)chunk; return false;
#else
        return pthread_create(&thread, nullptr, entry, &chunk) == 0;
#endif
    }
    bool join(Thread thread) { return pthread_join(thread, nullptr) == 0; }
#endif
    [[noreturn]] void fail(const char* message)
    {
        std::fputs("Aevix parallel runtime error: ", stderr);
        std::fputs(message, stderr);
        std::fputc('\n', stderr);
        std::abort();
    }
}

extern "C" std::uint64_t aevix_parallel_worker_count()
{
#if defined(_WIN32)
    const auto count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return std::clamp<std::uint64_t>(count, 1, maximumWorkers);
#else
    const auto count = sysconf(_SC_NPROCESSORS_ONLN);
    return count <= 0 ? 1 : std::min<std::uint64_t>(static_cast<std::uint64_t>(count), maximumWorkers);
#endif
}

extern "C" void aevix_parallel_for_each(void* data, std::uint64_t length, std::uint64_t stride,
                                        AevixParallelCallback callback)
{
    if (!length) return;
    if (!data || !callback || !stride || length > std::numeric_limits<std::size_t>::max() / stride)
        fail("invalid collection descriptor");
    const auto workers = std::min<std::uint64_t>(aevix_parallel_worker_count(), length);
    std::array<Chunk, maximumWorkers> chunks{};
    std::array<Thread, maximumWorkers - 1> threads{};
    const auto quotient = length / workers;
    const auto remainder = length % workers;
    std::uint64_t begin = 0;
    for (std::uint64_t index = 0; index < workers; ++index)
    {
        const auto end = begin + quotient + (index < remainder ? 1 : 0);
        chunks[index] = {static_cast<std::byte*>(data), begin, end, stride, callback};
        begin = end;
    }
    std::uint64_t started = 0;
    for (; started + 1 < workers; ++started)
        if (!start(threads[started], chunks[started])) break;
    // 생성되지 않은 구간만 호출 스레드가 맡으므로 오류 복구 시 중복 실행이 없다.
    // The calling thread handles only unstarted chunks, so fallback never repeats an element.
    for (auto index = started; index < workers; ++index) execute(chunks[index]);
    for (std::uint64_t index = 0; index < started; ++index)
        if (!join(threads[index])) fail("worker join failed");
}
