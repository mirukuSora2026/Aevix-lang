// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_task_scope_runtime_tests.cpp
//
// Description :
//     구조적 스코프의 입장 경쟁·버린 관측·재진입 정리·회수 실패의 소유권을 검증한다.
//     Verify structured-scope admission races, discarded observation, reentrant cleanup, and reap-failure ownership.
//
// ========================
#include "aevix/Runtime/TaskScopeRuntime.hpp"
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
    constexpr int ok = 0, allocation = 1, invalid = 3, stale = 4, badState = 5, threadError = 6, deadlock = 7,
                  notReady = 8;
    unsigned checks = 0;
    bool require(bool value, const char *expression, int line)
    {
        ++checks;
        if (!value)
            std::fprintf(stderr, "Task scope check failed at %d: %s\n", line, expression);
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
                  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                  while (!done.load())
                  {
                      if (std::chrono::steady_clock::now() > end)
                      {
                          std::fputs("Task scope dependency deadline exceeded.\n", stderr);
                          std::_Exit(92);
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
        std::atomic<unsigned> calls{0}, captures{0}, results{0};
        std::atomic<bool> entered{false}, go{false};
        std::atomic<int> selfClose{99}, dropClose{99};
        std::atomic<int> queryActiveStatus{99}, queryStateStatus{99}, queryAdmissionStatus{99};
        std::atomic<std::uint64_t> queryActive{99};
        std::atomic<std::uint32_t> queryState{99};
        std::atomic<bool> queried{false};
        std::uint64_t scope = 0;
    };
    struct Capture final
    {
        Counts *counts;
        int *owned;
        unsigned mode;
    };
    struct Result final
    {
        Counts *counts = nullptr;
        int *owned = nullptr;
        int value = 0;
        bool reenter = false, query = false;
    };
    void captureDrop(void *storage) noexcept
    {
        auto &value = *static_cast<Capture *>(storage);
        delete value.owned;
        value.owned = nullptr;
        ++value.counts->captures;
    }
    void resultDrop(void *storage) noexcept
    {
        auto &value = *static_cast<Result *>(storage);
        if (value.reenter)
        {
            aevix::TaskScopeReport report;
            value.counts->dropClose = aevix_task_scope_close(value.counts->scope, &report);
        }
        if (value.query)
        {
            std::uint64_t active = 99;
            std::uint32_t state = 99;
            value.counts->queryActiveStatus = aevix_task_scope_active_count(value.counts->scope, &active);
            value.counts->queryStateStatus = aevix_task_scope_status(value.counts->scope, &state);
            value.counts->queryAdmissionStatus = aevix_task_scope_close_admission(value.counts->scope);
            value.counts->queryActive = active;
            value.counts->queryState = state;
            value.counts->queried.store(true, std::memory_order_release);
        }
        delete value.owned;
        value.owned = nullptr;
        ++value.counts->results;
    }
    std::int32_t invoke(void *input, void *output, const aevix::TaskContext *context) noexcept
    {
        auto &value = *static_cast<Capture *>(input);
        auto &counts = *value.counts;
        ++counts.calls;
        counts.entered = true;
        if (value.mode == 1 || value.mode == 3)
            while (!counts.go.load())
                std::this_thread::yield();
        if (value.mode == 2)
        {
            while (!aevix_task_context_cancel_requested(context))
                std::this_thread::yield();
            captureDrop(input);
            return 1;
        }
        if (value.mode == 3)
        {
            aevix::TaskScopeReport report;
            counts.selfClose = aevix_task_scope_close(counts.scope, &report);
        }
        if (value.mode == 4)
        {
            captureDrop(input);
            return 2;
        }
        auto *result = new (output) Result{&counts, value.owned, *value.owned, value.mode == 5, value.mode == 6};
        (void)result;
        value.owned = nullptr;
        ++counts.captures;
        return 0;
    }
    int prepare(std::uint64_t scope, Counts &counts, int value, unsigned mode, std::uint64_t &handle,
                bool commit = true)
    {
        void *storage = nullptr;
        const auto result =
            aevix_task_scope_prepare(scope, sizeof(Capture), alignof(Capture), sizeof(Result), alignof(Result), invoke,
                                     captureDrop, resultDrop, &handle, &storage);
        if (result)
            return result;
        if (commit)
        {
            new (storage) Capture{&counts, new int(value), mode};
            return aevix_scoped_task_commit(handle);
        }
        return 0;
    }
    bool emptyAndFailures()
    {
        std::uint64_t scope = 99;
        aevix_task_test_fail_after(0);
        REQUIRE(aevix_task_scope_open(&scope) == allocation && !scope);
        aevix_task_test_fail_after(1);
        REQUIRE(aevix_task_scope_open(&scope) == allocation && !scope);
        aevix_task_test_fail_after(UINT64_MAX);
        REQUIRE(aevix_task_scope_open(&scope) == ok);
        Counts counts;
        counts.scope = scope;
        for (std::uint64_t budget = 0; budget < 6; ++budget)
        {
            std::uint64_t handle = 99;
            void *storage = reinterpret_cast<void *>(std::uintptr_t(1));
            int *original = new int(17);
            aevix_task_test_fail_after(budget);
            const auto result =
                aevix_task_scope_prepare(scope, sizeof(Capture), alignof(Capture), sizeof(Result), alignof(Result),
                                         invoke, captureDrop, resultDrop, &handle, &storage);
            aevix_task_test_fail_after(UINT64_MAX);
            REQUIRE(result == allocation && !handle && !storage && *original == 17 && counts.calls == 0 &&
                    counts.captures == 0);
            delete original;
        }
        aevix_task_test_fail_threads(1);
        std::uint64_t handle = 99;
        void *storage = nullptr;
        REQUIRE(aevix_task_scope_prepare(scope, sizeof(Capture), alignof(Capture), sizeof(Result), alignof(Result),
                                         invoke, captureDrop, resultDrop, &handle, &storage) == threadError &&
                !handle && !storage);
        aevix_task_test_fail_threads(0);
        aevix::TaskScopeSnapshot snapshot;
        REQUIRE(aevix_task_scope_snapshot(scope, &snapshot) == ok && !snapshot.accepted && !snapshot.live_handles);
        aevix::TaskScopeReport report;
        REQUIRE(aevix_task_scope_close(scope, &report) == ok && !report.accepted);
        REQUIRE(aevix_task_scope_close(scope, &report) == stale && aevix_task_scope_cleanup(0) == ok);
        REQUIRE(aevix_task_scope_test_live_scopes() == 0 && aevix_task_scope_test_live_handles() == 0);
        return true;
    }
    bool observedAndAbandoned()
    {
        std::uint64_t scope = 0, first = 0, second = 0, failure = 0;
        REQUIRE(aevix_task_scope_open(&scope) == ok);
        Counts a, b, c;
        a.scope = b.scope = c.scope = scope;
        REQUIRE(prepare(scope, a, 17, 0, first) == ok && prepare(scope, b, 23, 0, second) == ok &&
                prepare(scope, c, 31, 4, failure) == ok);
        Result output;
        REQUIRE(aevix_scoped_task_join(first, nullptr, sizeof output) == invalid);
        REQUIRE(aevix_scoped_task_join(first, &output, sizeof output) == ok && output.value == 17 && a.captures == 1 &&
                a.results == 0);
        resultDrop(&output);
        REQUIRE(a.results == 1 && aevix_scoped_task_join(first, &output, sizeof output) == stale);
        REQUIRE(aevix_scoped_task_wait(second) == ok && b.results == 0);
        REQUIRE(aevix_scoped_task_discard(second) == ok && aevix_scoped_task_snapshot(second, nullptr) == stale);
        REQUIRE(aevix_scoped_task_join(failure, &output, sizeof output) == 10 && c.captures == 1 && c.results == 0);
        aevix::TaskScopeReport report;
        REQUIRE(aevix_task_scope_close(scope, &report) == ok);
        REQUIRE(report.accepted == 3 && report.observed == 2 && report.discarded == 1 && report.completed == 2 &&
                report.failed == 1 && !report.aborted);
        REQUIRE(b.captures == 1 && b.results == 1 && aevix_task_scope_test_live_handles() == 0 &&
                aevix_task_test_live_controls() == 0);
        return true;
    }
    bool retiredHandleEntryReuse()
    {
        std::uint64_t a = 0, b = 0, oldHandle = 0, newHandle = 0;
        Counts first, second;
        REQUIRE(aevix_task_scope_open(&a) == ok && aevix_task_scope_open(&b) == ok);
        first.scope = a;
        second.scope = b;
        REQUIRE(prepare(a, first, 33, 0, oldHandle) == ok && aevix_scoped_task_wait(oldHandle) == ok);
        REQUIRE(aevix_scoped_task_discard(oldHandle) == ok);
        REQUIRE(prepare(b, second, 34, 1, newHandle) == ok);
        REQUIRE(std::uint32_t(oldHandle) == std::uint32_t(newHandle) && oldHandle != newHandle);
        aevix::TaskScopeReport report;
        REQUIRE(aevix_task_scope_close(a, &report) == ok && first.results == 1);
        aevix::TaskSnapshot snapshot;
        REQUIRE(aevix_scoped_task_snapshot(newHandle, &snapshot) == ok);
        second.go = true;
        Result result;
        REQUIRE(aevix_scoped_task_join(newHandle, &result, sizeof result) == ok && result.value == 34);
        resultDrop(&result);
        REQUIRE(aevix_task_scope_close(b, &report) == ok && second.results == 1);
        return true;
    }
    bool preparedClose(bool abort)
    {
        std::uint64_t scope = 0, handle = 0;
        REQUIRE(aevix_task_scope_open(&scope) == ok);
        Counts counts;
        counts.scope = scope;
        void *storage = nullptr;
        REQUIRE(aevix_task_scope_prepare(scope, sizeof(Capture), alignof(Capture), sizeof(Result), alignof(Result),
                                         invoke, captureDrop, resultDrop, &handle, &storage) == ok);
        REQUIRE(aevix_scoped_task_discard(handle) == badState && counts.calls == 0);
        aevix::TaskScopeReport report;
        std::atomic<int> closed{99};
        std::thread closer([&] { closed = aevix_task_scope_close(scope, &report); });
        aevix::TaskScopeSnapshot snapshot;
        std::int32_t snapshotStatus;
        do
        {
            snapshotStatus = aevix_task_scope_snapshot(scope, &snapshot);
            std::this_thread::yield();
        } while (snapshotStatus == ok && !snapshot.closing);
        REQUIRE(snapshotStatus == ok && snapshot.closing && closed == 99 && counts.calls == 0);
        std::uint64_t rejected = 99;
        void *rejectedStorage = reinterpret_cast<void *>(std::uintptr_t(1));
        REQUIRE(aevix_task_scope_prepare(scope, 0, 1, 0, 1, invoke, nullptr, nullptr, &rejected, &rejectedStorage) ==
                    badState &&
                !rejected && !rejectedStorage);
        if (abort)
            REQUIRE(aevix_scoped_task_abort(handle) == ok);
        else
        {
            new (storage) Capture{&counts, new int(41), 0};
            aevix_task_test_fail_after(0);
            REQUIRE(aevix_scoped_task_commit(handle) == ok);
            aevix_task_test_fail_after(UINT64_MAX);
        }
        closer.join();
        REQUIRE(closed == ok && report.accepted == 1 && report.aborted == unsigned(abort));
        REQUIRE(counts.calls == unsigned(!abort) && counts.captures == unsigned(!abort) &&
                counts.results == unsigned(!abort));
        REQUIRE(aevix_scoped_task_commit(handle) == stale && aevix_task_scope_test_live_handles() == 0);
        return true;
    }
    bool discardedRunningAndCancel()
    {
        std::uint64_t scope = 0, handle = 0;
        REQUIRE(aevix_task_scope_open(&scope) == ok);
        Counts counts;
        counts.scope = scope;
        REQUIRE(prepare(scope, counts, 51, 2, handle) == ok);
        while (!counts.entered.load())
            std::this_thread::yield();
        REQUIRE(aevix_scoped_task_discard(handle) == ok && !counts.captures && !counts.results);
        aevix::TaskScopeReport report;
        std::atomic<int> closed{99};
        std::thread closer([&] { closed = aevix_task_scope_close(scope, &report); });
        while (!aevix_task_scope_test_is_reaping(scope))
            std::this_thread::yield();
        REQUIRE(closed == 99 && !counts.captures);
        std::uint64_t requested = 0;
        REQUIRE(aevix_task_scope_request_cancel(scope, &requested) == ok && requested == 1);
        closer.join();
        REQUIRE(closed == ok && counts.calls == 1 && counts.captures == 1 && !counts.results);
        REQUIRE(report.accepted == 1 && report.discarded == 1 && report.cancelled == 1 && !report.observed);
        return true;
    }
    bool selfAndReentrantClose()
    {
        std::uint64_t scope = 0, selfHandle = 0, sibling = 0;
        REQUIRE(aevix_task_scope_open(&scope) == ok);
        Counts self, siblingCounts;
        self.scope = siblingCounts.scope = scope;
        REQUIRE(prepare(scope, self, 61, 3, selfHandle) == ok && prepare(scope, siblingCounts, 62, 1, sibling) == ok);
        while (!self.entered.load() || !siblingCounts.entered.load())
            std::this_thread::yield();
        self.go = true;
        while (self.selfClose == 99)
            std::this_thread::yield();
        REQUIRE(self.selfClose == deadlock && !siblingCounts.captures);
        Result result;
        REQUIRE(aevix_scoped_task_join(selfHandle, &result, sizeof result) == ok);
        resultDrop(&result);
        siblingCounts.go = true;
        REQUIRE(aevix_scoped_task_discard(sibling) == ok);
        Counts reentrant;
        reentrant.scope = scope;
        std::uint64_t handle = 0;
        REQUIRE(prepare(scope, reentrant, 63, 5, handle) == ok);
        aevix::TaskScopeReport report;
        REQUIRE(aevix_task_scope_close(scope, &report) == ok && reentrant.dropClose == deadlock);
        REQUIRE(report.accepted == 3 && report.completed == 3 && report.observed == 1 && report.discarded == 1 &&
                reentrant.results == 1 && siblingCounts.results == 1);
        return true;
    }
    bool reapingFailureRetry()
    {
        std::uint64_t scope = 0, handle = 0;
        REQUIRE(aevix_task_scope_open(&scope) == ok);
        Counts counts;
        counts.scope = scope;
        REQUIRE(prepare(scope, counts, 71, 0, handle) == ok);
        aevix_task_test_fail_reap(1);
        aevix::TaskScopeReport report;
        REQUIRE(aevix_task_scope_close(scope, &report) == threadError && report.accepted == 0 && counts.results == 0);
        REQUIRE(aevix_task_last_error() != 0 && aevix_task_scope_test_live_scopes() == 1 &&
                aevix_task_test_live_controls() == 1);
        aevix_task_test_fail_reap(0);
        aevix::TaskScopeSnapshot snapshot;
        REQUIRE(aevix_task_scope_snapshot(scope, &snapshot) == ok && snapshot.closing && snapshot.live_handles == 0);
        REQUIRE(aevix_task_scope_close(scope, &report) == ok && report.accepted == 1 && report.completed == 1 &&
                counts.captures == 1 && counts.results == 1);
        return true;
    }
    bool independentAndCompetingConsumers()
    {
        std::uint64_t scope = 0, handle = 0;
        REQUIRE(aevix_task_scope_open(&scope) == ok);
        Counts counts;
        counts.scope = scope;
        REQUIRE(prepare(scope, counts, 81, 1, handle) == ok);
        while (!counts.entered.load())
            std::this_thread::yield();
        Result output;
        std::atomic<int> joined{99};
        std::thread consumer([&] { joined = aevix_scoped_task_join(handle, &output, sizeof output); });
        while (!aevix_task_scope_test_is_reaping(scope))
            std::this_thread::yield();
        REQUIRE(aevix_scoped_task_discard(handle) == notReady && joined == 99);
        std::uint64_t otherScope = 0, otherHandle = 0;
        Counts other;
        REQUIRE(aevix_task_scope_open(&otherScope) == ok);
        other.scope = otherScope;
        REQUIRE(prepare(otherScope, other, 82, 0, otherHandle) == ok);
        aevix::TaskScopeReport otherReport;
        REQUIRE(aevix_task_scope_close(otherScope, &otherReport) == ok && other.results == 1 && joined == 99);
        counts.go = true;
        consumer.join();
        REQUIRE(joined == ok && output.value == 81);
        resultDrop(&output);
        aevix::TaskScopeReport report;
        REQUIRE(aevix_task_scope_close(scope, &report) == ok && report.observed == 1 && counts.results == 1);
        return true;
    }
    bool discardCloseRaces()
    {
        for (unsigned round = 0; round < 100; ++round)
        {
            std::uint64_t scope = 0, handle = 0;
            Counts counts;
            REQUIRE(aevix_task_scope_open(&scope) == ok);
            counts.scope = scope;
            REQUIRE(prepare(scope, counts, int(round), 0, handle) == ok);
            std::atomic<bool> start{false};
            int discarded = 99, closed = 99;
            aevix::TaskScopeReport report;
            std::thread a([&] {
                while (!start.load())
                    std::this_thread::yield();
                discarded = aevix_scoped_task_discard(handle);
            });
            std::thread b([&] {
                while (!start.load())
                    std::this_thread::yield();
                closed = aevix_task_scope_close(scope, &report);
            });
            start = true;
            a.join();
            b.join();
            REQUIRE(closed == ok && (discarded == ok || discarded == stale || discarded == notReady));
            REQUIRE(report.accepted == 1 && report.completed == 1 && report.discarded == unsigned(discarded == ok));
            REQUIRE(counts.calls == 1 && counts.captures == 1 && counts.results == 1);
        }
        return true;
    }
    bool admissionAndActiveSnapshots()
    {
        std::uint64_t scope = 0, handle = 0, active = 99;
        std::uint32_t state = 99;
        REQUIRE(aevix_task_scope_open(&scope) == ok);
        REQUIRE(aevix_task_scope_active_count(scope, &active) == ok && active == 0);
        REQUIRE(aevix_task_scope_status(scope, &state) == ok && state == std::uint32_t(aevix::TaskScopeState::Open));
        REQUIRE(aevix_task_scope_active_count(scope, nullptr) == invalid && aevix_task_scope_status(scope, nullptr) == invalid);
        Counts counts;
        counts.scope = scope;
        void *storage = nullptr;
        REQUIRE(aevix_task_scope_prepare(scope, sizeof(Capture), alignof(Capture), sizeof(Result), alignof(Result),
                                         invoke, captureDrop, resultDrop, &handle, &storage) == ok);
        REQUIRE(aevix_task_scope_active_count(scope, &active) == ok && active == 1 && counts.calls == 0);
        active = 99;
        state = 99;
        REQUIRE(aevix_task_scope_active_count(handle, &active) == stale && active == 0);
        REQUIRE(aevix_task_scope_status(handle, &state) == stale && state == 0);
        aevix_task_test_fail_after(0);
        REQUIRE(aevix_task_scope_close_admission(scope) == ok && aevix_task_scope_close_admission(scope) == ok);
        REQUIRE(aevix_task_scope_status(scope, &state) == ok && state == std::uint32_t(aevix::TaskScopeState::Closing));
        REQUIRE(aevix_task_scope_active_count(scope, &active) == ok && active == 1 && counts.calls == 0);
        aevix_task_test_fail_after(UINT64_MAX);
        std::uint64_t rejected = 99;
        void *rejectedStorage = reinterpret_cast<void *>(std::uintptr_t(1));
        REQUIRE(aevix_task_scope_prepare(scope, 0, 1, 0, 1, invoke, nullptr, nullptr, &rejected, &rejectedStorage) == badState &&
                rejected == 0 && rejectedStorage == nullptr);
        new (storage) Capture{&counts, new int(101), 1};
        REQUIRE(aevix_scoped_task_commit(handle) == ok);
        while (!counts.entered.load()) std::this_thread::yield();
        REQUIRE(aevix_scoped_task_discard(handle) == ok);
        aevix::TaskScopeSnapshot snapshot;
        REQUIRE(aevix_task_scope_snapshot(scope, &snapshot) == ok && snapshot.closing && snapshot.live_handles == 0);
        REQUIRE(aevix_task_scope_active_count(scope, &active) == ok && active == 1);
        std::atomic<int> closed{99};
        aevix::TaskScopeReport report;
        std::thread closer([&] { closed = aevix_task_scope_close(scope, &report); });
        while (!aevix_task_scope_test_is_reaping(scope)) std::this_thread::yield();
        REQUIRE(closed == 99 && counts.results == 0);
        REQUIRE(aevix_task_scope_status(scope, &state) == ok && state == std::uint32_t(aevix::TaskScopeState::Closing));
        REQUIRE(aevix_task_scope_active_count(scope, &active) == ok && active == 1);
        counts.go = true;
        closer.join();
        REQUIRE(closed == ok && report.accepted == 1 && report.discarded == 1 && report.completed == 1 &&
                counts.captures == 1 && counts.results == 1);
        active = 99;
        state = 99;
        REQUIRE(aevix_task_scope_active_count(scope, &active) == stale && active == 0);
        REQUIRE(aevix_task_scope_status(scope, &state) == stale && state == 0);
        REQUIRE(aevix_task_scope_close_admission(scope) == stale);
        std::uint64_t replacement = 0;
        REQUIRE(aevix_task_scope_open(&replacement) == ok && replacement != scope && std::uint32_t(replacement) == std::uint32_t(scope));
        active = 99;
        state = 99;
        REQUIRE(aevix_task_scope_active_count(scope, &active) == stale && active == 0 && aevix_task_scope_status(scope, &state) == stale && state == 0);
        REQUIRE(aevix_task_scope_status(replacement, &state) == ok && state == std::uint32_t(aevix::TaskScopeState::Open));
        REQUIRE(aevix_task_scope_close(replacement, &report) == ok && report.accepted == 0);
        return true;
    }
    bool terminalAndAbortedAreNotDrained()
    {
        std::uint64_t scope = 0, handle = 0, active = 99;
        std::uint32_t state = 99;
        Counts counts;
        REQUIRE(aevix_task_scope_open(&scope) == ok);
        counts.scope = scope;
        REQUIRE(prepare(scope, counts, 111, 0, handle) == ok);
        REQUIRE(aevix_scoped_task_wait_timeout(handle, 1000000000) == ok);
        REQUIRE(aevix_task_scope_active_count(scope, &active) == ok && active == 0 && counts.results == 0);
        REQUIRE(aevix_task_scope_status(scope, &state) == ok && state == std::uint32_t(aevix::TaskScopeState::Open));
        aevix::TaskScopeSnapshot snapshot;
        REQUIRE(aevix_task_scope_snapshot(scope, &snapshot) == ok && snapshot.live_handles == 1);
        REQUIRE(aevix_task_scope_close_admission(scope) == ok);
        aevix_task_test_fail_reap(1);
        aevix::TaskScopeReport report;
        REQUIRE(aevix_task_scope_close(scope, &report) == threadError && counts.results == 0);
        aevix_task_test_fail_reap(0);
        REQUIRE(aevix_task_scope_status(scope, &state) == ok && state == std::uint32_t(aevix::TaskScopeState::Closing));
        REQUIRE(aevix_task_scope_active_count(scope, &active) == ok && active == 0);
        REQUIRE(aevix_task_scope_close_admission(scope) == ok && aevix_task_scope_close(scope, &report) == ok && counts.results == 1);
        Counts uninitialized;
        REQUIRE(aevix_task_scope_open(&scope) == ok);
        uninitialized.scope = scope;
        REQUIRE(prepare(scope, uninitialized, 0, 0, handle, false) == ok);
        REQUIRE(aevix_task_scope_close_admission(scope) == ok && aevix_task_scope_active_count(scope, &active) == ok && active == 1);
        REQUIRE(aevix_scoped_task_abort(handle) == ok && aevix_task_scope_active_count(scope, &active) == ok && active == 0);
        REQUIRE(aevix_task_scope_status(scope, &state) == ok && state == std::uint32_t(aevix::TaskScopeState::Closing));
        REQUIRE(aevix_task_scope_close(scope, &report) == ok && report.accepted == 1 && report.aborted == 1 && uninitialized.calls == 0 && uninitialized.captures == 0 && uninitialized.results == 0);
        active = 99;
        state = 99;
        REQUIRE(aevix_task_scope_active_count(0, &active) == stale && active == 0 && aevix_task_scope_status(0, &state) == stale && state == 0 && aevix_task_scope_close_admission(0) == stale);
        return true;
    }
    bool cleanupCanObserveScope()
    {
        std::uint64_t scope = 0, sibling = 0, observer = 0;
        REQUIRE(aevix_task_scope_open(&scope) == ok);
        Counts running, query;
        running.scope = query.scope = scope;
        REQUIRE(prepare(scope, running, 121, 1, sibling) == ok);
        while (!running.entered.load()) std::this_thread::yield();
        REQUIRE(prepare(scope, query, 122, 6, observer) == ok && aevix_scoped_task_wait(observer) == ok);
        REQUIRE(aevix_scoped_task_discard(sibling) == ok && aevix_scoped_task_discard(observer) == ok);
        aevix::TaskScopeReport report;
        std::atomic<int> closed{99};
        std::thread closer([&] { closed = aevix_task_scope_close(scope, &report); });
        while (!query.queried.load(std::memory_order_acquire)) std::this_thread::yield();
        REQUIRE(query.queryActiveStatus == ok && query.queryStateStatus == ok && query.queryAdmissionStatus == ok &&
                query.queryActive == 1 && query.queryState == std::uint32_t(aevix::TaskScopeState::Closing));
        REQUIRE(closed == 99 && running.results == 0);
        running.go = true;
        closer.join();
        REQUIRE(closed == ok && report.accepted == 2 && report.discarded == 2 && report.completed == 2 && running.results == 1 && query.results == 1);
        return true;
    }
    bool admissionPrepareObservationRaces()
    {
        for (unsigned round = 0; round < 100; ++round)
        {
            std::uint64_t scope = 0, handle = 99;
            void *storage = nullptr;
            REQUIRE(aevix_task_scope_open(&scope) == ok);
            std::atomic<bool> start{false}, invalidObservation{false};
            int prepared = 99, closed = 99;
            std::thread producer([&] {
                while (!start.load()) std::this_thread::yield();
                prepared = aevix_task_scope_prepare(scope, sizeof(Capture), alignof(Capture), sizeof(Result), alignof(Result), invoke, captureDrop, resultDrop, &handle, &storage);
            });
            std::thread closer([&] {
                while (!start.load()) std::this_thread::yield();
                closed = aevix_task_scope_close_admission(scope);
            });
            std::thread observer([&] {
                while (!start.load()) std::this_thread::yield();
                for (unsigned repeat = 0; repeat < 16; ++repeat)
                {
                    std::uint64_t active = 99;
                    std::uint32_t state = 99;
                    if (aevix_task_scope_active_count(scope, &active) != ok || active > 1 ||
                        aevix_task_scope_status(scope, &state) != ok || state > std::uint32_t(aevix::TaskScopeState::Closing)) invalidObservation = true;
                }
            });
            start = true;
            producer.join(); closer.join(); observer.join();
            REQUIRE(closed == ok && (prepared == ok || prepared == badState) && !invalidObservation);
            std::uint64_t active = 99;
            std::uint32_t state = 99;
            REQUIRE(aevix_task_scope_active_count(scope, &active) == ok && active == std::uint64_t(prepared == ok));
            REQUIRE(aevix_task_scope_status(scope, &state) == ok && state == std::uint32_t(aevix::TaskScopeState::Closing));
            REQUIRE(aevix_task_scope_close_admission(scope) == ok);
            if (prepared == ok) REQUIRE(aevix_scoped_task_abort(handle) == ok);
            else REQUIRE(handle == 0 && storage == nullptr);
            aevix::TaskScopeReport report;
            REQUIRE(aevix_task_scope_close(scope, &report) == ok && report.accepted == std::uint64_t(prepared == ok) && report.aborted == report.accepted);
        }
        return true;
    }
    bool drainMetadata()
    {
        std::uint64_t scope = 0, handle = 0;
        REQUIRE(aevix_task_scope_open(&scope) == ok);
        Counts counts;
        counts.scope = scope;
        aevix_task_test_hold_worker_tail(1);
        REQUIRE(prepare(scope, counts, 91, 0, handle) == ok &&
                aevix_scoped_task_wait_timeout(handle, 1000000000) == ok);
        Result output;
        REQUIRE(aevix_scoped_task_try_take(handle, &output, sizeof output) == ok && output.value == 91);
        resultDrop(&output);
        aevix::TaskScopeReport report;
        REQUIRE(aevix_task_scope_close(scope, &report) == ok && report.observed == 1 && counts.results == 1);
        REQUIRE(aevix_task_scope_test_live_scopes() == 0 && aevix_task_test_live_controls() == 1);
        std::atomic<int> drained{99};
        std::atomic<bool> started{false};
        std::thread shutdown([&] {
            started = true;
            drained = aevix_task_scope_runtime_drain();
        });
        while (!started.load())
            std::this_thread::yield();
        REQUIRE(drained == 99);
        aevix_task_test_hold_worker_tail(0);
        shutdown.join();
        REQUIRE(drained == ok && aevix_task_test_live_controls() == 0 && aevix_task_scope_test_live_handles() == 0);
        REQUIRE(aevix_task_scope_open(&scope) == badState && !scope && aevix_task_scope_runtime_drain() == ok);
        return true;
    }
}
int main()
{
    Deadline deadline;
    if (!emptyAndFailures() || !observedAndAbandoned() || !retiredHandleEntryReuse() || !preparedClose(false) ||
        !preparedClose(true) || !discardedRunningAndCancel() || !selfAndReentrantClose() || !reapingFailureRetry() ||
        !independentAndCompetingConsumers() || !discardCloseRaces() || !admissionAndActiveSnapshots() ||
        !terminalAndAbortedAreNotDrained() || !cleanupCanObserveScope() || !admissionPrepareObservationRaces() || !drainMetadata())
        return 1;
    std::printf("Task scopes: %u checks; admission, discard, exact cleanup, self/reentrant close, and retry passed.\n",
                checks);
    return 0;
}
