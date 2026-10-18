// ========================
//
// Created By : miruku_sora2026
//
// File name : LockRuntime.cpp
//
// Description :
//     8바이트 정수 상태에서 할당 없는 Mutex·RwLock, 제한 대기와 권고성 오염을 구현한다.
//     Implement allocation-free Mutex/RwLock, bounded waiting, and advisory poison in eight-byte integer states.
//
// ========================
#include "aevix/Runtime/LockRuntime.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <thread>

namespace
{
    using aevix::LockStatus;
    using Atomic = std::atomic_ref<std::uint64_t>;
    static_assert(Atomic::is_always_lock_free, "Supported CPU lock state requires lock-free 64-bit atomic_ref.");
    static_assert(Atomic::required_alignment <= 8, "Compiler inline lock state has alignment 8.");
    constexpr std::uint64_t mutexLocked = 1, mutexPoison = 2;
    constexpr std::uint64_t writer = std::uint64_t{1} << 63, rwPoison = std::uint64_t{1} << 62;
    constexpr std::uint64_t readers = rwPoison - 1;
    constexpr std::int32_t good = 0;
    constexpr std::int32_t status(LockStatus value) noexcept { return std::int32_t(value); }
    bool validState(void *state) noexcept
    {
        const auto address = reinterpret_cast<std::uintptr_t>(state);
        return state && address % 8 == 0 && address <= UINTPTR_MAX - 8;
    }
    bool validOutput(void *state, std::uint32_t *output) noexcept
    {
        if (!validState(state) || !output) return false;
        const auto address = reinterpret_cast<std::uintptr_t>(output), owner = reinterpret_cast<std::uintptr_t>(state);
        return address % alignof(std::uint32_t) == 0 && address <= UINTPTR_MAX - sizeof(std::uint32_t)
            && !(address < owner + 8 && owner < address + sizeof(std::uint32_t));
    }
    Atomic atomic(void *state) noexcept { return Atomic(*static_cast<std::uint64_t *>(state)); }
    bool invalidMutex(std::uint64_t state) noexcept { return (state & ~(mutexLocked | mutexPoison)) != 0; }
    bool invalidRw(std::uint64_t state) noexcept { return (state & writer) && (state & readers); }
    using Attempt = std::int32_t (*)(void *, std::uint32_t *) noexcept;
    std::int32_t wait(void *state, std::uint32_t *poisoned, Attempt attempt) noexcept
    {
        for (;;) {
            const auto result = attempt(state, poisoned);
            if (result != status(LockStatus::Busy)) return result;
            std::this_thread::yield();
        }
    }
    std::int32_t timeout(void *state, std::uint64_t nanoseconds, std::uint32_t *poisoned, Attempt attempt) noexcept
    {
        if (!validOutput(state, poisoned)) return status(LockStatus::InvalidInput);
        *poisoned = 0;
        if (nanoseconds > std::uint64_t(INT64_MAX)) return status(LockStatus::InvalidInput);
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            const auto result = attempt(state, poisoned);
            if (result != status(LockStatus::Busy)) return result;
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed >= 0 && std::uint64_t(elapsed) >= nanoseconds) return status(LockStatus::TimedOut);
            std::this_thread::yield();
        }
    }
    std::int32_t poisonSnapshot(void *state, std::uint32_t *output, bool shared) noexcept
    {
        if (!validOutput(state, output)) return status(LockStatus::InvalidInput);
        *output = 0;
        const auto bits = atomic(state).load(std::memory_order_relaxed);
        if (shared ? invalidRw(bits) : invalidMutex(bits)) return status(LockStatus::InvalidState);
        *output = bool(bits & (shared ? rwPoison : mutexPoison));
        return good;
    }
    std::int32_t releaseExclusive(void *state, std::uint32_t markPoison, bool shared) noexcept
    {
        if (!validState(state) || markPoison > 1) return status(LockStatus::InvalidInput);
        auto cell = atomic(state);
        auto previous = cell.load(std::memory_order_relaxed);
        for (;;) {
            if (shared ? invalidRw(previous) : invalidMutex(previous)) return status(LockStatus::InvalidState);
            const auto held = shared ? writer : mutexLocked;
            if (!(previous & held)) return status(LockStatus::InvalidState);
            const auto updated = (previous & ~held) | (markPoison ? (shared ? rwPoison : mutexPoison) : 0);
            if (cell.compare_exchange_weak(previous, updated, std::memory_order_release, std::memory_order_relaxed)) return good;
        }
    }
    std::int32_t clearPoison(void *state, bool shared) noexcept
    {
        if (!validState(state)) return status(LockStatus::InvalidInput);
        auto cell = atomic(state);
        auto previous = cell.load(std::memory_order_relaxed);
        for (;;) {
            if (shared ? invalidRw(previous) : invalidMutex(previous)) return status(LockStatus::InvalidState);
            if (!(previous & (shared ? writer : mutexLocked))) return status(LockStatus::InvalidState);
            const auto updated = previous & ~(shared ? rwPoison : mutexPoison);
            if (cell.compare_exchange_weak(previous, updated, std::memory_order_relaxed, std::memory_order_relaxed)) return good;
        }
    }
}
extern "C"
{
    std::int32_t aevix_mutex_init(void *state) noexcept
    {
        if (!validState(state)) return status(LockStatus::InvalidInput);
        *static_cast<std::uint64_t *>(state) = 0; return good;
    }
    std::int32_t aevix_mutex_try_lock(void *state, std::uint32_t *poisoned) noexcept
    {
        if (!validOutput(state, poisoned)) return status(LockStatus::InvalidInput);
        *poisoned = 0;
        auto cell = atomic(state);
        auto previous = cell.load(std::memory_order_relaxed);
        if (invalidMutex(previous)) return status(LockStatus::InvalidState);
        if (previous & mutexLocked) return status(LockStatus::Busy);
        if (!cell.compare_exchange_strong(previous, previous | mutexLocked, std::memory_order_acquire, std::memory_order_relaxed))
            return status(LockStatus::Busy);
        *poisoned = bool(previous & mutexPoison); return good;
    }
    std::int32_t aevix_mutex_lock(void *state, std::uint32_t *poisoned) noexcept
    { return wait(state, poisoned, aevix_mutex_try_lock); }
    std::int32_t aevix_mutex_lock_timeout(void *state, std::uint64_t nanoseconds, std::uint32_t *poisoned) noexcept
    { return timeout(state, nanoseconds, poisoned, aevix_mutex_try_lock); }
    std::int32_t aevix_mutex_unlock(void *state, std::uint32_t markPoison) noexcept
    { return releaseExclusive(state, markPoison, false); }
    std::int32_t aevix_mutex_is_poisoned(void *state, std::uint32_t *output) noexcept
    { return poisonSnapshot(state, output, false); }
    std::int32_t aevix_mutex_clear_poison(void *state) noexcept { return clearPoison(state, false); }
    std::int32_t aevix_rwlock_init(void *state) noexcept { return aevix_mutex_init(state); }
    std::int32_t aevix_rwlock_try_read(void *state, std::uint32_t *poisoned) noexcept
    {
        if (!validOutput(state, poisoned)) return status(LockStatus::InvalidInput);
        *poisoned = 0;
        auto cell = atomic(state);
        auto previous = cell.load(std::memory_order_relaxed);
        if (invalidRw(previous)) return status(LockStatus::InvalidState);
        if (previous & writer) return status(LockStatus::Busy);
        if ((previous & readers) == readers) return status(LockStatus::ReaderOverflow);
        if (!cell.compare_exchange_strong(previous, previous + 1, std::memory_order_acquire, std::memory_order_relaxed))
            return status(LockStatus::Busy);
        *poisoned = bool(previous & rwPoison); return good;
    }
    std::int32_t aevix_rwlock_try_write(void *state, std::uint32_t *poisoned) noexcept
    {
        if (!validOutput(state, poisoned)) return status(LockStatus::InvalidInput);
        *poisoned = 0;
        auto cell = atomic(state);
        auto previous = cell.load(std::memory_order_relaxed);
        if (invalidRw(previous)) return status(LockStatus::InvalidState);
        if (previous & (writer | readers)) return status(LockStatus::Busy);
        if (!cell.compare_exchange_strong(previous, previous | writer, std::memory_order_acquire, std::memory_order_relaxed))
            return status(LockStatus::Busy);
        *poisoned = bool(previous & rwPoison); return good;
    }
    std::int32_t aevix_rwlock_read(void *state, std::uint32_t *poisoned) noexcept
    { return wait(state, poisoned, aevix_rwlock_try_read); }
    std::int32_t aevix_rwlock_write(void *state, std::uint32_t *poisoned) noexcept
    { return wait(state, poisoned, aevix_rwlock_try_write); }
    std::int32_t aevix_rwlock_read_timeout(void *state, std::uint64_t nanoseconds, std::uint32_t *poisoned) noexcept
    { return timeout(state, nanoseconds, poisoned, aevix_rwlock_try_read); }
    std::int32_t aevix_rwlock_write_timeout(void *state, std::uint64_t nanoseconds, std::uint32_t *poisoned) noexcept
    { return timeout(state, nanoseconds, poisoned, aevix_rwlock_try_write); }
    std::int32_t aevix_rwlock_read_unlock(void *state) noexcept
    {
        if (!validState(state)) return status(LockStatus::InvalidInput);
        auto cell = atomic(state);
        auto previous = cell.load(std::memory_order_relaxed);
        for (;;) {
            if (invalidRw(previous) || (previous & writer) || !(previous & readers)) return status(LockStatus::InvalidState);
            if (cell.compare_exchange_weak(previous, previous - 1, std::memory_order_release, std::memory_order_relaxed)) return good;
        }
    }
    std::int32_t aevix_rwlock_write_unlock(void *state, std::uint32_t markPoison) noexcept
    { return releaseExclusive(state, markPoison, true); }
    std::int32_t aevix_rwlock_is_poisoned(void *state, std::uint32_t *output) noexcept
    { return poisonSnapshot(state, output, true); }
    std::int32_t aevix_rwlock_clear_poison(void *state) noexcept { return clearPoison(state, true); }
#if defined(AEVIX_LOCK_RUNTIME_TESTING)
    std::int32_t aevix_lock_test_set_state(void *state, std::uint64_t bits) noexcept
    {
        if (!validState(state)) return status(LockStatus::InvalidInput);
        atomic(state).store(bits, std::memory_order_relaxed); return good;
    }
#endif
}
