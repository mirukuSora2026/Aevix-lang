// ========================
//
// Created By : miruku_sora2026
//
// File name : SharedRuntime.cpp
//
// Description :
//     Shared와 Weak의 검사된 참조 수, 정렬된 페이로드와 재진입 가능한 정확한 정리를 구현한다.
//     Implement checked Shared/Weak counts, aligned payloads, and reentrant exactly-once destruction.
//
// ========================
#include "aevix/Runtime/SharedRuntime.hpp"
#include "aevix/Runtime/NativeBoundary.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>

namespace
{
    using aevix::SharedStatus;
    constexpr auto success = std::int32_t(SharedStatus::Success);
    constexpr auto weakBit = std::uint64_t{1} << 63;
    constexpr auto maximumGeneration = std::uint32_t{0x7fffffff};
    constexpr std::int32_t status(SharedStatus value) noexcept { return std::int32_t(value); }
    enum class State : unsigned char { Prepared, Live, Destroying, Expired };
    struct Control final
    {
        void *allocation = nullptr;
        void *payload = nullptr;
        std::uint64_t size = 0;
        std::uint64_t alignment = 0;
        std::uint64_t strong = 0;
        std::uint64_t weak = 0;
        aevix::SharedDrop drop = nullptr;
        State state = State::Prepared;
    };
    struct Entry final
    {
        Entry *next = nullptr;
        Control *control = nullptr;
        std::uint32_t slot = 0;
        std::uint32_t generation = 0;
    };
    struct Registry final
    {
        std::mutex mutex;
        Entry *head = nullptr;
        std::uint64_t nextSlot = 1;
        ~Registry() noexcept
        {
            // 정리 중인 소스 코드에 재진입하지 않는다. 정상 종료는 이미 모든 소유자를 정리해야 한다.
            // Never reenter source destructors during static teardown; normal shutdown must release owners first.
            for (auto *entry = head; entry; entry = entry->next) if (entry->control) return;
            while (head) { auto *next = head->next; delete head; head = next; }
        }
    };
    Registry &registry() noexcept { static Registry value; return value; }
#if defined(AEVIX_SHARED_RUNTIME_TESTING)
    thread_local std::uint64_t allocationBudget = UINT64_MAX;
#endif
    bool allowAllocation() noexcept
    {
#if defined(AEVIX_SHARED_RUNTIME_TESTING)
        if (!allocationBudget) return false;
        if (allocationBudget != UINT64_MAX) --allocationBudget;
#endif
        return true;
    }
    void *allocate(std::size_t size) noexcept { return allowAllocation() ? std::malloc(size) : nullptr; }
    std::uint64_t token(const Entry &entry, bool weak = false) noexcept
    {
        return (weak ? weakBit : 0) | (std::uint64_t(entry.generation) << 32) | entry.slot;
    }
    Entry *lookup(std::uint64_t owner, bool weak) noexcept
    {
        if (!owner || bool(owner & weakBit) != weak) return nullptr;
        const auto slot = std::uint32_t(owner), generation = std::uint32_t((owner & ~weakBit) >> 32);
        if (!slot || !generation) return nullptr;
        for (auto *entry = registry().head; entry; entry = entry->next)
            if (entry->slot == slot && entry->generation == generation && entry->control) return entry;
        return nullptr;
    }
    std::int32_t strongControl(std::uint64_t owner, Entry *&entry) noexcept
    {
        entry = lookup(owner, false);
        if (!entry) return status(SharedStatus::InvalidToken);
        if (entry->control->state == State::Prepared) return status(SharedStatus::InvalidState);
        if (!entry->control->strong) return status(SharedStatus::Expired);
        return success;
    }
    std::int32_t weakControl(std::uint64_t owner, Entry *&entry) noexcept
    {
        entry = lookup(owner, true);
        if (!entry || !entry->control->weak) return status(SharedStatus::InvalidToken);
        if (entry->control->state == State::Prepared) return status(SharedStatus::InvalidState);
        return success;
    }
    void freeControl(Control *control) noexcept
    {
        if (!control) return;
        std::free(control->allocation);
        delete control;
    }
    // Destroying 자체가 내부 메타데이터 보호자이다. 마지막 Weak도 정리 함수 실행 중에는 제어 블록을 해제하지 않는다.
    // Destroying is an internal metadata guard: even the last Weak cannot free control while its thunk is running.
    void finishPayload(Entry *entry, Control *control, bool destroy) noexcept
    {
        if (destroy && control->drop) control->drop(control->payload);
        std::free(control->allocation);
        Control *retired = nullptr;
        {
            std::lock_guard lock(registry().mutex);
            control->allocation = nullptr;
            control->payload = nullptr;
            control->state = State::Expired;
            if (!control->weak) { entry->control = nullptr; retired = control; }
        }
        delete retired;
    }
}

