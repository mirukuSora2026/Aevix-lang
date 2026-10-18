// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_lock_runtime_tests.cpp
//
// Description :
//     인라인 잠금의 비트 경계, 오염·대기 계약과 실제 읽기·쓰기 경쟁 및 게시를 검증한다.
//     Verify inline-lock bit bounds, poison/wait contracts, real reader/writer contention, and publication.
//
// ========================
#include "aevix/Runtime/LockRuntime.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <type_traits>

#if !defined(AEVIX_LOCK_RUNTIME_TESTING)
#error "Compile this dedicated test with AEVIX_LOCK_RUNTIME_TESTING."
#endif

namespace
{
    using aevix::InlineMutexState;
    using aevix::InlineRwLockState;
    constexpr std::int32_t ok = 0, busy = 1, timedOut = 2, invalid = 3, badState = 4, overflow = 5;
    constexpr auto writer = std::uint64_t{1} << 63, poison = std::uint64_t{1} << 62, readers = poison - 1;
    static_assert(std::is_trivially_copyable_v<InlineMutexState> && std::is_trivially_copyable_v<InlineRwLockState>);
    unsigned checks = 0;
    bool require(bool condition, const char *expression, int line)
    {
        ++checks;
        if (!condition) std::fprintf(stderr, "Inline lock check failed at %d: %s\n", line, expression);
        return condition;
    }
#define REQUIRE(...) do { if (!require(bool(__VA_ARGS__), #__VA_ARGS__, __LINE__)) return false; } while (false)
    struct Deadline final
    {
        std::atomic<bool> done{false};
        std::thread worker;
        Deadline() : worker([this] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (!done.load()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    std::fputs("Inline lock test exceeded its bounded dependency deadline.\n", stderr);
                    std::_Exit(91);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }) {}
        ~Deadline() { done = true; worker.join(); }
    };
    bool mutexContracts()
    {
        InlineMutexState state;
        REQUIRE(aevix_mutex_init(&state) == ok && state.bits == 0);
        REQUIRE(aevix_mutex_init(nullptr) == invalid);
        alignas(8) std::array<unsigned char, 24> storage{};
        REQUIRE(aevix_mutex_init(storage.data() + 1) == invalid);
        REQUIRE(aevix_mutex_init(reinterpret_cast<void *>(UINTPTR_MAX - 7)) == invalid);
        std::uint32_t poisoned = 99;
        REQUIRE(aevix_mutex_try_lock(&state, nullptr) == invalid && state.bits == 0);
        REQUIRE(aevix_mutex_try_lock(&state, reinterpret_cast<std::uint32_t *>(&state)) == invalid && state.bits == 0);
        REQUIRE(aevix_mutex_unlock(&state, 0) == badState);
        REQUIRE(aevix_mutex_clear_poison(&state) == badState);
        REQUIRE(aevix_mutex_lock_timeout(&state, UINT64_MAX, &poisoned) == invalid && poisoned == 0 && state.bits == 0);
        REQUIRE(aevix_mutex_try_lock(&state, &poisoned) == ok && poisoned == 0 && state.bits == 1);
        REQUIRE(aevix_mutex_try_lock(&state, &poisoned) == busy && poisoned == 0 && state.bits == 1);
        REQUIRE(aevix_mutex_lock_timeout(&state, 0, &poisoned) == timedOut && poisoned == 0 && state.bits == 1);
        const auto started = std::chrono::steady_clock::now();
        REQUIRE(aevix_mutex_lock_timeout(&state, 1000000, &poisoned) == timedOut && state.bits == 1);
        REQUIRE(std::chrono::steady_clock::now() >= started);
        REQUIRE(aevix_mutex_unlock(&state, 2) == invalid && state.bits == 1);
        REQUIRE(aevix_mutex_unlock(&state, 1) == ok && state.bits == 2);
        REQUIRE(aevix_mutex_is_poisoned(&state, &poisoned) == ok && poisoned == 1);
        REQUIRE(aevix_mutex_clear_poison(&state) == badState && state.bits == 2);
        REQUIRE(aevix_mutex_lock(&state, &poisoned) == ok && poisoned == 1 && state.bits == 3);
        // 오염 결과도 획득한 보호자를 보존하므로 복구 전의 두 번째 획득은 성공하면 안 된다.
        // Poison still retains the acquired guard, so a second acquisition cannot succeed before recovery.
        REQUIRE(aevix_mutex_try_lock(&state, &poisoned) == busy && state.bits == 3);
        REQUIRE(aevix_mutex_clear_poison(&state) == ok && state.bits == 1);
        REQUIRE(aevix_mutex_is_poisoned(&state, &poisoned) == ok && !poisoned);
        REQUIRE(aevix_mutex_unlock(&state, 0) == ok && state.bits == 0);
        REQUIRE(aevix_mutex_lock_timeout(&state, 0, &poisoned) == ok && !poisoned);
        REQUIRE(aevix_mutex_unlock(&state, 0) == ok);
        REQUIRE(aevix_lock_test_set_state(&state, 4) == ok);
        REQUIRE(aevix_mutex_try_lock(&state, &poisoned) == badState && state.bits == 4 && !poisoned);
        REQUIRE(aevix_mutex_is_poisoned(&state, &poisoned) == badState);
        REQUIRE(aevix_mutex_unlock(&state, 0) == badState && state.bits == 4);
        REQUIRE(aevix_mutex_init(&state) == ok && state.bits == 0);
        REQUIRE(aevix_mutex_lock(&state, &poisoned) == ok && aevix_mutex_unlock(&state, 1) == ok);
        InlineMutexState relocated;
        std::memcpy(&relocated, &state, sizeof state); // 대여자가 없을 때만 일반 정수 상태를 이동한다.
                                                     // Relocate ordinary integer state only without active borrowers.
        state.bits = 0;
        REQUIRE(aevix_mutex_lock(&relocated, &poisoned) == ok && poisoned == 1);
        REQUIRE(aevix_mutex_clear_poison(&relocated) == ok && aevix_mutex_unlock(&relocated, 0) == ok);
        return true;
    }
    bool rwContracts()
    {
        InlineRwLockState state;
        std::uint32_t poisoned = 99;
        REQUIRE(aevix_rwlock_init(&state) == ok && state.bits == 0);
        REQUIRE(aevix_rwlock_read_unlock(&state) == badState);
        REQUIRE(aevix_rwlock_write_unlock(&state, 0) == badState);
        REQUIRE(aevix_rwlock_try_read(&state, &poisoned) == ok && state.bits == 1 && !poisoned);
        REQUIRE(aevix_rwlock_try_read(&state, &poisoned) == ok && state.bits == 2);
        REQUIRE(aevix_rwlock_try_write(&state, &poisoned) == busy && state.bits == 2);
        REQUIRE(aevix_rwlock_write_timeout(&state, 0, &poisoned) == timedOut && state.bits == 2);
        REQUIRE(aevix_rwlock_clear_poison(&state) == badState);
        REQUIRE(aevix_rwlock_read_unlock(&state) == ok && state.bits == 1);
        REQUIRE(aevix_rwlock_read_unlock(&state) == ok && state.bits == 0);
        REQUIRE(aevix_rwlock_write(&state, &poisoned) == ok && state.bits == writer);
        REQUIRE(aevix_rwlock_try_read(&state, &poisoned) == busy && state.bits == writer);
        REQUIRE(aevix_rwlock_try_write(&state, &poisoned) == busy && state.bits == writer);
        REQUIRE(aevix_rwlock_read_timeout(&state, 0, &poisoned) == timedOut && state.bits == writer);
        REQUIRE(aevix_rwlock_write_unlock(&state, 2) == invalid && state.bits == writer);
        REQUIRE(aevix_rwlock_read_unlock(&state) == badState && state.bits == writer);
        REQUIRE(aevix_rwlock_write_unlock(&state, 1) == ok && state.bits == poison);
        REQUIRE(aevix_rwlock_is_poisoned(&state, &poisoned) == ok && poisoned == 1);
        REQUIRE(aevix_rwlock_read(&state, &poisoned) == ok && poisoned == 1 && state.bits == (poison | 1));
        REQUIRE(aevix_rwlock_clear_poison(&state) == badState && state.bits == (poison | 1));
        REQUIRE(aevix_rwlock_try_write(&state, &poisoned) == busy);
        REQUIRE(aevix_rwlock_read_unlock(&state) == ok && state.bits == poison);
        REQUIRE(aevix_rwlock_write_timeout(&state, 0, &poisoned) == ok && poisoned == 1 && state.bits == (writer | poison));
        REQUIRE(aevix_rwlock_try_read(&state, &poisoned) == busy && state.bits == (writer | poison));
        REQUIRE(aevix_rwlock_clear_poison(&state) == ok && state.bits == writer);
        REQUIRE(aevix_rwlock_write_unlock(&state, 0) == ok && state.bits == 0);
        REQUIRE(aevix_lock_test_set_state(&state, poison | readers) == ok);
        REQUIRE(aevix_rwlock_try_read(&state, &poisoned) == overflow && !poisoned && state.bits == (poison | readers));
        REQUIRE(aevix_rwlock_read_timeout(&state, 0, &poisoned) == overflow && state.bits == (poison | readers));
        REQUIRE(aevix_rwlock_try_write(&state, &poisoned) == busy && state.bits == (poison | readers));
        REQUIRE(aevix_rwlock_read_unlock(&state) == ok && state.bits == (poison | (readers - 1)));
        REQUIRE(aevix_rwlock_try_read(&state, &poisoned) == ok && poisoned == 1 && state.bits == (poison | readers));
        REQUIRE(aevix_lock_test_set_state(&state, writer | 1) == ok);
        REQUIRE(aevix_rwlock_try_read(&state, &poisoned) == badState && state.bits == (writer | 1));
        REQUIRE(aevix_rwlock_try_write(&state, &poisoned) == badState && state.bits == (writer | 1));
        REQUIRE(aevix_rwlock_is_poisoned(&state, &poisoned) == badState);
        REQUIRE(aevix_rwlock_read_unlock(&state) == badState && aevix_rwlock_write_unlock(&state, 0) == badState);
        REQUIRE(aevix_rwlock_init(&state) == ok);
        REQUIRE(aevix_rwlock_read_timeout(&state, UINT64_MAX, &poisoned) == invalid && state.bits == 0);
        REQUIRE(aevix_rwlock_write_timeout(&state, UINT64_MAX, &poisoned) == invalid && state.bits == 0);
        REQUIRE(aevix_rwlock_read_timeout(&state, 0, &poisoned) == ok && !poisoned);
        REQUIRE(aevix_rwlock_read_unlock(&state) == ok);
        REQUIRE(aevix_rwlock_write(&state, &poisoned) == ok && aevix_rwlock_write_unlock(&state, 1) == ok);
        InlineRwLockState relocated;
        std::memcpy(&relocated, &state, sizeof state);
        state.bits = 0;
        REQUIRE(aevix_rwlock_write(&relocated, &poisoned) == ok && poisoned == 1);
        REQUIRE(aevix_rwlock_clear_poison(&relocated) == ok && aevix_rwlock_write_unlock(&relocated, 0) == ok);
        return true;
    }
    bool mutexThreads()
    {
        InlineMutexState state;
        std::uint64_t value = 0;
        std::atomic<bool> good{true};
        std::array<std::thread, 6> threads;
        for (auto &thread : threads) thread = std::thread([&] {
            for (unsigned iteration = 0; iteration < 5000; ++iteration) {
                std::uint32_t poisoned = 0;
                if (aevix_mutex_lock(&state, &poisoned) != ok || poisoned) { good = false; return; }
                ++value;
                if (aevix_mutex_unlock(&state, 0) != ok) { good = false; return; }
            }
        });
        for (auto &thread : threads) thread.join();
        REQUIRE(good && value == 30000 && state.bits == 0);
        std::atomic<bool> ready{false};
        std::thread publishing([&] {
            std::uint32_t poisoned = 0;
            if (aevix_mutex_lock(&state, &poisoned) != ok) { good = false; ready = true; return; }
            value = 987654321;
            if (aevix_mutex_unlock(&state, 0) != ok) good = false;
            ready.store(true, std::memory_order_relaxed);
        });
        while (!ready.load(std::memory_order_relaxed)) std::this_thread::yield();
        std::uint32_t poisoned = 0;
        const auto acquired = aevix_mutex_lock(&state, &poisoned);
        const auto published = value;
        const auto released = aevix_mutex_unlock(&state, 0);
        publishing.join();
        REQUIRE(good && acquired == ok && released == ok && published == 987654321);
        return true;
    }
    bool rwThreads()
    {
        InlineRwLockState state;
        std::uint64_t version = 0, inverse = UINT64_MAX;
        std::atomic<bool> good{true};
        std::array<std::thread, 6> threads;
        for (unsigned index = 0; index < threads.size(); ++index) threads[index] = std::thread([&, index] {
            for (unsigned iteration = 0; iteration < 4000; ++iteration) {
                std::uint32_t poisoned = 0;
                if (index < 2) {
                    if (aevix_rwlock_write(&state, &poisoned) != ok || poisoned) { good = false; return; }
                    ++version;
                    inverse = ~version;
                    if (aevix_rwlock_write_unlock(&state, 0) != ok) { good = false; return; }
                } else {
                    if (aevix_rwlock_read(&state, &poisoned) != ok || poisoned) { good = false; return; }
                    if (inverse != ~version) good = false;
                    if (aevix_rwlock_read_unlock(&state) != ok) { good = false; return; }
                }
            }
        });
        for (auto &thread : threads) thread.join();
        REQUIRE(good && version == 8000 && inverse == ~version && state.bits == 0);
        // 두 독립 읽기 보호자는 실제로 동시에 살아 있을 수 있어야 한다.
        // Two independent read guards must be able to remain live concurrently.
        std::uint32_t poisoned = 0;
        REQUIRE(aevix_rwlock_read(&state, &poisoned) == ok);
        std::atomic<bool> acquired{false}, finish{false};
        std::thread secondReader([&] {
            std::uint32_t flag = 0;
            if (aevix_rwlock_read(&state, &flag) != ok) { good = false; acquired = true; return; }
            acquired = true;
            while (!finish.load()) std::this_thread::yield();
            if (aevix_rwlock_read_unlock(&state) != ok) good = false;
        });
        while (!acquired.load()) std::this_thread::yield();
        const auto exclusive = aevix_rwlock_try_write(&state, &poisoned);
        finish = true;
        secondReader.join();
        const auto firstReleased = aevix_rwlock_read_unlock(&state);
        REQUIRE(good && exclusive == busy && firstReleased == ok && state.bits == 0);
        return true;
    }
}
int main()
{
    Deadline deadline;
    if (!mutexContracts() || !rwContracts() || !mutexThreads() || !rwThreads()) return 1;
    std::printf("Inline Mutex/RwLock: %u checks, 30000 mutex updates, 8000 writes and 16000 reads passed.\n", checks);
    return 0;
}
