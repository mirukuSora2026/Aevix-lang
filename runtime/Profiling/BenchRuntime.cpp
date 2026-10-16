// ========================
//
// Created By : miruku_sora2026
//
// File name : BenchRuntime.cpp
//
// Description :
//     중첩 함수의 경과 시간과 실제 프로세스 RSS 표본을 스레드 안전하게 집계한다.
//     Aggregate nested-function wall time and actual process RSS samples safely across threads.
//
// ========================
#include "aevix/Runtime/BenchRuntime.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <limits>
#include <new>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#include <cerrno>
#include <ctime>
#else
#include <pthread.h>
#include <unistd.h>
#include <cerrno>
#include <ctime>
#endif

namespace
{
    constexpr std::uint64_t intervalMilliseconds = 10;
    constexpr std::size_t maximumFunctions = 16384;
    constexpr std::size_t maximumNameBytes = 4096;
    struct Function final
    {
        Function* next;
        const char* name;
        std::uint64_t calls;
        std::uint64_t totalNanoseconds;
        std::uint64_t maximumNanoseconds;
        std::uint64_t residentPeak;
        long double residentSum;
        std::uint64_t samples;
        std::uint64_t unavailableSamples;
        std::uint64_t active;
    };
    struct Invocation final
    {
        Invocation* next;
        Function* function;
        std::uint64_t start;
        std::uint64_t token;
    };
    // 이 상태는 모두 정적 0 초기화되므로 다른 런타임의 생성·파괴 순서에 의존하지 않는다.
    // Every field is statically zero-initialized, independent of other runtimes' construction or destruction order.
    struct State final
    {
        Function* functions;
        Function* lastFunction;
        Invocation* invocations;
        std::size_t functionCount;
        std::uint64_t nextToken;
        std::uint64_t generation;
        std::uint64_t reportedGeneration;
        bool initialized;
        bool shuttingDown;
        bool samplerStarted;
        bool reportPrinted;
        bool metadataWarning;
#if defined(_WIN32)
        HANDLE sampler;
#else
        pthread_t sampler;
#endif
    };
    State state{};
#if defined(_WIN32)
    SRWLOCK mutex = SRWLOCK_INIT;
    CONDITION_VARIABLE changed = CONDITION_VARIABLE_INIT;
    void lock() { AcquireSRWLockExclusive(&mutex); }
    void unlock() { ReleaseSRWLockExclusive(&mutex); }
    void wake() { WakeAllConditionVariable(&changed); }
    void wait() { SleepConditionVariableSRW(&changed, &mutex, INFINITE, 0); }
    bool waitInterval() { return !SleepConditionVariableSRW(&changed, &mutex, static_cast<DWORD>(intervalMilliseconds), 0) && GetLastError() == ERROR_TIMEOUT; }
#else
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
    void lock() { pthread_mutex_lock(&mutex); }
    void unlock() { pthread_mutex_unlock(&mutex); }
    void wake() { pthread_cond_broadcast(&changed); }
    void wait() { pthread_cond_wait(&changed, &mutex); }
    bool waitInterval()
    {
        timespec deadline{};
        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return false;
        deadline.tv_nsec += static_cast<long>(intervalMilliseconds * 1000000);
        if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
        return pthread_cond_timedwait(&changed, &mutex, &deadline) == ETIMEDOUT;
    }
#endif
    struct Locked final
    {
        Locked() { lock(); }
        ~Locked() { unlock(); }
    };
    std::uint64_t monotonicNanoseconds()
    {
#if defined(_WIN32)
        LARGE_INTEGER counter{}, frequency{};
        if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 || counter.QuadPart < 0) return 0;
        const auto ticks = static_cast<std::uint64_t>(counter.QuadPart), rate = static_cast<std::uint64_t>(frequency.QuadPart);
        const auto seconds = ticks / rate, remainder = ticks % rate;
        if (seconds > std::numeric_limits<std::uint64_t>::max() / 1000000000ULL) return std::numeric_limits<std::uint64_t>::max();
        const auto fraction = remainder <= std::numeric_limits<std::uint64_t>::max() / 1000000000ULL
            ? remainder * 1000000000ULL / rate
            : static_cast<std::uint64_t>(static_cast<double>(remainder) * 1000000000.0 / static_cast<double>(rate));
        return seconds * 1000000000ULL + fraction;
#else
        timespec instant{};
        if (clock_gettime(CLOCK_MONOTONIC, &instant) != 0 || instant.tv_sec < 0 || instant.tv_nsec < 0) return 0;
        const auto seconds = static_cast<std::uint64_t>(instant.tv_sec);
        if (seconds > std::numeric_limits<std::uint64_t>::max() / 1000000000ULL) return std::numeric_limits<std::uint64_t>::max();
        return seconds * 1000000000ULL + static_cast<std::uint64_t>(instant.tv_nsec);
#endif
    }
    std::uint64_t saturatingAdd(std::uint64_t left, std::uint64_t right)
    {
        return right > std::numeric_limits<std::uint64_t>::max() - left ? std::numeric_limits<std::uint64_t>::max() : left + right;
    }
    bool residentBytes(std::uint64_t& value)
    {
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return false;
        value = static_cast<std::uint64_t>(counters.WorkingSetSize); return true;
#elif defined(__APPLE__)
        mach_task_basic_info_data_t information{};
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&information), &count) != KERN_SUCCESS) return false;
        value = static_cast<std::uint64_t>(information.resident_size); return true;
