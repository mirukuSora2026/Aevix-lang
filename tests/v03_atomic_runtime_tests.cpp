// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_atomic_runtime_tests.cpp
//
// Description :
//     원자 정수 경계·잘못된 순서·경합·대기와 발행 가시성을 검사한다.
//     Check atomic integer boundaries, invalid orders, contention, waiting, and publication visibility.
//
// ========================

#include "aevix/Runtime/AtomicRuntime.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>
#include <type_traits>

namespace
{
    using namespace aevix::runtime;
    std::uint64_t checks = 0;

    void require(bool condition, const char* message)
    {
        ++checks;
        if (!condition) { std::cerr << message << '\n'; std::exit(1); }
    }

    struct Outcome
    {
        std::int32_t status;
        std::uint64_t old;
        std::uint32_t changed;
    };

    Outcome call(void* cell, AtomicKind kind, AtomicOperation operation,
        std::uint64_t operand = 0, std::uint64_t expected = 0,
        AtomicOrder success = AtomicOrder::SeqCst, AtomicOrder failure = AtomicOrder::Relaxed)
    {
        Outcome out{0, 999, 999};
        out.status = aevix_atomic_operation(cell, static_cast<std::uint32_t>(kind),
            static_cast<std::uint32_t>(operation), operand, expected,
            static_cast<std::uint32_t>(success), static_cast<std::uint32_t>(failure), &out.old, &out.changed);
        return out;
    }

    template<class T> std::uint64_t bits(T value)
    {
        if constexpr (std::is_same_v<T, bool>) return value ? 1 : 0;
        else return static_cast<std::make_unsigned_t<T>>(value);
    }

    template<class T> void width(AtomicKind kind)
    {
        alignas(std::atomic_ref<T>::required_alignment) T cell = std::numeric_limits<T>::max();
        require(aevix_atomic_alignment(static_cast<std::uint32_t>(kind)) == std::atomic_ref<T>::required_alignment, "wrong atomic alignment");
        require(aevix_atomic_lock_free(static_cast<std::uint32_t>(kind)) == std::atomic_ref<T>::is_always_lock_free, "wrong lock-free capability");
        require(call(&cell, kind, AtomicOperation::Load).old == bits(cell), "wrong atomic load width");
        const auto maximum = cell;
        Outcome checked = call(&cell, kind, AtomicOperation::CheckedFetchAdd, 1);
        require(checked.status == 3 && cell == maximum && checked.old == 0, "overflow changed the cell or exposed a success value");
        Outcome saturated = call(&cell, kind, AtomicOperation::SaturatingFetchAdd, 1);
        require(saturated.status == 0 && saturated.old == bits(maximum) && cell == maximum, "maximum saturation failed");
        Outcome wrapped = call(&cell, kind, AtomicOperation::FetchAdd, 1);
        require(wrapped.status == 0 && wrapped.old == bits(maximum) && cell == std::numeric_limits<T>::min(), "modular addition failed");
        const T minimum = cell;
        Outcome underflow = call(&cell, kind, AtomicOperation::CheckedFetchSub, 1);
        require(underflow.status == 3 && cell == minimum, "underflow performed a write");
        Outcome sat_sub = call(&cell, kind, AtomicOperation::SaturatingFetchSub, 1);
        require(sat_sub.status == 0 && sat_sub.old == bits(minimum) && cell == minimum, "minimum saturation failed");
        Outcome exchange = call(&cell, kind, AtomicOperation::CompareExchange, 7, bits(minimum));
        require(exchange.status == 0 && exchange.old == bits(minimum) && exchange.changed == 1 && cell == 7, "successful CAS lost the old value");
        Outcome mismatch = call(&cell, kind, AtomicOperation::CompareExchange, 9, 6);
        require(mismatch.status == 0 && mismatch.old == 7 && mismatch.changed == 0 && cell == 7, "CAS mismatch must be an unchanged successful outcome");
        require(call(&cell, kind, AtomicOperation::FetchMin, 3).old == 7 && cell == 3, "atomic minimum failed");
        require(call(&cell, kind, AtomicOperation::FetchMax, 8).old == 3 && cell == 8, "atomic maximum failed");
    }

