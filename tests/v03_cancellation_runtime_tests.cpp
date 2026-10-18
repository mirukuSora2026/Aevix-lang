// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_cancellation_runtime_tests.cpp
//
// Description :
//     작업과 독립적인 취소 소유자의 역할·실패 보존·참조 수·회수 경쟁을 검증한다.
//     Verify independent cancellation-owner roles, failure preservation, counts, and cleanup races.
//
// ========================
#include "aevix/Runtime/CancellationRuntime.hpp"
#include "aevix/Runtime/TaskScopeRuntime.hpp"
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
    constexpr int ok = 0, allocation = 1, overflow = 2, invalid = 3, stale = 4;
    unsigned checks = 0;
    bool require(bool value, const char *expression, int line)
    {
        ++checks;
        if (!value)
            std::fprintf(stderr, "Cancellation check failed at %d: %s\n", line, expression);
        return value;
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
                  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(40);
                  while (!done.load())
                  {
                      if (std::chrono::steady_clock::now() > end)
                      {
                          std::fputs("Cancellation test deadline exceeded.\n", stderr);
                          std::_Exit(93);
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
    struct Counts final
    {
        std::atomic<unsigned> captures{0}, results{0};
        std::atomic<bool> entered{false};
    };
    struct Capture final
    {
        Counts *counts;
        int *owned;
        bool wait;
    };
    struct Result final
    {
        Counts *counts = nullptr;
        int *owned = nullptr;
    };
    void dropCapture(void *storage) noexcept
    {
        auto &value = *static_cast<Capture *>(storage);
        delete value.owned;
        value.owned = nullptr;
        ++value.counts->captures;
    }
    void dropResult(void *storage) noexcept
    {
        auto &value = *static_cast<Result *>(storage);
        delete value.owned;
        value.owned = nullptr;
        ++value.counts->results;
    }
    std::int32_t invoke(void *input, void *output, const aevix::TaskContext *context) noexcept
    {
        auto &value = *static_cast<Capture *>(input);
        value.counts->entered = true;
        if (value.wait)
        {
            while (!aevix_task_context_cancel_requested(context))
                std::this_thread::yield();
            dropCapture(input);
            return 1;
        }
        new (output) Result{value.counts, value.owned};
        value.owned = nullptr;
        ++value.counts->captures;
        return 0;
    }
    int prepare(Counts &counts, bool wait, std::uint64_t &owner, std::uint64_t scope = 0)
    {
        void *storage = nullptr;
        const auto result =
            scope ? aevix_task_scope_prepare(scope, sizeof(Capture), alignof(Capture), sizeof(Result), alignof(Result),
                                             invoke, dropCapture, dropResult, &owner, &storage)
                  : aevix_task_prepare(sizeof(Capture), alignof(Capture), sizeof(Result), alignof(Result), invoke,
                                       dropCapture, dropResult, &owner, &storage);
        if (result)
            return result;
        new (storage) Capture{&counts, new int(17), wait};
        return scope ? aevix_scoped_task_commit(owner) : aevix_task_commit(owner);
    }
    bool allocationAndRoles()
    {
        std::uint64_t source = 99;
        REQUIRE(aevix_cancel_source_new(nullptr) == invalid);
        for (std::uint64_t count : {0u, 1u})
        {
            aevix_task_test_fail_after(count);
            const auto result = aevix_cancel_source_new(&source);
            aevix_task_test_fail_after(UINT64_MAX);
            REQUIRE(result == allocation && !source && !aevix_cancel_test_live_states());
        }
        REQUIRE(aevix_cancel_source_new(&source) == ok && aevix_cancel_test_owner_count(source) == 1);
        std::uint64_t copy = 99, token = 99, tokenCopy = 99;
        aevix_task_test_fail_after(0);
        const auto cloneFailed = aevix_cancel_source_clone(source, &copy),
                   deriveFailed = aevix_cancel_token_from_source(source, &token);
        aevix_task_test_fail_after(UINT64_MAX);
        REQUIRE(cloneFailed == allocation && deriveFailed == allocation && !copy && !token &&
                aevix_cancel_test_owner_count(source) == 1);
        REQUIRE(aevix_cancel_source_clone(source, &copy) == ok &&
                aevix_cancel_token_from_source(source, &token) == ok &&
                aevix_cancel_token_clone(token, &tokenCopy) == ok);
        REQUIRE(aevix_cancel_test_owner_count(source) == 4);
        std::uint64_t rejected = 99;
        std::uint32_t requested = 99;
        REQUIRE(aevix_cancel_source_clone(token, &rejected) == stale && !rejected);
        REQUIRE(aevix_cancel_token_from_source(token, &rejected) == stale && !rejected);
        REQUIRE(aevix_cancel_token_clone(source, &rejected) == stale && !rejected);
        REQUIRE(aevix_cancel_source_request(token, &requested) == stale && !requested);
        REQUIRE(aevix_cancel_token_requested(source, &requested) == stale && !requested);
        REQUIRE(aevix_cancel_source_release(token) == stale && aevix_cancel_token_release(source) == stale);
        const auto forged = UINT64_C(0xd000000000000000) | (source & UINT64_C(0x0fffffffffffffff));
        REQUIRE(aevix_cancel_token_requested(forged, &requested) == stale);
        REQUIRE(aevix_cancel_token_requested(token + UINT64_C(0x100000000), &requested) == stale);
        REQUIRE(aevix_cancel_source_clone(source, nullptr) == invalid &&
                aevix_cancel_token_requested(token, nullptr) == invalid);
        REQUIRE(aevix_cancel_token_requested(token, &requested) == ok && !requested);
        REQUIRE(aevix_cancel_source_request(copy, &requested) == ok && !requested);
        REQUIRE(aevix_cancel_source_request(source, &requested) == ok && requested == 1);
        REQUIRE(aevix_cancel_source_release(source) == ok && aevix_cancel_source_release(copy) == ok);
        REQUIRE(aevix_cancel_token_requested(tokenCopy, &requested) == ok && requested == 1 &&
                aevix_cancel_test_owner_count(token) == 2);
        REQUIRE(aevix_cancel_token_release(token) == ok && aevix_cancel_token_release(token) == stale &&
                aevix_cancel_token_release(tokenCopy) == ok);
        REQUIRE(aevix_cancel_test_live_states() == 0 && aevix_cancel_source_release(0) == ok &&
                aevix_cancel_token_release(0) == ok);
        std::uint64_t replacement = 0;
        REQUIRE(aevix_cancel_source_new(&replacement) == ok);
        REQUIRE(std::uint32_t(replacement) == std::uint32_t(tokenCopy) && replacement != tokenCopy);
        REQUIRE(aevix_cancel_token_requested(tokenCopy, &requested) == stale &&
                aevix_cancel_source_release(replacement) == ok);
        return true;
    }
    bool countsAndCachedAllocation()
    {
        aevix_cancel_test_owner_limit(1);
        std::uint64_t source = 0, token = 99, clone = 99;
        REQUIRE(aevix_cancel_source_new(&source) == ok);
        REQUIRE(aevix_cancel_token_from_source(source, &token) == overflow && !token);
        REQUIRE(aevix_cancel_source_clone(source, &clone) == overflow && !clone &&
                aevix_cancel_test_owner_count(source) == 1);
        aevix_cancel_test_owner_limit(2);
        REQUIRE(aevix_cancel_token_from_source(source, &token) == ok && aevix_cancel_test_owner_count(token) == 2);
        REQUIRE(aevix_cancel_token_clone(token, &clone) == overflow && !clone);
        REQUIRE(aevix_cancel_source_release(source) == ok && aevix_cancel_token_clone(token, &clone) == ok);
        REQUIRE(aevix_cancel_token_release(token) == ok && aevix_cancel_token_release(clone) == ok);
        aevix_cancel_test_owner_limit(UINT64_MAX);
        REQUIRE(aevix_cancel_source_new(&source) == ok);
        std::array<std::uint64_t, 64> held{};
        unsigned count = 0;
        int last = 0;
        aevix_task_test_fail_after(0);
        for (; count < held.size(); ++count)
        {
            last = aevix_cancel_token_from_source(source, &held[count]);
            if (last)
                break;
        }
        aevix_task_test_fail_after(UINT64_MAX);
        REQUIRE(last == allocation && count < held.size() && !held[count] &&
                aevix_cancel_test_owner_count(source) == count + 1);
        for (unsigned index = 0; index < count; ++index)
            REQUIRE(aevix_cancel_token_release(held[index]) == ok);
        REQUIRE(aevix_cancel_source_release(source) == ok && !aevix_cancel_test_live_states());
        return true;
    }
    bool taskStateAndScopeRetention()
    {
        Counts counts;
        std::uint64_t task = 0, token = 99;
        REQUIRE(prepare(counts, true, task) == ok);
        while (!counts.entered.load())
            std::this_thread::yield();
        aevix_cancel_test_owner_limit(1);
        REQUIRE(aevix_task_cancel_token(task, &token) == overflow && !token && aevix_task_test_live_controls() == 1);
        aevix_cancel_test_owner_limit(UINT64_MAX);
        REQUIRE(aevix_task_cancel_token(task, &token) == ok && aevix_cancel_test_owner_count(token) == 2);
        std::uint32_t requested = 99;
        REQUIRE(aevix_cancel_token_requested(token, &requested) == ok && !requested);
        REQUIRE(aevix_cancel_source_request(token, &requested) == stale);
        REQUIRE(aevix_task_request_cancel(task, &requested) == ok);
        Result output;
        REQUIRE(aevix_task_join(task, &output, sizeof output) == 9 && counts.captures == 1 && !counts.results);
        REQUIRE(aevix_task_test_live_controls() == 0 && aevix_cancel_test_owner_count(token) == 1);
        REQUIRE(aevix_cancel_token_requested(token, &requested) == ok && requested == 1);
        std::uint64_t rejected = 99;
        REQUIRE(aevix_task_cancel_token(task, &rejected) == stale && !rejected);
        REQUIRE(aevix_cancel_token_release(token) == ok && !aevix_cancel_test_live_states());
        std::uint64_t scope = 0, handle = 0;
        Counts scoped;
        REQUIRE(aevix_task_scope_open(&scope) == ok && prepare(scoped, false, handle, scope) == ok);
        REQUIRE(aevix_scoped_task_cancel_token(scope, &token) == stale && !token);
        REQUIRE(aevix_scoped_task_cancel_token(handle, &token) == ok);
        REQUIRE(aevix_scoped_task_discard(handle) == ok);
        aevix::TaskScopeReport report;
        REQUIRE(aevix_task_scope_close(scope, &report) == ok && report.discarded == 1 && scoped.results == 1);
        REQUIRE(aevix_task_test_live_controls() == 0 && aevix_cancel_test_owner_count(token) == 1);
        REQUIRE(aevix_cancel_token_requested(token, &requested) == ok && !requested);
        REQUIRE(aevix_scoped_task_cancel_token(handle, &rejected) == stale && !rejected);
        REQUIRE(aevix_cancel_token_release(token) == ok && !aevix_cancel_test_live_states());
        return true;
    }
    bool clonesAcrossThreads()
    {
        std::uint64_t source = 0, token = 0;
        REQUIRE(aevix_cancel_source_new(&source) == ok && aevix_cancel_token_from_source(source, &token) == ok);
        std::atomic<bool> valid{true}, start{false};
        std::array<std::thread, 4> workers;
        for (auto &worker : workers)
            worker = std::thread([&] {
                while (!start.load())
                    std::this_thread::yield();
                for (unsigned round = 0; round < 1000; ++round)
                {
                    std::uint64_t copy = 0;
                    std::uint32_t requested = 0;
                    if (aevix_cancel_token_clone(token, &copy) != ok ||
                        aevix_cancel_token_requested(copy, &requested) != ok || requested > 1 ||
                        aevix_cancel_token_release(copy) != ok)
                        valid = false;
                }
            });
        start = true;
        std::uint32_t requested = 99;
        REQUIRE(aevix_cancel_source_request(source, &requested) == ok && !requested);
        for (auto &worker : workers)
            worker.join();
        REQUIRE(valid && aevix_cancel_test_owner_count(token) == 2 &&
                aevix_cancel_token_requested(token, &requested) == ok && requested == 1);
        REQUIRE(aevix_cancel_source_release(source) == ok && aevix_cancel_token_release(token) == ok &&
                !aevix_cancel_test_live_states());
        return true;
    }
    bool derivationConsumptionRaces(bool scoped)
    {
        for (unsigned round = 0; round < 100; ++round)
        {
            std::uint64_t scope = 0, task = 0, token = 0;
            Counts counts;
            if (scoped)
                REQUIRE(aevix_task_scope_open(&scope) == ok);
            REQUIRE(prepare(counts, false, task, scope) == ok);
            std::atomic<bool> start{false};
            int derived = 99, consumed = 99;
            Result output;
            aevix::TaskScopeReport report;
            std::thread a([&] {
                while (!start.load())
                    std::this_thread::yield();
                derived = scoped ? aevix_scoped_task_cancel_token(task, &token) : aevix_task_cancel_token(task, &token);
            });
            std::thread b([&] {
                while (!start.load())
                    std::this_thread::yield();
                consumed =
                    scoped ? aevix_task_scope_close(scope, &report) : aevix_task_join(task, &output, sizeof output);
            });
            start = true;
            a.join();
            b.join();
            REQUIRE(consumed == ok && (derived == ok || derived == stale) && counts.captures == 1);
            if (!scoped)
                dropResult(&output);
            REQUIRE(counts.results == 1 && !aevix_task_test_live_controls());
            bool validObserver = !token, released = true;
            if (derived == ok)
            {
                std::uint32_t requested = 99;
                validObserver = aevix_cancel_token_requested(token, &requested) == ok && !requested;
                released = aevix_cancel_token_release(token) == ok;
            }
            REQUIRE(validObserver);
            REQUIRE(released);
            REQUIRE(!aevix_cancel_test_live_states());
        }
        return true;
    }
    bool drainWithRetainedOwners()
    {
        Counts counts;
        std::uint64_t task = 0, token = 0;
        REQUIRE(prepare(counts, false, task) == ok && aevix_task_cancel_token(task, &token) == ok);
        REQUIRE(aevix_task_scope_runtime_drain() == ok && counts.captures == 1 && counts.results == 1);
        REQUIRE(!aevix_task_test_live_controls() && aevix_cancel_test_live_states() == 1 &&
                aevix_cancel_test_owner_count(token) == 1);
        std::uint32_t requested = 99;
        REQUIRE(aevix_cancel_token_requested(token, &requested) == ok && !requested);
        std::uint64_t clone = 0, source = 0;
        REQUIRE(aevix_cancel_token_clone(token, &clone) == ok && aevix_cancel_source_new(&source) == ok);
        REQUIRE(aevix_cancel_token_release(token) == ok && aevix_cancel_token_release(clone) == ok &&
                aevix_cancel_source_release(source) == ok);
        REQUIRE(!aevix_cancel_test_live_states());
        return true;
    }
}
int main()
{
    Deadline deadline;
    if (!allocationAndRoles() || !countsAndCachedAllocation() || !taskStateAndScopeRetention() ||
        !clonesAcrossThreads() || !derivationConsumptionRaces(false) || !derivationConsumptionRaces(true) ||
        !drainWithRetainedOwners())
        return 1;
    std::printf(
        "Cancellation owners: %u checks, 4000 clone/query/release iterations, 200 derivation/cleanup races passed.\n",
        checks);
    return 0;
}
