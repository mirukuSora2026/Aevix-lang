// ========================
//
// Created By : miruku_sora2026
//
// File name : AtomicRuntime.cpp
//
// Description :
//     할당 없는 CPU 원자 셀 접근과 확인된 메모리 순서를 구현한다.
//     Implement allocation-free CPU atomic-cell access with validated memory orders.
//
// ========================

#include "aevix/Runtime/AtomicRuntime.hpp"

#include <atomic>
#include <bit>
#include <chrono>
#include <limits>
#include <thread>
#include <type_traits>

namespace
{
    using namespace aevix::runtime;

    constexpr std::int32_t status(AtomicStatus value) noexcept
    {
        return static_cast<std::int32_t>(value);
    }

    bool loadOrder(std::uint32_t order) noexcept
    {
        return order == 0 || order == 1 || order == 4;
    }

    bool storeOrder(std::uint32_t order) noexcept
    {
        return order == 0 || order == 2 || order == 4;
    }

    std::memory_order memoryOrder(std::uint32_t order) noexcept
    {
        switch (static_cast<AtomicOrder>(order))
        {
            case AtomicOrder::Relaxed: return std::memory_order_relaxed;
            case AtomicOrder::Acquire: return std::memory_order_acquire;
            case AtomicOrder::Release: return std::memory_order_release;
            case AtomicOrder::AcqRel: return std::memory_order_acq_rel;
            case AtomicOrder::SeqCst: return std::memory_order_seq_cst;
        }
        return std::memory_order_seq_cst;
    }

    // C++의 CAS 순서 전제조건을 만족하도록 내부 성공 순서만 강화할 수 있다.
    // Strengthen only the internal success order when needed to satisfy C++ CAS preconditions.
    std::memory_order casSuccess(std::uint32_t success, std::uint32_t failure) noexcept
    {
        if (failure == 4) return std::memory_order_seq_cst;
        if (failure == 1)
        {
            if (success == 0) return std::memory_order_acquire;
            if (success == 2) return std::memory_order_acq_rel;
        }
        return memoryOrder(success);
    }

    template<class T> T decode(std::uint64_t bits) noexcept
    {
        if constexpr (std::is_same_v<T, bool>) return bits != 0;
        else
        {
            using U = std::make_unsigned_t<T>;
            return std::bit_cast<T>(static_cast<U>(bits));
        }
    }

    template<class T> std::uint64_t encode(T value) noexcept
    {
        if constexpr (std::is_same_v<T, bool>) return value ? 1 : 0;
        else return static_cast<std::make_unsigned_t<T>>(value);
    }

    template<class T> bool aligned(void* storage) noexcept
    {
        return storage && reinterpret_cast<std::uintptr_t>(storage)
            % std::atomic_ref<T>::required_alignment == 0;
    }

    template<class T> bool addOverflow(T a, T b) noexcept
    {
        constexpr T lo = std::numeric_limits<T>::min(), hi = std::numeric_limits<T>::max();
        if constexpr (std::is_signed_v<T>) return (b > 0 && a > hi - b) || (b < 0 && a < lo - b);
        else return a > hi - b;
    }

    template<class T> bool subOverflow(T a, T b) noexcept
    {
        constexpr T lo = std::numeric_limits<T>::min(), hi = std::numeric_limits<T>::max();
        if constexpr (std::is_signed_v<T>) return (b > 0 && a < lo + b) || (b < 0 && a > hi + b);
        else return a < b;
    }