#else
        // statm의 두 번째 필드는 현재 상주 페이지 수이며 누적 최대 RSS가 아니다.
        // The second statm field is the current resident-page count, not a cumulative maximum RSS.
        FILE* input = std::fopen("/proc/self/statm", "r");
        if (!input) return false;
        unsigned long long totalPages = 0, pages = 0;
        const bool parsed = std::fscanf(input, "%llu %llu", &totalPages, &pages) == 2;
        std::fclose(input);
        const auto pageBytes = sysconf(_SC_PAGESIZE);
        if (!parsed || pageBytes <= 0 || pages > std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(pageBytes)) return false;
        value = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(pageBytes); return true;
#endif
    }
    void sampleLocked()
    {
        if (!state.invocations) return;
        std::uint64_t resident = 0;
        const bool available = residentBytes(resident);
        // 재귀 깊이와 관계없이 같은 함수에는 한 시점의 RSS 표본을 한 번만 더한다.
        // Charge one RSS sample per active function, regardless of its recursion depth.
        for (auto* function = state.functions; function; function = function->next)
        {
            if (!function->active) continue;
            if (!available) { function->unavailableSamples = saturatingAdd(function->unavailableSamples, 1); continue; }
            if (resident > function->residentPeak) function->residentPeak = resident;
            function->residentSum += static_cast<long double>(resident);
            function->samples = saturatingAdd(function->samples, 1);
        }
    }
#if defined(_WIN32)
    DWORD WINAPI sampleThread(void*)
#else
    void* sampleThread(void*)