    void exhaustiveSmallIntegers()
    {
        constexpr std::array operations{AtomicOperation::FetchAdd, AtomicOperation::FetchSub,
            AtomicOperation::CheckedFetchAdd, AtomicOperation::CheckedFetchSub,
            AtomicOperation::SaturatingFetchAdd, AtomicOperation::SaturatingFetchSub,
            AtomicOperation::FetchMin, AtomicOperation::FetchMax};
        for (bool signed_kind : {false, true})
        {
            for (int a = 0; a < 256; ++a) for (int b = 0; b < 256; ++b)
            {
                const int left = signed_kind && a >= 128 ? a - 256 : a;
                const int right = signed_kind && b >= 128 ? b - 256 : b;
                const int lo = signed_kind ? -128 : 0, hi = signed_kind ? 127 : 255;
                for (AtomicOperation operation : operations)
                {
                    const bool addition = operation == AtomicOperation::FetchAdd || operation == AtomicOperation::CheckedFetchAdd
                        || operation == AtomicOperation::SaturatingFetchAdd;
                    const bool subtraction = operation == AtomicOperation::FetchSub || operation == AtomicOperation::CheckedFetchSub
                        || operation == AtomicOperation::SaturatingFetchSub;
                    int result = addition ? left + right : subtraction ? left - right
                        : operation == AtomicOperation::FetchMin ? (left < right ? left : right) : (left > right ? left : right);
                    bool overflow = result < lo || result > hi;
                    const bool checked = operation == AtomicOperation::CheckedFetchAdd || operation == AtomicOperation::CheckedFetchSub;
                    const bool saturating = operation == AtomicOperation::SaturatingFetchAdd || operation == AtomicOperation::SaturatingFetchSub;
                    if (saturating) result = result < lo ? lo : result > hi ? hi : result;
                    std::uint8_t unsigned_cell = static_cast<std::uint8_t>(a);
                    std::int8_t signed_cell = std::bit_cast<std::int8_t>(unsigned_cell);
                    Outcome out = signed_kind ? call(&signed_cell, AtomicKind::I8, operation, b)
                        : call(&unsigned_cell, AtomicKind::U8, operation, b);
                    const std::uint64_t after = signed_kind ? bits(signed_cell) : bits(unsigned_cell);
                    if (checked && overflow)
                        require(out.status == 3 && out.old == 0 && after == static_cast<std::uint64_t>(a), "exhaustive rejected update changed a cell");
                    else require(out.status == 0 && out.old == static_cast<std::uint64_t>(a)
                        && after == static_cast<std::uint8_t>(result), "exhaustive atomic arithmetic mismatch");
                }
            }
        }
    }

