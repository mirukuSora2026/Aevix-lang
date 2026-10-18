// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_shared_runtime_tests.cpp
//
// Description :
//     Shared와 Weak의 실패 원자성, 정렬, 재진입 정리, 참조 수와 네이티브 경합을 검증한다.
//     Verify Shared/Weak failure atomicity, alignment, reentrant cleanup, counts, and native races.
//
// ========================
#include "aevix/Runtime/SharedRuntime.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <thread>

#if !defined(AEVIX_SHARED_RUNTIME_TESTING)
#error "Compile this dedicated test with AEVIX_SHARED_RUNTIME_TESTING."
#endif

namespace
{
    unsigned checks = 0;
    constexpr std::int32_t ok = 0, allocation = 1, overflow = 2, invalid = 3, stale = 4,
                           state = 5, countOverflow = 6, expired = 7, notUnique = 8;
    bool require(bool condition, const char *expression, int line)
    {
        ++checks;
        if (!condition) std::fprintf(stderr, "Shared runtime check failed at %d: %s\n", line, expression);
        return condition;
    }
#define REQUIRE(...) do { if (!require(bool(__VA_ARGS__), #__VA_ARGS__, __LINE__)) return false; } while (false)
    struct Deadline final
    {
        std::atomic<bool> done{false};
        std::thread worker;
        Deadline() : worker([this] {
            const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (!done.load()) {
                if (std::chrono::steady_clock::now() > limit) {
                    std::fputs("Shared runtime dependency test exceeded its bounded deadline.\n", stderr);
                    std::_Exit(91);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }) {}
        ~Deadline() { done = true; worker.join(); }
    };
    struct Payload final
    {
        std::atomic<unsigned> *drops;
        int *resource;
        std::uint64_t value;
    };
    void dropPayload(void *storage) noexcept
    {
        auto &payload = *static_cast<Payload *>(storage);
        delete payload.resource;
        payload.drops->fetch_add(1);
    }
    bool create(std::atomic<unsigned> &drops, std::uint64_t value, std::uint64_t &owner)
    {
        void *storage = nullptr;
        REQUIRE(aevix_shared_prepare(sizeof(Payload), alignof(Payload), dropPayload, &owner, &storage) == ok);
        auto *resource = new (std::nothrow) int(37);
        REQUIRE(resource);
        new (storage) Payload{&drops, resource, value};
        REQUIRE(aevix_shared_commit(owner) == ok);
        return true;
    }
    std::atomic<unsigned> zeroDrops{0};
    void dropZero(void *) noexcept { ++zeroDrops; }
    bool construction()
    {
        std::uint64_t owner = 91; void *storage = reinterpret_cast<void *>(std::uintptr_t{1});
        REQUIRE(aevix_shared_prepare(1, 0, nullptr, &owner, &storage) == invalid && owner == 0 && !storage);
        REQUIRE(aevix_shared_prepare(1, 3, nullptr, &owner, &storage) == invalid && owner == 0 && !storage);
        REQUIRE(aevix_shared_prepare(UINT64_MAX, 1, nullptr, &owner, &storage) == overflow && owner == 0 && !storage);
        REQUIRE(aevix_shared_prepare(1, std::uint64_t{1} << 63, nullptr, &owner, &storage) == overflow);
        REQUIRE(aevix_shared_prepare(1, 1, nullptr, nullptr, &storage) == invalid && !storage);
        REQUIRE(aevix_shared_prepare(1, 1, nullptr, &owner, nullptr) == invalid && owner == 0);
        REQUIRE(aevix_shared_test_live_controls() == 0);
        for (std::uint64_t allowed : {0u, 1u, 2u}) {
            aevix_shared_test_fail_after(allowed);
            const auto result = aevix_shared_prepare(sizeof(Payload), alignof(Payload), dropPayload, &owner, &storage);
            aevix_shared_test_fail_after(UINT64_MAX);
            REQUIRE(result == allocation && !owner && !storage);
            REQUIRE(aevix_shared_test_live_controls() == 0);
        }
        REQUIRE(aevix_shared_prepare(0, 256, dropZero, &owner, &storage) == ok);
        REQUIRE(owner && storage && reinterpret_cast<std::uintptr_t>(storage) % 256 == 0);
        REQUIRE(aevix_shared_test_live_controls() == 1);
        std::uint64_t copy = 99;
        REQUIRE(aevix_shared_clone(owner, &copy) == state && !copy);
        REQUIRE(aevix_shared_release(owner) == state);
        REQUIRE(aevix_shared_payload(owner, &storage) == state && !storage);
        REQUIRE(aevix_shared_abort(owner) == ok && zeroDrops == 0);
        REQUIRE(aevix_shared_abort(owner) == stale);
        REQUIRE(aevix_shared_test_live_controls() == 0);
        const auto old = owner;
        REQUIRE(aevix_shared_prepare(0, 4096, dropZero, &owner, &storage) == ok && owner != old);
        REQUIRE(reinterpret_cast<std::uintptr_t>(storage) % 4096 == 0);
        aevix_shared_test_fail_after(0);
        const auto committed = aevix_shared_commit(owner);
        aevix_shared_test_fail_after(UINT64_MAX);
        REQUIRE(committed == ok);
        REQUIRE(aevix_shared_commit(owner) == state);
        REQUIRE(aevix_shared_abort(owner) == state);
        REQUIRE(aevix_shared_release(old) == stale);
        REQUIRE(aevix_shared_release(owner) == ok && zeroDrops == 1);
        REQUIRE(aevix_shared_test_live_controls() == 0);
        return true;
    }
    struct alignas(256) Wide final { std::atomic<unsigned> *drops; std::array<unsigned char, 257> bytes; };
    void dropWide(void *storage) noexcept { ++*static_cast<Wide *>(storage)->drops; }
    bool allocationOwnership()
    {
        std::atomic<unsigned> drops{0};
        auto *original = new (std::nothrow) int(91);
        REQUIRE(original);
        Payload input{&drops, original, 73};
        std::uint64_t owner = 99; void *storage = reinterpret_cast<void *>(std::uintptr_t{1});
        aevix_shared_test_fail_after(0);
        const auto failed = aevix_shared_prepare(sizeof(Payload), alignof(Payload), dropPayload, &owner, &storage);
        aevix_shared_test_fail_after(UINT64_MAX);
        REQUIRE(failed == allocation && !owner && !storage && input.resource == original && *input.resource == 91 && drops == 0);
        REQUIRE(aevix_shared_prepare(sizeof(Payload), alignof(Payload), dropPayload, &owner, &storage) == ok);
        new (storage) Payload(input);
        input.resource = nullptr; // prepare 성공 후에만 소스 소유권을 이동한다.
                                  // Relocate source ownership only after successful prepare.
        REQUIRE(aevix_shared_commit(owner) == ok && aevix_shared_release(owner) == ok && drops == 1);
        REQUIRE(aevix_shared_prepare(sizeof(Wide), alignof(Wide), dropWide, &owner, &storage) == ok);
        REQUIRE(reinterpret_cast<std::uintptr_t>(storage) % alignof(Wide) == 0);
        auto *wide = new (storage) Wide{&drops, {}};
        wide->bytes[256] = 0xa7;
        REQUIRE(aevix_shared_commit(owner) == ok);
        alignas(Wide) std::array<unsigned char, sizeof(Wide)> copied{};
        REQUIRE(aevix_shared_take(owner, copied.data(), copied.size()) == ok && drops == 1);
        Wide result{};
        std::memcpy(&result, copied.data(), sizeof result);
        REQUIRE(result.bytes[256] == 0xa7 && result.drops == &drops);
        dropWide(&result);
        REQUIRE(drops == 2 && aevix_shared_test_live_controls() == 0);
        return true;
    }
    bool ownership()
    {
        std::atomic<unsigned> drops{0};
        std::uint64_t owner = 0, copy = 0, weak = 0, weakCopy = 0, upgraded = 0;
        REQUIRE(create(drops, 9876, owner));
        std::uint64_t strongCount = 0, weakCount = 0;
        REQUIRE(aevix_shared_counts(owner, &strongCount, &weakCount) == ok && strongCount == 1 && weakCount == 0);
        void *storage = nullptr;
        REQUIRE(aevix_shared_unique_payload(owner, &storage) == ok && static_cast<Payload *>(storage)->value == 9876);
        REQUIRE(aevix_shared_clone(owner, &copy) == ok && copy == owner);
        REQUIRE(aevix_shared_unique_payload(owner, &storage) == notUnique && !storage);
        REQUIRE(aevix_shared_downgrade(owner, &weak) == ok && (weak >> 63) == 1);
        REQUIRE(aevix_shared_clone(weak, &upgraded) == stale && !upgraded);
        REQUIRE(aevix_weak_clone(owner, &weakCopy) == stale && !weakCopy);
        REQUIRE(aevix_weak_clone(weak, &weakCopy) == ok && weakCopy == weak);
        REQUIRE(aevix_shared_counts(weak, &strongCount, &weakCount) == ok && strongCount == 2 && weakCount == 2);
        REQUIRE(aevix_weak_upgrade(weak, &upgraded) == ok && upgraded == owner);
        REQUIRE(aevix_shared_payload(upgraded, &storage) == ok && *static_cast<Payload *>(storage)->resource == 37);
        std::uint32_t same = 0;
        REQUIRE(aevix_shared_same_allocation(owner, weak, &same) == ok && same);
        REQUIRE(aevix_shared_release(owner) == ok && drops == 0);
        REQUIRE(aevix_shared_release(copy) == ok && drops == 0);
        REQUIRE(aevix_shared_release(upgraded) == ok && drops == 1);
        REQUIRE(aevix_shared_counts(weak, &strongCount, &weakCount) == ok && strongCount == 0 && weakCount == 2);
        REQUIRE(aevix_shared_test_live_controls() == 1);
        REQUIRE(aevix_weak_upgrade(weak, &upgraded) == expired && !upgraded);
        REQUIRE(aevix_weak_clone(weak, &upgraded) == ok && upgraded == weak);
        REQUIRE(aevix_weak_release(upgraded) == ok);
        REQUIRE(aevix_shared_release(owner) == expired && drops == 1);
        REQUIRE(aevix_shared_payload(owner, &storage) == expired && !storage);
        REQUIRE(aevix_weak_release(weakCopy) == ok && aevix_shared_test_live_controls() == 1);
        REQUIRE(aevix_weak_release(weak) == ok && aevix_shared_test_live_controls() == 0);
        REQUIRE(aevix_weak_release(weak) == stale && drops == 1);
        REQUIRE(aevix_shared_payload(owner, &storage) == stale);
        REQUIRE(aevix_shared_release(0) == stale && aevix_weak_release(0) == stale);
        REQUIRE(aevix_shared_clone(0, nullptr) == invalid);
        REQUIRE(aevix_shared_downgrade(0, nullptr) == invalid);
        REQUIRE(aevix_weak_upgrade(0, nullptr) == invalid);
        REQUIRE(aevix_shared_counts(0, &strongCount, &strongCount) == invalid);
        REQUIRE(create(drops, 3, owner));
        std::uint64_t other = 0;
        REQUIRE(create(drops, 4, other));
        REQUIRE(aevix_shared_same_allocation(owner, other, &same) == ok && !same);
        REQUIRE(aevix_shared_release(owner) == ok && aevix_shared_release(other) == ok && drops == 3);
        return true;
    }
    bool overflowAndTake()
    {
        std::atomic<unsigned> drops{0};
        std::uint64_t owner = 0, weak = 0, output = 99, strong = 0, count = 0;
        REQUIRE(create(drops, 42, owner));
        REQUIRE(aevix_shared_downgrade(owner, &weak) == ok);
        REQUIRE(aevix_shared_test_set_counts(owner, UINT64_MAX, UINT64_MAX) == ok);
        REQUIRE(aevix_shared_clone(owner, &output) == countOverflow && !output);
        REQUIRE(aevix_shared_downgrade(owner, &output) == countOverflow && !output);
        REQUIRE(aevix_weak_clone(weak, &output) == countOverflow && !output);
        REQUIRE(aevix_weak_upgrade(weak, &output) == countOverflow && !output);
        REQUIRE(aevix_shared_counts(weak, &strong, &count) == ok && strong == UINT64_MAX && count == UINT64_MAX);
        REQUIRE(aevix_shared_test_set_counts(owner, 1, 1) == ok);
        void *storage = nullptr;
        REQUIRE(aevix_shared_unique_payload(owner, &storage) == notUnique && !storage);
        REQUIRE(aevix_shared_payload(owner, &storage) == ok);
        Payload taken{};
        REQUIRE(aevix_shared_take(owner, &taken, sizeof(Payload) - 1) == invalid);
        REQUIRE(aevix_shared_take(owner, nullptr, sizeof(Payload)) == invalid);
        REQUIRE(aevix_shared_take(owner, storage, sizeof(Payload)) == invalid);
        REQUIRE(aevix_shared_take(owner, reinterpret_cast<void *>(UINTPTR_MAX - 1), sizeof(Payload)) == overflow);
        REQUIRE(aevix_shared_counts(owner, &strong, &count) == ok && strong == 1 && count == 1);
        REQUIRE(aevix_shared_clone(owner, &output) == ok);
        REQUIRE(aevix_shared_take(owner, &taken, sizeof(Payload)) == notUnique && !taken.resource);
        REQUIRE(aevix_shared_release(output) == ok);
        REQUIRE(aevix_shared_take(owner, &taken, sizeof(Payload)) == ok && taken.resource && taken.value == 42 && drops == 0);
        REQUIRE(aevix_weak_upgrade(weak, &output) == expired && !output);
        REQUIRE(aevix_weak_release(weak) == ok && aevix_shared_test_live_controls() == 0);
        dropPayload(&taken);
        REQUIRE(drops == 1);
        REQUIRE(aevix_shared_prepare(0, 1, dropZero, &owner, &storage) == ok);
        REQUIRE(aevix_shared_commit(owner) == ok);
        REQUIRE(aevix_shared_take(owner, nullptr, 0) == ok && zeroDrops == 1);
        REQUIRE(aevix_shared_test_live_controls() == 0);
        return true;
    }
    bool upgradeRace()
    {
        for (unsigned round = 0; round < 100; ++round) {
            std::atomic<unsigned> drops{0};
            std::uint64_t owner = 0, weak = 0;
            REQUIRE(create(drops, round, owner));
            REQUIRE(aevix_shared_downgrade(owner, &weak) == ok);
            std::atomic<bool> start{false}, good{true};
            std::thread upgrading([&] {
                while (!start.load()) std::this_thread::yield();
                std::uint64_t acquired = 0;
                const auto result = aevix_weak_upgrade(weak, &acquired);
                if (result == ok) {
                    void *storage = nullptr;
                    if (aevix_shared_payload(acquired, &storage) != ok || static_cast<Payload *>(storage)->value != round || aevix_shared_release(acquired) != ok) good = false;
                } else if (result != expired || acquired) good = false;
            });
            start = true;
            const auto released = aevix_shared_release(owner);
            upgrading.join();
            REQUIRE(released == ok && good && drops == 1);
            REQUIRE(aevix_weak_release(weak) == ok && aevix_shared_test_live_controls() == 0);
        }
        return true;
    }
    struct Reentrant final { std::atomic<unsigned> *drops; std::uint64_t weak; std::uint64_t nested; };
    std::atomic<bool> callbackGood{true};
    void dropReentrant(void *storage) noexcept
    {
        auto &value = *static_cast<Reentrant *>(storage);
        std::uint64_t upgraded = 99;
        if (aevix_weak_upgrade(value.weak, &upgraded) != expired || upgraded) callbackGood = false;
        if (aevix_weak_release(value.weak) != ok) callbackGood = false;
        // 마지막 Weak 정리 후에도 현재 정리 중인 제어 블록이 살아 있어야 한다.
        // The active control block must remain alive even after releasing its final Weak.
        if (aevix_shared_test_live_controls() != 2) callbackGood = false;
        if (aevix_shared_release(value.nested) != ok) callbackGood = false;
        ++*value.drops;
    }
    bool reentrant()
    {
        std::atomic<unsigned> drops{0}, nestedDrops{0};
        std::uint64_t nested = 0, owner = 0, weak = 0; void *storage = nullptr;
        REQUIRE(create(nestedDrops, 8, nested));
        REQUIRE(aevix_shared_prepare(sizeof(Reentrant), alignof(Reentrant), dropReentrant, &owner, &storage) == ok);
        auto *value = new (storage) Reentrant{&drops, 0, nested};
        REQUIRE(aevix_shared_commit(owner) == ok);
        REQUIRE(aevix_shared_downgrade(owner, &weak) == ok);
        value->weak = weak;
        REQUIRE(aevix_shared_release(owner) == ok && callbackGood && drops == 1 && nestedDrops == 1);
        REQUIRE(aevix_shared_test_live_controls() == 0);
        return true;
    }
    struct Blocking final { std::atomic<bool> *entered; std::atomic<bool> *finish; std::atomic<unsigned> *drops; };
    void dropBlocking(void *storage) noexcept
    {
        auto &value = *static_cast<Blocking *>(storage);
        *value.entered = true;
        while (!value.finish->load()) std::this_thread::yield();
        ++*value.drops;
    }
    bool concurrency()
    {
        std::atomic<unsigned> drops{0};
        std::uint64_t owner = 0, weak = 0;
        REQUIRE(create(drops, 6789, owner));
        REQUIRE(aevix_shared_downgrade(owner, &weak) == ok);
        std::atomic<bool> good{true};
        std::array<std::thread, 6> workers;
        for (auto &worker : workers) worker = std::thread([&] {
            for (unsigned i = 0; i < 300; ++i) {
                std::uint64_t local = 0, localWeak = 0; void *storage = nullptr;
                if (aevix_shared_clone(owner, &local) != ok || aevix_shared_payload(local, &storage) != ok || static_cast<Payload *>(storage)->value != 6789) { good = false; return; }
                if (aevix_weak_clone(weak, &localWeak) != ok || aevix_weak_release(localWeak) != ok || aevix_shared_release(local) != ok) { good = false; return; }
            }
        });
        for (auto &worker : workers) worker.join();
        REQUIRE(good && drops == 0);
        REQUIRE(aevix_shared_release(owner) == ok && drops == 1);
        REQUIRE(aevix_weak_release(weak) == ok && aevix_shared_test_live_controls() == 0);
        std::atomic<bool> entered{false}, finish{false};
        drops = 0; void *storage = nullptr;
        REQUIRE(aevix_shared_prepare(sizeof(Blocking), alignof(Blocking), dropBlocking, &owner, &storage) == ok);
        new (storage) Blocking{&entered, &finish, &drops};
        REQUIRE(aevix_shared_commit(owner) == ok);
        REQUIRE(aevix_shared_downgrade(owner, &weak) == ok);
        std::atomic<std::int32_t> released{99};
        std::thread finalStrong([&] { released = aevix_shared_release(owner); });
        while (!entered.load()) std::this_thread::yield();
        const auto lastWeak = aevix_weak_release(weak);
        const auto controlsDuringDrop = aevix_shared_test_live_controls();
        finish = true;
        finalStrong.join();
        REQUIRE(lastWeak == ok && controlsDuringDrop == 1 && released == ok && drops == 1);
        REQUIRE(aevix_shared_test_live_controls() == 0);
        return true;
    }
}
int main()
{
    Deadline deadline;
    if (!construction() || !allocationOwnership() || !ownership() || !overflowAndTake() || !reentrant() || !concurrency() || !upgradeRace()) return 1;
    std::printf("Shared/Weak runtime: %u checks, 1,800 concurrent ownership iterations, and 100 upgrade/drop races passed.\n", checks);
    return 0;
}
