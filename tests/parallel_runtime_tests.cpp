// ========================
//
// Created By : miruku_sora2026
//
// File name : parallel_runtime_tests.cpp
//
// Description :
//     분리된 구간의 정확한 단일 실행, 실제 스레드 사용과 생성 실패 복구를 검사한다.
//     Check exactly-once disjoint work, actual thread use, and thread-creation failure fallback.
//
// ========================

#include "aevix/Runtime/ParallelRuntime.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace
{
    std::mutex observedMutex;
    std::set<std::thread::id> observed;
    void increment(void* element)
    {
        ++*static_cast<std::uint64_t*>(element);
        std::lock_guard lock(observedMutex);
        observed.insert(std::this_thread::get_id());
    }
    void toggle(void* element) { *static_cast<bool*>(element) = !*static_cast<bool*>(element); }
}

int main()
{
    const auto workers = aevix_parallel_worker_count();
    if (workers < 1 || workers > 32) return 1;
    aevix_parallel_for_each(nullptr, 0, 0, nullptr);
    std::uint64_t one = 0;
    aevix_parallel_for_each(&one, 1, sizeof(one), increment);
    if (one != 1 || observed.size() != 1) return 2;
    observed.clear();
    std::vector<std::uint64_t> values(8193);
    aevix_parallel_for_each(values.data(), values.size(), sizeof(values[0]), increment);
    for (const auto value : values) if (value != 1) return 3;
#if defined(AEVIX_PARALLEL_TEST_CREATE_FAILURE)
    if (observed.size() != 1) return 4;
#else
    if (workers > 1 && observed.size() < 2) return 5;
#endif
    std::array<bool, 17> bits{};
    aevix_parallel_for_each(bits.data(), bits.size(), sizeof(bits[0]), toggle);
    for (const auto bit : bits) if (!bit) return 6;
    std::cout << "Parallel runtime passed: " << values.size() << " elements exactly once; observed "
              << observed.size() << " thread(s), scheduler limit " << workers << ".\n";
}
