// ========================
//
// Created By : miruku_sora2026
//
// File name : ParallelRuntime.hpp
//
// Description :
//     분리된 원소 대여를 병렬로 처리하고 반환 전에 합류하는 C 호출 규약을 정의한다.
//     Define a C ABI for disjoint element callbacks that join before returning.
//
// ========================

#pragma once
#include <cstdint>

extern "C"
{
    using AevixParallelCallback = void (*)(void* element);

    // 스케줄러 상한이며 실제 실행 스레드 수나 성능 향상을 보장하지 않는다.
    // Report the scheduler limit without promising a specific thread count or speedup.
    std::uint64_t aevix_parallel_worker_count();

    // 각 원소를 정확히 한 번 호출하고 생성 실패 시 남은 구간을 호출 스레드에서 처리한다.
    // Visit every element exactly once and process unstarted chunks locally if thread creation fails.
    void aevix_parallel_for_each(void* data, std::uint64_t length, std::uint64_t stride,
                                 AevixParallelCallback callback);
}