#endif
    {
        lock();
        while (!state.shuttingDown)
        {
            if (!state.invocations) wait();
            else if (waitInterval() && !state.shuttingDown) sampleLocked();
        }
        unlock();
#if defined(_WIN32)
        return 0;
#else
        return nullptr;
#endif
    }
    void warnMetadataLocked()
    {
        if (state.metadataWarning) return;
        state.metadataWarning = true;
        std::fputs("Aevix @bench: measurement metadata unavailable; affected calls are not recorded.\n", stderr);
    }
    void quotedName(const char* name)
    {
        std::fputc('"', stderr);
        for (const auto* cursor = reinterpret_cast<const unsigned char*>(name); *cursor; ++cursor)
        {
            if (*cursor == '"' || *cursor == '\\') { std::fputc('\\', stderr); std::fputc(*cursor, stderr); }
            else if (*cursor < 32 || *cursor == 127) std::fprintf(stderr, "\\x%02x", static_cast<unsigned>(*cursor));
            else std::fputc(*cursor, stderr);
        }
        std::fputc('"', stderr);
    }
    void reportLocked()
    {
        if (!state.functions) return;
        std::fputs("Aevix @bench: wall time; sampled process RSS (not per-function allocations); arithmetic sample mean; nested time is inclusive; interval_ms=10\n", stderr);
        if (!state.samplerStarted) std::fputs("Aevix @bench: periodic sampler unavailable; memory uses entry/exit samples only.\n", stderr);
        for (auto* function = state.functions; function; function = function->next)
        {
            std::fputs("  function=", stderr); quotedName(function->name);
            const auto total = static_cast<long double>(function->totalNanoseconds) / 1000000.0L;
            const auto maximum = static_cast<long double>(function->maximumNanoseconds) / 1000000.0L;
            const auto mean = function->calls ? total / static_cast<long double>(function->calls) : 0.0L;
            std::fprintf(stderr, " calls=%llu time_total_ms=%.6Lf time_mean_ms=%.6Lf time_max_ms=%.6Lf", static_cast<unsigned long long>(function->calls), total, mean, maximum);
            if (function->samples)
                std::fprintf(stderr, " memory_peak_mib=%.6Lf memory_mean_mib=%.6Lf samples=%llu", static_cast<long double>(function->residentPeak) / 1048576.0L, function->residentSum / static_cast<long double>(function->samples) / 1048576.0L, static_cast<unsigned long long>(function->samples));
            else std::fputs(" memory_peak_mib=unavailable memory_mean_mib=unavailable samples=0", stderr);
            std::fprintf(stderr, " rss_unavailable_samples=%llu active_calls=%llu\n", static_cast<unsigned long long>(function->unavailableSamples), static_cast<unsigned long long>(function->active));
        }
        state.reportPrinted = true; state.reportedGeneration = state.generation;
        std::fflush(stderr);
    }
    void completeLocked(Invocation* invocation, std::uint64_t now)
    {
        auto* function = invocation->function;
        const auto elapsed = now >= invocation->start ? now - invocation->start : std::uint64_t{0};
        function->calls = saturatingAdd(function->calls, 1);
        function->totalNanoseconds = saturatingAdd(function->totalNanoseconds, elapsed);
        if (elapsed > function->maximumNanoseconds) function->maximumNanoseconds = elapsed;
        if (function->active) --function->active;
        state.generation = saturatingAdd(state.generation, 1);
    }
    void shutdown()
    {
        lock();
        if (state.shuttingDown) { unlock(); return; }
        sampleLocked(); state.shuttingDown = true; wake();
        const bool joinSampler = state.samplerStarted;
        unlock();
        if (joinSampler)
        {
#if defined(_WIN32)
            WaitForSingleObject(state.sampler, INFINITE); CloseHandle(state.sampler);
#else
            pthread_join(state.sampler, nullptr);
#endif
        }
        lock();
        const auto now = monotonicNanoseconds();
        while (state.invocations)
        {
            auto* invocation = state.invocations; state.invocations = invocation->next;
            completeLocked(invocation, now); std::free(invocation);
        }
        if (!state.reportPrinted || state.reportedGeneration != state.generation) reportLocked();
        while (state.functions) { auto* function = state.functions; state.functions = function->next; std::free(function); }
        state.lastFunction = nullptr;
        unlock();
    }
    bool initializeLocked()
    {
        if (state.shuttingDown) return false;
        if (state.initialized) return true;
        if (std::atexit(shutdown) != 0) { warnMetadataLocked(); state.shuttingDown = true; return false; }
        state.initialized = true;
#if defined(_WIN32)
        state.sampler = CreateThread(nullptr, 0, sampleThread, nullptr, 0, nullptr);
        state.samplerStarted = state.sampler != nullptr;
#else
        state.samplerStarted = pthread_create(&state.sampler, nullptr, sampleThread, nullptr) == 0;
#endif
        return true;
    }
}

extern "C" std::uint64_t aevix_bench_begin(const char* name)
{
    Locked guard;
    if (!initializeLocked()) return 0;
    if (!name) name = "<unnamed>";
    std::size_t length = 0;
    while (length <= maximumNameBytes && name[length]) ++length;
    if (length > maximumNameBytes) { warnMetadataLocked(); return 0; }
    auto* function = state.functions;
    while (function && std::strcmp(function->name, name) != 0) function = function->next;
    if (!function)
    {
        if (state.functionCount == maximumFunctions) { warnMetadataLocked(); return 0; }
        function = static_cast<Function*>(std::calloc(1, sizeof(Function) + length + 1));
        if (!function) { warnMetadataLocked(); return 0; }
        auto* copy = reinterpret_cast<char*>(function + 1); std::memcpy(copy, name, length + 1); function->name = copy;
        if (state.lastFunction) state.lastFunction->next = function; else state.functions = function;
        state.lastFunction = function; ++state.functionCount;
    }
    auto* invocation = static_cast<Invocation*>(std::malloc(sizeof(Invocation)));
    if (!invocation) { warnMetadataLocked(); return 0; }
    std::uint64_t token = 0;
    do
    {
        token = ++state.nextToken;
        if (!token) token = ++state.nextToken;
        for (auto* existing = state.invocations; existing; existing = existing->next)
            if (existing->token == token) { token = 0; break; }
    } while (!token);
    const bool idle = state.invocations == nullptr;
    new (invocation) Invocation{state.invocations, function, monotonicNanoseconds(), token};
    state.invocations = invocation; ++function->active;
    sampleLocked();
    if (idle) wake();
    return token;
}
extern "C" void aevix_bench_end(std::uint64_t token)
{
    if (!token) return;
    const auto now = monotonicNanoseconds();
    Locked guard;
    auto** link = &state.invocations;
    while (*link && (*link)->token != token) link = &(*link)->next;
    if (!*link) return;
    sampleLocked();
    auto* invocation = *link; *link = invocation->next;
    completeLocked(invocation, now); std::free(invocation);
    if (!state.invocations) wake();
}
extern "C" void aevix_bench_report()
{
    Locked guard;
    if (state.shuttingDown) return;
    reportLocked();
}