extern "C"
{
    std::int32_t aevix_shared_prepare(std::uint64_t size, std::uint64_t alignment, aevix::SharedDrop drop,
                                      std::uint64_t *output, void **payload) noexcept
    {
        if (output) *output = 0;
        if (payload) *payload = nullptr;
        if (!output || !payload || !alignment || (alignment & (alignment - 1))) return status(SharedStatus::InvalidInput);
        constexpr auto maximum = std::uint64_t(PTRDIFF_MAX);
        const auto storedSize = size ? size : 1;
        if (storedSize > maximum || alignment > maximum || storedSize > maximum - (alignment - 1))
            return status(SharedStatus::SizeOverflow);
        auto *control = allowAllocation() ? new (std::nothrow) Control : nullptr;
        if (!control) return status(SharedStatus::AllocationFailure);
        control->allocation = allocate(std::size_t(storedSize + alignment - 1));
        if (!control->allocation) { delete control; return status(SharedStatus::AllocationFailure); }
        const auto base = reinterpret_cast<std::uintptr_t>(control->allocation);
        if (base > UINTPTR_MAX - (storedSize + alignment - 1)) { freeControl(control); return status(SharedStatus::SizeOverflow); }
        control->payload = reinterpret_cast<void *>((base + alignment - 1) & ~(std::uintptr_t(alignment) - 1));
        control->size = size; control->alignment = alignment; control->drop = drop;
        std::lock_guard lock(registry().mutex);
        Entry *selected = nullptr;
        for (auto *entry = registry().head; entry; entry = entry->next)
            if (!entry->control && entry->generation < maximumGeneration) { selected = entry; break; }
        if (!selected)
        {
            if (registry().nextSlot > UINT32_MAX) { freeControl(control); return status(SharedStatus::SizeOverflow); }
            selected = allowAllocation() ? new (std::nothrow) Entry : nullptr;
            if (!selected) { freeControl(control); return status(SharedStatus::AllocationFailure); }
            selected->slot = std::uint32_t(registry().nextSlot++);
            selected->next = registry().head; registry().head = selected;
        }
        ++selected->generation;
        selected->control = control;
        *output = token(*selected); *payload = control->payload;
        return success;
    }
    std::int32_t aevix_shared_commit(std::uint64_t owner) noexcept
    {
        std::lock_guard lock(registry().mutex);
        auto *entry = lookup(owner, false);
        if (!entry) return status(SharedStatus::InvalidToken);
        if (entry->control->state != State::Prepared) return status(SharedStatus::InvalidState);
        entry->control->strong = 1; entry->control->state = State::Live;
        return success;
    }
    std::int32_t aevix_shared_abort(std::uint64_t owner) noexcept
    {
        Control *aborted = nullptr;
        {
            std::lock_guard lock(registry().mutex);
            auto *entry = lookup(owner, false);
            if (!entry) return status(SharedStatus::InvalidToken);
            if (entry->control->state != State::Prepared) return status(SharedStatus::InvalidState);
            aborted = entry->control; entry->control = nullptr;
        }
        freeControl(aborted); return success;
    }
    std::int32_t aevix_shared_clone(std::uint64_t owner, std::uint64_t *output) noexcept
    {
        if (!output) return status(SharedStatus::InvalidInput);
        *output = 0;
        std::lock_guard lock(registry().mutex);
        Entry *entry = nullptr;
        if (const auto result = strongControl(owner, entry)) return result;
        if (entry->control->strong == UINT64_MAX) return status(SharedStatus::CountOverflow);
        ++entry->control->strong; *output = owner; return success;
    }
    std::int32_t aevix_shared_release(std::uint64_t owner) noexcept
    {
        Entry *entry = nullptr; Control *control = nullptr;
        {
            std::lock_guard lock(registry().mutex);
            if (const auto result = strongControl(owner, entry)) return result;
            control = entry->control;
            if (--control->strong) return success;
            control->state = State::Destroying;
        }
        finishPayload(entry, control, true); return success;
    }
    std::int32_t aevix_shared_downgrade(std::uint64_t owner, std::uint64_t *output) noexcept
    {
        if (!output) return status(SharedStatus::InvalidInput);
        *output = 0;
        std::lock_guard lock(registry().mutex);
        Entry *entry = nullptr;
        if (const auto result = strongControl(owner, entry)) return result;
        if (entry->control->weak == UINT64_MAX) return status(SharedStatus::CountOverflow);
        ++entry->control->weak; *output = token(*entry, true); return success;
    }
    std::int32_t aevix_weak_clone(std::uint64_t owner, std::uint64_t *output) noexcept
    {
        if (!output) return status(SharedStatus::InvalidInput);
        *output = 0;
        std::lock_guard lock(registry().mutex);
        Entry *entry = nullptr;
        if (const auto result = weakControl(owner, entry)) return result;
        if (entry->control->weak == UINT64_MAX) return status(SharedStatus::CountOverflow);
        ++entry->control->weak; *output = owner; return success;
    }
    std::int32_t aevix_weak_release(std::uint64_t owner) noexcept
    {
        Control *retired = nullptr;
        {
            std::lock_guard lock(registry().mutex);
            Entry *entry = nullptr;
            if (const auto result = weakControl(owner, entry)) return result;
            auto *control = entry->control;
            --control->weak;
            if (!control->weak && control->state == State::Expired) { entry->control = nullptr; retired = control; }
        }
        delete retired; return success;
    }
    std::int32_t aevix_weak_upgrade(std::uint64_t owner, std::uint64_t *output) noexcept
    {
        if (!output) return status(SharedStatus::InvalidInput);
        *output = 0;
        std::lock_guard lock(registry().mutex);
        Entry *entry = nullptr;
        if (const auto result = weakControl(owner, entry)) return result;
        if (!entry->control->strong) return status(SharedStatus::Expired);
        if (entry->control->strong == UINT64_MAX) return status(SharedStatus::CountOverflow);
        ++entry->control->strong; *output = token(*entry); return success;
    }
    std::int32_t aevix_shared_payload(std::uint64_t owner, void **output) noexcept
    {
        if (!output) return status(SharedStatus::InvalidInput);
        *output = nullptr;
        std::lock_guard lock(registry().mutex);
        Entry *entry = nullptr;
        if (const auto result = strongControl(owner, entry)) return result;
        *output = entry->control->payload; return success;
    }
    std::int32_t aevix_shared_unique_payload(std::uint64_t owner, void **output) noexcept
    {
        if (!output) return status(SharedStatus::InvalidInput);
        *output = nullptr;
        std::lock_guard lock(registry().mutex);
        Entry *entry = nullptr;
        if (const auto result = strongControl(owner, entry)) return result;
        if (entry->control->strong != 1 || entry->control->weak) return status(SharedStatus::NotUnique);
        *output = entry->control->payload; return success;
    }
    std::int32_t aevix_shared_take(std::uint64_t owner, void *destination, std::uint64_t size) noexcept
    {
        const auto checked = aevix::NativeOutputBytes::checkedRaw(destination, size);
        if (!checked) return status(checked.status == aevix::NativeBufferStatus::NullWithNonzeroLength
                                     ? SharedStatus::InvalidInput : SharedStatus::SizeOverflow);
        Entry *entry = nullptr; Control *control = nullptr;
        {
            std::lock_guard lock(registry().mutex);
            if (const auto result = strongControl(owner, entry)) return result;
            control = entry->control;
            if (size != control->size) return status(SharedStatus::InvalidInput);
            if (size && (reinterpret_cast<std::uintptr_t>(destination) & (control->alignment - 1))) return status(SharedStatus::InvalidInput);
            if (control->strong != 1) return status(SharedStatus::NotUnique);
            const auto target = reinterpret_cast<std::uintptr_t>(destination);
            const auto source = reinterpret_cast<std::uintptr_t>(control->payload);
            if (size && target < source + size && source < target + size) return status(SharedStatus::InvalidInput);
            if (size) std::memcpy(destination, control->payload, std::size_t(size));
            control->strong = 0; control->state = State::Destroying;
        }
        finishPayload(entry, control, false); return success;
    }
    std::int32_t aevix_shared_counts(std::uint64_t owner, std::uint64_t *strong, std::uint64_t *weak) noexcept
    {
        if (strong) *strong = 0;
        if (weak) *weak = 0;
        if (!strong || !weak || strong == weak) return status(SharedStatus::InvalidInput);
        std::lock_guard lock(registry().mutex);
        Entry *entry = nullptr;
        const auto result = owner & weakBit ? weakControl(owner, entry) : strongControl(owner, entry);
        if (result) return result;
        *strong = entry->control->strong; *weak = entry->control->weak; return success;
    }
    std::int32_t aevix_shared_same_allocation(std::uint64_t first, std::uint64_t second, std::uint32_t *output) noexcept
    {
        if (!output) return status(SharedStatus::InvalidInput);
        *output = 0;
        std::lock_guard lock(registry().mutex);
        Entry *left = nullptr, *right = nullptr;
        auto result = first & weakBit ? weakControl(first, left) : strongControl(first, left);
        if (result) return result;
        result = second & weakBit ? weakControl(second, right) : strongControl(second, right);
        if (result) return result;
        *output = left == right; return success;
    }
#if defined(AEVIX_SHARED_RUNTIME_TESTING)
    void aevix_shared_test_fail_after(std::uint64_t count) noexcept { allocationBudget = count; }
    std::uint64_t aevix_shared_test_live_controls() noexcept
    {
        std::lock_guard lock(registry().mutex);
        std::uint64_t result = 0;
        for (auto *entry = registry().head; entry; entry = entry->next) if (entry->control) ++result;
        return result;
    }
    std::int32_t aevix_shared_test_set_counts(std::uint64_t owner, std::uint64_t strong, std::uint64_t weak) noexcept
    {
        std::lock_guard lock(registry().mutex);
        Entry *entry = nullptr;
        if (const auto result = strongControl(owner, entry)) return result;
        if (!strong) return status(SharedStatus::InvalidInput);
        entry->control->strong = strong; entry->control->weak = weak; return success;
    }
#endif
}