    template<class T> std::int32_t operate(void* storage, AtomicOperation operation,
        std::uint64_t operand, std::uint64_t expected, std::uint32_t success,
        std::uint32_t failure, std::uint64_t* observed, std::uint32_t* changed) noexcept
    {
        if (!aligned<T>(storage)) return status(AtomicStatus::InvalidStorage);
        if constexpr (std::is_same_v<T, bool>)
        {
            if (operation == AtomicOperation::FetchAdd || operation == AtomicOperation::FetchSub
                || operation == AtomicOperation::FetchMin || operation == AtomicOperation::FetchMax
                || operation == AtomicOperation::CheckedFetchAdd || operation == AtomicOperation::CheckedFetchSub
                || operation == AtomicOperation::SaturatingFetchAdd || operation == AtomicOperation::SaturatingFetchSub)
                return status(AtomicStatus::UnsupportedType);
        }
        std::atomic_ref<T> cell(*static_cast<T*>(storage));
        const auto order = memoryOrder(success);
        T value = decode<T>(operand);
        if (operation == AtomicOperation::Load) *observed = encode(cell.load(order));
        else if (operation == AtomicOperation::Store) cell.store(value, order);
        else if (operation == AtomicOperation::Swap) *observed = encode(cell.exchange(value, order));
        else if (operation == AtomicOperation::CompareExchange || operation == AtomicOperation::CompareExchangeWeak)
        {
            T old = decode<T>(expected);
            bool updated = operation == AtomicOperation::CompareExchange
                ? cell.compare_exchange_strong(old, value, casSuccess(success, failure), memoryOrder(failure))
                : cell.compare_exchange_weak(old, value, casSuccess(success, failure), memoryOrder(failure));
            *observed = encode(old);
            *changed = updated ? 1 : 0;
        }
        else if (operation == AtomicOperation::NotifyOne) cell.notify_one();
        else if (operation == AtomicOperation::NotifyAll) cell.notify_all();
        else
        {
            T old = cell.load(std::memory_order_relaxed);
            for (;;)
            {
                T replacement{};
                switch (operation)
                {
                    case AtomicOperation::FetchAnd: replacement = static_cast<T>(old & value); break;
                    case AtomicOperation::FetchOr: replacement = static_cast<T>(old | value); break;
                    case AtomicOperation::FetchXor: replacement = static_cast<T>(old ^ value); break;
                    case AtomicOperation::FetchNand:
                        if constexpr (std::is_same_v<T, bool>) replacement = !(old && value);
                        else replacement = static_cast<T>(~(old & value));
                        break;
                    default:
                        if constexpr (!std::is_same_v<T, bool>)
                        {
                            using U = std::make_unsigned_t<T>;
                            const U a = static_cast<U>(old), b = static_cast<U>(value);
                            const bool addition = operation == AtomicOperation::FetchAdd
                                || operation == AtomicOperation::CheckedFetchAdd || operation == AtomicOperation::SaturatingFetchAdd;
                            const bool subtraction = operation == AtomicOperation::FetchSub
                                || operation == AtomicOperation::CheckedFetchSub || operation == AtomicOperation::SaturatingFetchSub;
                            if (addition || subtraction)
                            {
                                bool overflow = addition ? addOverflow(old, value) : subOverflow(old, value);
                                if (overflow && (operation == AtomicOperation::CheckedFetchAdd || operation == AtomicOperation::CheckedFetchSub))
                                    return status(AtomicStatus::Overflow);
                                if (overflow && (operation == AtomicOperation::SaturatingFetchAdd || operation == AtomicOperation::SaturatingFetchSub))
                                {
                                    if constexpr (std::is_signed_v<T>)
                                        replacement = (addition ? value < 0 : value > 0)
                                            ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max();
                                    else replacement = addition ? std::numeric_limits<T>::max() : 0;
                                }
                                else replacement = std::bit_cast<T>(static_cast<U>(addition ? a + b : a - b));
                            }
                            else if (operation == AtomicOperation::FetchMin) replacement = old < value ? old : value;
                            else if (operation == AtomicOperation::FetchMax) replacement = old > value ? old : value;
                            else return status(AtomicStatus::InvalidOperation);
                        }
                        else return status(AtomicStatus::UnsupportedType);
                        break;
                }
                if (cell.compare_exchange_weak(old, replacement, order, std::memory_order_relaxed))
                {
                    *observed = encode(old);
                    *changed = 1;
                    break;
                }
            }
        }
        return status(AtomicStatus::Ok);
    }

