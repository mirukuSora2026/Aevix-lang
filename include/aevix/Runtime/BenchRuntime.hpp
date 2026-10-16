// ========================
//
// Created By : miruku_sora2026
//
// File name : BenchRuntime.hpp
//
// Description :
//     함수 벤치마크의 시작·종료와 프로세스 메모리 표본 보고용 C 호출 규약을 정의한다.
//     Define the C ABI for function timing and sampled process-memory reports.
//
// ========================
#pragma once
#include <cstdint>

extern "C"
{
    // 토큰 0은 측정이 비활성화되었거나 메타데이터를 만들지 못했다는 뜻이다.
    // Token zero means measurement was disabled or its metadata could not be allocated.
    std::uint64_t aevix_bench_begin(const char* name);

    // 중첩 호출은 독립적으로 종료하며 이미 종료된 토큰과 0은 무시한다.
    // Nested calls end independently; completed tokens and zero are ignored.
    void aevix_bench_end(std::uint64_t token);

    // 정상 종료에도 자동 보고하며 이 함수는 현재의 완료된 호출 통계를 출력한다.
    // Normal process exit also reports automatically; this function prints completed-call statistics.
    void aevix_bench_report();
}
