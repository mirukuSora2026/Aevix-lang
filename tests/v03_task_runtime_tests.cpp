// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_task_runtime_tests.cpp
//
// Description :
//     네이티브 작업의 생성 실패, 캡처·결과 소유권, 취소 경쟁과 회수·종료 경계를 검증한다.
//     Verify native task creation failures, capture/result ownership, cancellation races, reaping, and shutdown boundaries.
//
// ========================
#include "aevix/Runtime/TaskRuntime.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>

#if !defined(AEVIX_TASK_RUNTIME_TESTING)
#error "Compile this dedicated test with AEVIX_TASK_RUNTIME_TESTING."
#endif

namespace
{
    constexpr std::int32_t ok = 0, allocation = 1, overflow = 2, invalid = 3, stale = 4, badState = 5, threadError = 6,
                           deadlock = 7, notReady = 8, cancelled = 9, failed = 10, timedOut = 11;
    unsigned checks = 0;
    bool require(bool condition, const char *expression, int line)
    {
        ++checks;
        if (!condition)
            std::fprintf(stderr, "Task runtime check failed at %d: %s\n", line, expression);
        return condition;
    }
#define REQUIRE(...)                                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!require(bool(__VA_ARGS__), #__VA_ARGS__, __LINE__))                                                       \
            return false;                                                                                              \
    } while (false)
    struct Deadline final
    {
        std::atomic<bool> done{false};
        std::thread worker;
        Deadline()
            : worker([this] {
                  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                  while (!done.load())
                  {
                      if (std::chrono::steady_clock::now() > limit)
                      {
                          std::fputs("Task runtime test exceeded its bounded dependency deadline.\n", stderr);
                          std::_Exit(91);
                      }
                      std::this_thread::sleep_for(std::chrono::milliseconds(5));
                  }
              })
        {
        }
        ~Deadline()
        {
            done = true;
            worker.join();
        }
    };
    struct Counters final
    {
        std::atomic<unsigned> invokes{0}, captures{0}, results{0};
        std::atomic<bool> good{true}, entered{false}, proceed{false};
        std::atomic<std::uint64_t> owner{0};
    };
    struct Capture final
    {
        Counters *counts;
        int *owned;
        std::uint64_t value;
        unsigned mode;
    };
    struct Result final
    {
        Counters *counts = nullptr;
        int *owned = nullptr;
        std::uint64_t value = 0;
        bool reentrant = false;
    };
    void dropCapture(void *storage) noexcept
    {
        auto &capture = *static_cast<Capture *>(storage);
        delete capture.owned;
        capture.owned = nullptr;
        ++capture.counts->captures;
    }
    void dropResult(void *storage) noexcept
    {
        auto &result = *static_cast<Result *>(storage);
        if (result.reentrant && aevix_task_cleanup(result.counts->owner.load()) != stale)
            result.counts->good = false;
        if (result.reentrant && aevix_task_runtime_drain() != deadlock)
            result.counts->good = false;
        delete result.owned;
        result.owned = nullptr;
        ++result.counts->results;
    }
    std::int32_t invoke(void *captures, void *result, const aevix::TaskContext *context) noexcept
    {
        auto &capture = *static_cast<Capture *>(captures);
        auto &counts = *capture.counts;
        ++counts.invokes;
        counts.entered = true;
        if (capture.mode == 1)
            while (!counts.proceed.load())
                std::this_thread::yield();
        if (capture.mode == 2)
        {
            while (!aevix_task_context_cancel_requested(context))
                std::this_thread::yield();
            dropCapture(captures);
            return 1;
        }
        if (capture.mode == 3)
        {
            dropCapture(captures);
            return 2;
        }
        if (capture.mode == 4)
        {
            Result unused;
            const auto owner = counts.owner.load();
            if (aevix_task_wait(owner) != deadlock || aevix_task_cleanup(owner) != deadlock ||
                aevix_task_try_take(owner, &unused, sizeof unused) != deadlock ||
                aevix_task_runtime_drain() != deadlock)
                counts.good = false;
        }
        const auto value = capture.value;
        const auto reentrant = capture.mode == 5;
        dropCapture(captures);
        auto *owned = new (std::nothrow) int(64);
        if (!owned)
            std::abort();
        new (result) Result{&counts, owned, value, reentrant};
        return 0;
    }
    bool prepare(Counters &counts, std::uint64_t value, unsigned mode, std::uint64_t &owner, bool commit = true,
                 void **saved = nullptr)
    {
        void *storage = nullptr;
        REQUIRE(aevix_task_prepare(sizeof(Capture), alignof(Capture), sizeof(Result), alignof(Result), invoke,
                                   dropCapture, dropResult, &owner, &storage) == ok);
        auto *resource = new (std::nothrow) int(37);
        REQUIRE(resource);
        new (storage) Capture{&counts, resource, value, mode};
        counts.owner = owner;
        if (saved)
            *saved = storage;
        if (commit)
            REQUIRE(aevix_task_commit(owner) == ok);
        return true;
    }
    bool failedCreation()
    {
        std::uint64_t owner = 91;
        void *storage = reinterpret_cast<void *>(std::uintptr_t{1});
        REQUIRE(aevix_task_prepare(1, 0, 1, 1, invoke, dropCapture, dropResult, &owner, &storage) == invalid &&
                !owner && !storage);
        REQUIRE(aevix_task_prepare(1, 3, 1, 1, invoke, dropCapture, dropResult, &owner, &storage) == invalid);
        REQUIRE(aevix_task_prepare(1, 1, UINT64_MAX, 1, invoke, dropCapture, dropResult, &owner, &storage) == overflow);
        REQUIRE(aevix_task_prepare(UINT64_MAX, 1, 1, 1, invoke, dropCapture, dropResult, &owner, &storage) == overflow);
        REQUIRE(aevix_task_prepare(1, 1, 1, 1, nullptr, dropCapture, dropResult, &owner, &storage) == invalid);
        REQUIRE(aevix_task_prepare(1, 1, 1, 1, invoke, dropCapture, dropResult, nullptr, &storage) == invalid);
        REQUIRE(aevix_task_prepare(1, 1, 1, 1, invoke, dropCapture, dropResult, &owner, nullptr) == invalid);
        Counters counts;
        auto *original = new (std::nothrow) int(91);
        REQUIRE(original);
        Capture input{&counts, original, 1, 0};
        for (std::uint64_t allowed : {0u, 1u, 2u, 3u, 4u})
        {
            aevix_task_test_fail_after(allowed);
            const auto result = aevix_task_prepare(sizeof(Capture), alignof(Capture), sizeof(Result), alignof(Result),
                                                   invoke, dropCapture, dropResult, &owner, &storage);
            aevix_task_test_fail_after(UINT64_MAX);
            REQUIRE(result == allocation && !owner && !storage);
            REQUIRE(input.owned == original && *input.owned == 91 && counts.invokes == 0 && counts.captures == 0 &&
                    counts.results == 0);
            REQUIRE(aevix_task_test_live_controls() == 0);
        }
        aevix_task_test_fail_threads(1);
        const auto result = aevix_task_prepare(sizeof(Capture), alignof(Capture), sizeof(Result), alignof(Result),
                                               invoke, dropCapture, dropResult, &owner, &storage);
        const auto native = aevix_task_last_error(), domain = aevix_task_error_domain();
        aevix_task_test_fail_threads(0);
        REQUIRE(result == threadError && native && domain == 1 && !owner && !storage);
        REQUIRE(*input.owned == 91 && counts.invokes == 0 && counts.captures == 0 &&
                aevix_task_test_live_controls() == 0);
        dropCapture(&input);
        REQUIRE(counts.captures == 1);
        return true;
    }
    bool abortAndGating()
    {
        std::uint64_t owner = 0;
        void *storage = nullptr;
        REQUIRE(aevix_task_prepare(sizeof(Capture), alignof(Capture), sizeof(Result), alignof(Result), invoke,
                                   dropCapture, dropResult, &owner, &storage) == ok);
        aevix::TaskSnapshot snapshot;
        REQUIRE(aevix_task_snapshot(owner, &snapshot) == ok && snapshot.phase == 0);
        Result unused;
        REQUIRE(aevix_task_join(owner, &unused, sizeof unused) == badState);
        REQUIRE(aevix_task_try_take(owner, &unused, sizeof unused) == badState);
        REQUIRE(aevix_task_wait(owner) == badState && aevix_task_wait_timeout(owner, 0) == badState);
        REQUIRE(aevix_task_cleanup(owner) == badState);
        REQUIRE(aevix_task_abort(owner) == ok && aevix_task_test_live_controls() == 0);
        const auto staleOwner = owner;
        REQUIRE(aevix_task_abort(owner) == stale);
        Counters counts;
        REQUIRE(prepare(counts, 11, 0, owner, false));
        REQUIRE(owner != staleOwner && counts.invokes == 0 && counts.captures == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE(counts.invokes == 0);
        aevix_task_test_fail_after(0);
        const auto committed = aevix_task_commit(owner);
        aevix_task_test_fail_after(UINT64_MAX);
        REQUIRE(committed == ok);
        REQUIRE(aevix_task_commit(owner) == badState && aevix_task_abort(owner) == badState);
        REQUIRE(aevix_task_wait(owner) == ok && aevix_task_wait(owner) == ok);
        REQUIRE(counts.invokes == 1 && counts.captures == 1 && counts.results == 0);
        Result output;
        REQUIRE(aevix_task_join(owner, &output, sizeof output) == ok && output.value == 11 && *output.owned == 64);
        REQUIRE(aevix_task_join(owner, &unused, sizeof unused) == stale && aevix_task_cleanup(owner) == stale);
        dropResult(&output);
        REQUIRE(counts.results == 1 && aevix_task_test_live_controls() == 0);
        return true;
    }
    bool resultOwnership()
    {
        Counters counts;
        std::uint64_t owner = 0;
        void *captureStorage = nullptr;
        REQUIRE(prepare(counts, 17, 0, owner, true, &captureStorage));
        REQUIRE(aevix_task_wait(owner) == ok);
        Result output;
        REQUIRE(aevix_task_join(owner, nullptr, sizeof output) == invalid);
        REQUIRE(aevix_task_join(owner, &output, sizeof output - 1) == invalid && !output.owned);
        REQUIRE(aevix_task_join(owner, captureStorage, sizeof output) == invalid);
        REQUIRE(aevix_task_join(owner, reinterpret_cast<void *>(UINTPTR_MAX - 1), sizeof output) == overflow);
        REQUIRE(counts.results == 0);
        REQUIRE(aevix_task_cleanup(owner) == ok && counts.captures == 1 && counts.results == 1);
        REQUIRE(aevix_task_cleanup(owner) == stale && counts.results == 1);
        Counters reentrant;
        REQUIRE(prepare(reentrant, 18, 5, owner));
        REQUIRE(aevix_task_cleanup(owner) == ok && reentrant.good && reentrant.captures == 1 && reentrant.results == 1);
        Counters selfChecks;
        REQUIRE(prepare(selfChecks, 19, 4, owner));
        REQUIRE(aevix_task_join(owner, &output, sizeof output) == ok && selfChecks.good && selfChecks.captures == 1);
        dropResult(&output);
        REQUIRE(selfChecks.results == 1 && aevix_task_test_live_controls() == 0);
        return true;
    }
    bool cancellation()
    {
        Counters beforeStart;
        std::uint64_t owner = 0;
        REQUIRE(prepare(beforeStart, 1, 0, owner, false));
        std::uint32_t disposition = 99;
        REQUIRE(aevix_task_request_cancel(owner, &disposition) == ok && disposition == 0);
        REQUIRE(aevix_task_request_cancel(owner, &disposition) == ok && disposition == 2);
        REQUIRE(aevix_task_commit(owner) == ok);
        Result output;
        REQUIRE(aevix_task_join(owner, &output, sizeof output) == cancelled && !output.owned);
        REQUIRE(beforeStart.invokes == 0 && beforeStart.captures == 1 && beforeStart.results == 0);
        Counters cooperative;
        REQUIRE(prepare(cooperative, 2, 2, owner));
        while (!cooperative.entered.load())
            std::this_thread::yield();
        REQUIRE(aevix_task_wait_timeout(owner, 0) == timedOut);
        REQUIRE(aevix_task_try_take(owner, &output, sizeof output) == notReady && !output.owned);
        REQUIRE(aevix_task_request_cancel(owner, &disposition) == ok && disposition == 0);
        REQUIRE(aevix_task_join(owner, &output, sizeof output) == cancelled);
        REQUIRE(cooperative.invokes == 1 && cooperative.captures == 1 && cooperative.results == 0);
        Counters ignored;
        REQUIRE(prepare(ignored, 3, 1, owner));
        while (!ignored.entered.load())
            std::this_thread::yield();
        REQUIRE(aevix_task_request_cancel(owner, &disposition) == ok && disposition == 0);
        REQUIRE(aevix_task_wait_timeout(owner, 1000000) == timedOut && ignored.captures == 0);
        ignored.proceed = true;
        REQUIRE(aevix_task_wait(owner) == ok);
        REQUIRE(aevix_task_request_cancel(owner, &disposition) == ok && disposition == 1);
        REQUIRE(aevix_task_join(owner, &output, sizeof output) == ok && output.value == 3);
        dropResult(&output);
        REQUIRE(ignored.captures == 1 && ignored.results == 1);
        Counters explicitlyFailed;
        REQUIRE(prepare(explicitlyFailed, 4, 3, owner));
        REQUIRE(aevix_task_join(owner, &output, sizeof output) == failed);
        REQUIRE(explicitlyFailed.invokes == 1 && explicitlyFailed.captures == 1 && explicitlyFailed.results == 0);
        REQUIRE(aevix_task_context_cancel_requested(nullptr) == 0);
        return true;
    }
    bool reapFailures()
    {
        Counters counts;
        std::uint64_t owner = 0;
        REQUIRE(prepare(counts, 55, 0, owner));
        REQUIRE(aevix_task_wait_timeout(owner, 1000000000) == ok);
        Result output;
        aevix_task_test_fail_reap(2);
        const auto failedTake = aevix_task_try_take(owner, &output, sizeof output);
        const auto code = aevix_task_last_error();
        aevix_task_test_fail_reap(0);
        REQUIRE(failedTake == threadError && code && !output.owned && counts.results == 0);
        REQUIRE(aevix_task_join(owner, &output, sizeof output) == ok && output.value == 55);
        dropResult(&output);
        REQUIRE(counts.results == 1);
        Counters blocked;
        REQUIRE(prepare(blocked, 56, 1, owner));
        while (!blocked.entered.load())
            std::this_thread::yield();
        aevix_task_test_fail_reap(1);
        const auto failedJoin = aevix_task_join(owner, &output, sizeof output);
        aevix_task_test_fail_reap(0);
        REQUIRE(failedJoin == threadError && blocked.captures == 0);
        aevix::TaskSnapshot snapshot;
        REQUIRE(aevix_task_snapshot(owner, &snapshot) == ok && snapshot.phase == 2);
        blocked.proceed = true;
        REQUIRE(aevix_task_cleanup(owner) == ok && blocked.captures == 1 && blocked.results == 1);
        return true;
    }
    struct alignas(256) AlignedCapture final
    {
        Counters *counts;
        std::array<unsigned char, 257> bytes;
    };
    struct alignas(512) AlignedResult final
    {
        Counters *counts = nullptr;
        std::array<unsigned char, 513> bytes{};
    };
    void dropAlignedCapture(void *storage) noexcept
    {
        ++static_cast<AlignedCapture *>(storage)->counts->captures;
    }
    void dropAlignedResult(void *storage) noexcept
    {
        ++static_cast<AlignedResult *>(storage)->counts->results;
    }
    std::int32_t invokeAligned(void *input, void *output, const aevix::TaskContext *) noexcept
    {
        auto &source = *static_cast<AlignedCapture *>(input);
        auto &counts = *source.counts;
        if (reinterpret_cast<std::uintptr_t>(input) % 256 || reinterpret_cast<std::uintptr_t>(output) % 512)
            counts.good = false;
        auto *result = new (output) AlignedResult{};
        result->counts = &counts;
        result->bytes[512] = source.bytes[256];
        dropAlignedCapture(input);
        ++counts.invokes;
        return 0;
    }
    std::atomic<unsigned> unitInvokes{0}, unitCaptures{0}, unitResults{0};
    void dropUnitCapture(void *) noexcept
    {
        ++unitCaptures;
    }
    void dropUnitResult(void *) noexcept
    {
        ++unitResults;
    }
    std::int32_t invokeUnit(void *, void *, const aevix::TaskContext *) noexcept
    {
        ++unitInvokes;
        ++unitCaptures;
        return 0;
    }
    bool layouts()
    {
        Counters counts;
        std::uint64_t owner = 0;
        void *storage = nullptr;
        REQUIRE(aevix_task_prepare(sizeof(AlignedCapture), alignof(AlignedCapture), sizeof(AlignedResult),
                                   alignof(AlignedResult), invokeAligned, dropAlignedCapture, dropAlignedResult, &owner,
                                   &storage) == ok);
        REQUIRE(reinterpret_cast<std::uintptr_t>(storage) % 256 == 0);
        auto *capture = new (storage) AlignedCapture{&counts, {}};
        capture->bytes[256] = 0xa7;
        REQUIRE(aevix_task_commit(owner) == ok);
        AlignedResult result;
        REQUIRE(aevix_task_join(owner, &result, sizeof result) == ok && result.bytes[512] == 0xa7 && counts.good &&
                counts.captures == 1);
        dropAlignedResult(&result);
        REQUIRE(counts.results == 1);
        REQUIRE(aevix_task_prepare(0, 4096, 0, 8192, invokeUnit, dropUnitCapture, dropUnitResult, &owner, &storage) ==
                ok);
        REQUIRE(reinterpret_cast<std::uintptr_t>(storage) % 4096 == 0);
        REQUIRE(aevix_task_commit(owner) == ok && aevix_task_join(owner, nullptr, 0) == ok);
        REQUIRE(unitInvokes == 1 && unitCaptures == 1 && unitResults == 0);
        REQUIRE(aevix_task_prepare(0, 1, 0, 1, invokeUnit, dropUnitCapture, dropUnitResult, &owner, &storage) == ok);
        REQUIRE(aevix_task_commit(owner) == ok && aevix_task_cleanup(owner) == ok);
        REQUIRE(unitInvokes == 2 && unitCaptures == 2 && unitResults == 1);
        return true;
    }
    bool independentTasks()
    {
        Counters first, second;
        std::uint64_t a = 0, b = 0;
        REQUIRE(prepare(first, 71, 1, a));
        while (!first.entered.load())
            std::this_thread::yield();
        Result outputA, outputB;
        std::atomic<std::int32_t> joined{99};
        std::atomic<bool> joining{false};
        std::thread waiter([&] {
            joining = true;
            joined = aevix_task_join(a, &outputA, sizeof outputA);
        });
        while (!joining.load())
            std::this_thread::yield();
        while (!aevix_task_test_is_reaping(a))
            std::this_thread::yield();
        aevix::TaskSnapshot snapshot;
        const auto observed = aevix_task_snapshot(a, &snapshot);
        const auto created = prepare(second, 72, 0, b);
        const auto secondJoined = aevix_task_join(b, &outputB, sizeof outputB);
        first.proceed = true;
        waiter.join();
        REQUIRE(observed == ok && snapshot.phase == 2 && created && secondJoined == ok && joined == ok);
        REQUIRE(outputA.value == 71 && outputB.value == 72);
        dropResult(&outputA);
        dropResult(&outputB);
        REQUIRE(first.captures == 1 && second.captures == 1 && first.results == 1 && second.results == 1);
        return true;
    }
    bool cancellationRaces()
    {
        for (unsigned round = 0; round < 100; ++round)
        {
            Counters counts;
            std::uint64_t owner = 0;
            REQUIRE(prepare(counts, round, 0, owner));
            std::uint32_t disposition = 0;
            REQUIRE(aevix_task_request_cancel(owner, &disposition) == ok);
            Result output;
            const auto result = aevix_task_join(owner, &output, sizeof output);
            REQUIRE((result == ok || result == cancelled) && counts.captures == 1 && counts.invokes <= 1);
            if (result == ok)
            {
                const auto value = output.value;
                dropResult(&output);
                REQUIRE(value == round && counts.results == 1);
            }
            else
                REQUIRE(!output.owned && counts.results == 0);
        }
        return true;
    }
    bool concurrentConsumers()
    {
        Counters counts;
        std::uint64_t owner = 0;
        REQUIRE(prepare(counts, 90, 0, owner));
        REQUIRE(aevix_task_wait(owner) == ok);
        Result first, second;
        std::atomic<bool> start{false};
        std::int32_t one = 99, two = 99;
        std::thread a([&] {
            while (!start.load())
                std::this_thread::yield();
            one = aevix_task_join(owner, &first, sizeof first);
        });
        std::thread b([&] {
            while (!start.load())
                std::this_thread::yield();
            two = aevix_task_join(owner, &second, sizeof second);
        });
        start = true;
        a.join();
        b.join();
        REQUIRE((one == ok && two == stale) || (two == ok && one == stale));
        if (one == ok)
        {
            REQUIRE(!second.owned);
            dropResult(&first);
        }
        else
        {
            REQUIRE(!first.owned);
            dropResult(&second);
        }
        REQUIRE(counts.captures == 1 && counts.results == 1);
        return true;
    }
    bool nonblockingAndDrain()
    {
        aevix_task_test_hold_worker_tail(1);
        Counters counts, unclaimed;
        std::uint64_t owner = 0, unclaimedOwner = 0;
        REQUIRE(prepare(counts, 101, 0, owner));
        REQUIRE(aevix_task_wait_timeout(owner, 1000000000) == ok);
        Result output;
        const auto transferred = aevix_task_try_take(owner, &output, sizeof output);
        REQUIRE(transferred == ok && output.value == 101 && counts.captures == 1 && counts.results == 0);
        REQUIRE(aevix_task_snapshot(owner, nullptr) == invalid);
        REQUIRE(aevix_task_test_live_controls() == 1);
        dropResult(&output);
        REQUIRE(prepare(unclaimed, 102, 0, unclaimedOwner));
        REQUIRE(aevix_task_wait_timeout(unclaimedOwner, 1000000000) == ok && unclaimed.captures == 1 &&
                unclaimed.results == 0);
        std::atomic<bool> draining{false}, drained{false};
        std::atomic<std::int32_t> result{99};
        std::thread shutdown([&] {
            draining = true;
            result = aevix_task_runtime_drain();
            drained = true;
        });
        while (!draining.load())
            std::this_thread::yield();
        REQUIRE(!drained.load() && aevix_task_test_live_controls() == 2);
        aevix_task_test_hold_worker_tail(0);
        shutdown.join();
        REQUIRE(drained && result == ok && counts.results == 1 && unclaimed.results == 1 &&
                aevix_task_test_live_controls() == 0);
        void *storage = nullptr;
        REQUIRE(aevix_task_prepare(0, 1, 0, 1, invokeUnit, dropUnitCapture, dropUnitResult, &owner, &storage) ==
                    badState &&
                !owner && !storage);
        REQUIRE(aevix_task_runtime_drain() == ok && aevix_task_cleanup(0) == ok);
        return true;
    }
}
int main()
{
    Deadline deadline;
    if (!failedCreation() || !abortAndGating() || !resultOwnership() || !cancellation() || !reapFailures() ||
        !layouts() || !independentTasks() || !cancellationRaces() || !concurrentConsumers() || !nonblockingAndDrain())
        return 1;
    std::printf(
        "Task runtime: %u checks, 100 cancellation races, exact ownership and nonblocking terminal reaping passed.\n",
        checks);
    return 0;
}