    template<class F> auto dispatch(std::uint32_t kind, F&& callable) noexcept
    {
        switch (static_cast<AtomicKind>(kind))
        {
            case AtomicKind::I8: return callable.template operator()<std::int8_t>();
            case AtomicKind::U8: return callable.template operator()<std::uint8_t>();
            case AtomicKind::I16: return callable.template operator()<std::int16_t>();
            case AtomicKind::U16: return callable.template operator()<std::uint16_t>();
            case AtomicKind::I32: return callable.template operator()<std::int32_t>();
            case AtomicKind::U32: return callable.template operator()<std::uint32_t>();
            case AtomicKind::I64: return callable.template operator()<std::int64_t>();
            case AtomicKind::U64: return callable.template operator()<std::uint64_t>();
            case AtomicKind::Boolean: return callable.template operator()<bool>();
        }
        return decltype(callable.template operator()<bool>()){};
    }
}

extern "C" std::int32_t aevix_atomic_operation(void* storage, std::uint32_t kind,
    std::uint32_t operation, std::uint64_t operand, std::uint64_t expected,
    std::uint32_t success_order, std::uint32_t failure_order,
    std::uint64_t* observed, std::uint32_t* changed) noexcept
{
    if (observed) *observed = 0;
    if (changed) *changed = 0;
    if (!observed || !changed) return status(AtomicStatus::InvalidStorage);
    if (kind > static_cast<std::uint32_t>(AtomicKind::Boolean)) return status(AtomicStatus::UnsupportedType);
    if (operation > static_cast<std::uint32_t>(AtomicOperation::NotifyAll)) return status(AtomicStatus::InvalidOperation);
    const auto op = static_cast<AtomicOperation>(operation);
    if (success_order > 4 || failure_order > 4
        || (op == AtomicOperation::Load && !loadOrder(success_order))
        || (op == AtomicOperation::Store && !storeOrder(success_order))
        || ((op == AtomicOperation::CompareExchange || op == AtomicOperation::CompareExchangeWeak) && !loadOrder(failure_order)))
        return status(AtomicStatus::InvalidOrder);
    return dispatch(kind, [&]<class T>() { return operate<T>(storage, op, operand, expected, success_order, failure_order, observed, changed); });
}

extern "C" std::int32_t aevix_atomic_fence(std::uint32_t order, std::uint32_t compiler_only) noexcept
{
    if (order == 0 || order > 4) return status(AtomicStatus::InvalidOrder);
    if (compiler_only) std::atomic_signal_fence(memoryOrder(order));
    else std::atomic_thread_fence(memoryOrder(order));
    return status(AtomicStatus::Ok);
}

extern "C" std::int32_t aevix_atomic_wait(void* storage, std::uint32_t kind,
    std::uint64_t expected, std::uint32_t order, std::uint64_t timeout_ns,
    std::uint32_t finite, std::uint64_t* observed, std::uint32_t* changed) noexcept
{
    if (observed) *observed = 0;
    if (changed) *changed = 0;
    if (!observed || !changed) return status(AtomicStatus::InvalidStorage);
    if (!loadOrder(order)) return status(AtomicStatus::InvalidOrder);
    if (kind > static_cast<std::uint32_t>(AtomicKind::Boolean)) return status(AtomicStatus::UnsupportedType);
    return dispatch(kind, [&]<class T>() -> std::int32_t {
        if (!aligned<T>(storage)) return status(AtomicStatus::InvalidStorage);
        std::atomic_ref<T> cell(*static_cast<T*>(storage));
        const T initial = decode<T>(expected);
        const auto start = std::chrono::steady_clock::now();
        for (;;)
        {
            const T current = cell.load(memoryOrder(order));
            if (current != initial)
            {
                *observed = encode(current);
                *changed = 1;
                return status(AtomicStatus::Ok);
            }
            if (finite)
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (elapsed >= 0 && static_cast<std::uint64_t>(elapsed) >= timeout_ns)
                {
                    *observed = encode(current);
                    return status(AtomicStatus::Ok);
                }
                std::this_thread::yield();
            }
            else cell.wait(initial, memoryOrder(order));
        }
    });
}

extern "C" std::uint32_t aevix_atomic_alignment(std::uint32_t kind) noexcept
{
    return dispatch(kind, []<class T>() { return static_cast<std::uint32_t>(std::atomic_ref<T>::required_alignment); });
}

extern "C" std::uint32_t aevix_atomic_lock_free(std::uint32_t kind) noexcept
{
    return dispatch(kind, []<class T>() { return std::atomic_ref<T>::is_always_lock_free ? 1u : 0u; });
}
