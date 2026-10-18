// ========================
//
// Created By : miruku_sora2026
//
// File name : AtomicModuleSource.hpp
//
// Description :
//     신뢰된 CPU 원자 연산 모듈의 공개 선언과 수명 정보를 제공한다.
//     Provide public declarations and lifecycle metadata for the trusted CPU atomic module.
//
// ========================
#pragma once
#include <string_view>

namespace aevix
{
    inline constexpr std::string_view AtomicModuleSource = R"AEVIXDOC(

/*
 * CPU atomic protocol; operations are explicitly ordered and use inline storage.
 * @module atomic
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public import time { Duration };
import error { Error };

/*
 * The five explicit CPU ordering modes; operation-specific validation is required.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum MemoryOrder implements Copy {

/*
 * The Relaxed memory ordering mode.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Relaxed;

/*
 * The Acquire memory ordering mode.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Acquire;

/*
 * The Release memory ordering mode.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Release;

/*
 * The AcqRel memory ordering mode.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
AcqRel;

/*
 * The SeqCst memory ordering mode.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
SeqCst;
};

/*
 * Recoverable order, capability, or arithmetic failures; storage invariants remain internal.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum AtomicError implements Copy, Error {

/*
 * The InvalidOrder atomic failure.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
InvalidOrder;

/*
 * The UnsupportedType atomic failure.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
UnsupportedType;

/*
 * The Overflow atomic failure.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Overflow;
/*
 * Return a stable static error code while borrowing the error value.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public string_view code(&AtomicError self) {
    switch (*self) {
        case AtomicError.InvalidOrder: return "atomic.invalid_order"; break;
        case AtomicError.UnsupportedType: return "atomic.unsupported_type"; break;
        case AtomicError.Overflow: return "atomic.overflow"; break;
    }
}
/*
 * Return a static English explanation without allocating or moving the error.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public string_view message(&AtomicError self) {
    switch (*self) {
        case AtomicError.InvalidOrder: return "The memory order is invalid for this atomic operation."; break;
        case AtomicError.UnsupportedType: return "The atomic scalar type is not supported by the CPU profile."; break;
        case AtomicError.Overflow: return "The checked atomic arithmetic would overflow; this attempt performed no write."; break;
    }
}
};

/*
 * A compare-exchange observation; mismatch and weak spurious failure set changed to false.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct CasOutcome<T> implements Copy where T implements Copy {

/*
 * Whether this compare-exchange changed the cell.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public bool changed;

/*
 * The value observed by compare-exchange, including a mismatch.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public T observed;
};

/*
 * A finite CPU wait observation; notifications alone do not publish data.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct WaitOutcome<T> implements Copy where T implements Copy {

/*
 * The value observed at completion.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public T observed;

/*
 * Whether a value different from the expected value was observed.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public bool changed;

/*
 * Whether the timeout elapsed before a different value was observed.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public bool timed_out;
};

/*
 * A non-Copy inline CPU cell; ordinary non-atomic accesses require exclusive ownership.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct Atomic<T> {
T value;

/*
 * Construct an inline cell without heap allocation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void new(T value) {}

/*
 * Load with Relaxed, Acquire, or SeqCst ordering.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void load(&Atomic<T> cell, MemoryOrder order) {}

/*
 * Store with Relaxed, Release, or SeqCst ordering.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void store(&Atomic<T> cell, T value, MemoryOrder order) {}

/*
 * Replace atomically and return the previous value.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void swap(&Atomic<T> cell, T value, MemoryOrder order) {}

/*
 * Compare and conditionally exchange; mismatch is a successful observation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void compare_exchange(&Atomic<T> cell, T expected, T desired, MemoryOrder success, MemoryOrder failure) {}

/*
 * Weak compare-exchange may fail spuriously and requires a retry protocol.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void compare_exchange_weak(&Atomic<T> cell, T expected, T desired, MemoryOrder success, MemoryOrder failure) {}

/*
 * Perform fetch_add and return the previous value; arithmetic policy is explicit.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void fetch_add(&Atomic<T> cell, T value, MemoryOrder order) {}

/*
 * Perform fetch_sub and return the previous value; arithmetic policy is explicit.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void fetch_sub(&Atomic<T> cell, T value, MemoryOrder order) {}

/*
 * Perform fetch_and and return the previous value; arithmetic policy is explicit.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void fetch_and(&Atomic<T> cell, T value, MemoryOrder order) {}

/*
 * Perform fetch_or and return the previous value; arithmetic policy is explicit.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void fetch_or(&Atomic<T> cell, T value, MemoryOrder order) {}

/*
 * Perform fetch_xor and return the previous value; arithmetic policy is explicit.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void fetch_xor(&Atomic<T> cell, T value, MemoryOrder order) {}

/*
 * Perform fetch_nand and return the previous value; arithmetic policy is explicit.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void fetch_nand(&Atomic<T> cell, T value, MemoryOrder order) {}

/*
 * Perform fetch_min and return the previous value; arithmetic policy is explicit.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void fetch_min(&Atomic<T> cell, T value, MemoryOrder order) {}

/*
 * Perform fetch_max and return the previous value; arithmetic policy is explicit.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void fetch_max(&Atomic<T> cell, T value, MemoryOrder order) {}

/*
 * Perform checked_fetch_add and return the previous value; arithmetic policy is explicit.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void checked_fetch_add(&Atomic<T> cell, T value, MemoryOrder order) {}

/*
 * Perform checked_fetch_sub and return the previous value; arithmetic policy is explicit.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void checked_fetch_sub(&Atomic<T> cell, T value, MemoryOrder order) {}

/*
 * Perform saturating_fetch_add and return the previous value; arithmetic policy is explicit.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void saturating_fetch_add(&Atomic<T> cell, T value, MemoryOrder order) {}

/*
 * Perform saturating_fetch_sub and return the previous value; arithmetic policy is explicit.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void saturating_fetch_sub(&Atomic<T> cell, T value, MemoryOrder order) {}

/*
 * Borrow non-atomic scalar storage only under exclusive cell access.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void get_mut(&mut Atomic<T> cell) {}

/*
 * Consume the cell and return its value after all loans end.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void into_inner(Atomic<T> cell) {}

/*
 * Query support for the explicitly selected <T> on the current CPU profile.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void is_supported() {}

/*
 * Query conservative native lock-free support for the explicitly selected <T>.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void is_lock_free() {}

/*
 * Query required native alignment for the explicitly selected <T>.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void required_alignment() {}

/*
 * Issue an interthread CPU fence; Relaxed is invalid.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void fence(MemoryOrder order) {}

/*
 * Issue a compiler fence; this does not replace an interthread hardware fence.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void compiler_fence(MemoryOrder order) {}

/*
 * Wait until the expected value differs; normal ABA limitations apply.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void wait(&Atomic<T> cell, T expected, MemoryOrder order) {}

/*
 * Poll until a different value or Duration timeout; report the observation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void wait_timeout(&Atomic<T> cell, T expected, Duration timeout, MemoryOrder order) {}

/*
 * Notify one eligible waiter without independently publishing state.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void notify_one(&Atomic<T> cell) {}

/*
 * Notify eligible waiters without promising fairness or completion.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void notify_all(&Atomic<T> cell) {}
};
)AEVIXDOC";
}