    void ordersAndBoolean()
    {
        std::uint32_t cell = 11;
        for (std::uint32_t order = 0; order < 8; ++order)
        {
            const auto value = static_cast<AtomicOrder>(order);
            const bool valid_load = order == 0 || order == 1 || order == 4;
            const bool valid_store = order == 0 || order == 2 || order == 4;
            auto load = call(&cell, AtomicKind::U32, AtomicOperation::Load, 0, 0, value);
            require(load.status == (valid_load ? 0 : 1) && cell == 11, "load order validation failed");
            auto store = call(&cell, AtomicKind::U32, AtomicOperation::Store, 11, 0, value);
            require(store.status == (valid_store ? 0 : 1) && cell == 11, "store order validation failed");
            require(aevix_atomic_fence(order, 0) == (order > 0 && order <= 4 ? 0 : 1), "thread fence order validation failed");
            require(aevix_atomic_fence(order, 1) == (order > 0 && order <= 4 ? 0 : 1), "compiler fence order validation failed");
            for (std::uint32_t failure = 0; failure < 8; ++failure)
            {
                const bool valid = order <= 4 && (failure == 0 || failure == 1 || failure == 4);
                auto cas = call(&cell, AtomicKind::U32, AtomicOperation::CompareExchange, 11, 11,
                    value, static_cast<AtomicOrder>(failure));
                require(cas.status == (valid ? 0 : 1) && cell == 11, "CAS order validation failed");
            }
        }
        bool flag = false;
        require(call(&flag, AtomicKind::Boolean, AtomicOperation::Swap, 1).old == 0 && flag, "boolean exchange failed");
        require(call(&flag, AtomicKind::Boolean, AtomicOperation::FetchXor, 1).old == 1 && !flag, "boolean xor failed");
        require(call(&flag, AtomicKind::Boolean, AtomicOperation::FetchNand, 1).old == 0 && flag, "boolean nand failed");
        require(call(&flag, AtomicKind::Boolean, AtomicOperation::FetchAdd, 1).status == 2 && flag, "boolean arithmetic must fail without a write");
        alignas(8) std::array<unsigned char, 16> buffer{};
        require(call(buffer.data() + 1, AtomicKind::U64, AtomicOperation::Load).status == 4, "misaligned storage was accepted");
        require(call(nullptr, AtomicKind::U64, AtomicOperation::Load).status == 4, "null storage was accepted");
        require(aevix_atomic_alignment(999) == 0 && aevix_atomic_lock_free(999) == 0, "unsupported width reported native support");
    }

    void concurrentAndWait()
    {
        alignas(std::atomic_ref<std::uint64_t>::required_alignment) std::uint64_t counter = 0;
        std::array<std::thread, 8> workers;
        std::atomic<bool> failed = false;
        for (auto& worker : workers) worker = std::thread([&] {
            for (int i = 0; i < 10000; ++i)
            {
                std::uint64_t old = 0; std::uint32_t changed = 0;
                if (aevix_atomic_operation(&counter, 7, 5, 1, 0, 0, 0, &old, &changed) != 0)
                    failed.store(true, std::memory_order_relaxed);
            }
        });
        for (auto& worker : workers) worker.join();
        require(!failed && counter == 80000, "concurrent updates lost values");

        std::uint32_t ready = 0;
        int published = 0;
        std::thread producer([&] {
            published = 42;
            std::uint64_t old = 0; std::uint32_t changed = 0;
            aevix_atomic_operation(&ready, 5, 1, 1, 0, 2, 0, &old, &changed);
            aevix_atomic_operation(&ready, 5, 18, 0, 0, 0, 0, &old, &changed);
        });
        std::uint64_t observed = 0; std::uint32_t changed = 0;
        const auto waited = aevix_atomic_wait(&ready, 5, 0, 1, 0, 0, &observed, &changed);
        require(waited == 0 && observed == 1 && changed == 1 && published == 42, "acquire wait failed to publish prior ordinary writes");
        producer.join();
        require(aevix_atomic_wait(&ready, 5, 1, 1, 0, 1, &observed, &changed) == 0
            && observed == 1 && changed == 0, "zero timeout is not an unchanged timeout outcome");
        require(aevix_atomic_wait(&ready, 5, 0, 1, 0, 1, &observed, &changed) == 0
            && observed == 1 && changed == 1, "already changed value must precede timeout");
        require(aevix_atomic_wait(&ready, 5, 0, 2, 0, 1, &observed, &changed) == 1
            && observed == 0 && changed == 0, "invalid wait order did not zero outputs");
    }
}

int main()
{
    width<std::int8_t>(AtomicKind::I8); width<std::uint8_t>(AtomicKind::U8);
    width<std::int16_t>(AtomicKind::I16); width<std::uint16_t>(AtomicKind::U16);
    width<std::int32_t>(AtomicKind::I32); width<std::uint32_t>(AtomicKind::U32);
    width<std::int64_t>(AtomicKind::I64); width<std::uint64_t>(AtomicKind::U64);
    exhaustiveSmallIntegers(); ordersAndBoolean(); concurrentAndWait();
    std::cout << checks << " atomic runtime checks passed.\n";
}
