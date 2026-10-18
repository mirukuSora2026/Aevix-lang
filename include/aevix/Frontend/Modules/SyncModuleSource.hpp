// ========================
//
// Created By : miruku_sora2026
//
// File name : SyncModuleSource.hpp
//
// Description :
//     인라인 CPU 잠금과 선형 보호자의 신뢰된 공개 선언을 제공한다.
//     Provide trusted public declarations for inline CPU locks and linear guards.
//
// ========================
#pragma once
#include <string_view>

namespace aevix
{
    inline constexpr std::string_view SyncModuleSource = R"AEVIXDOC(
/*
 * Inline CPU synchronization. Locks are nonrecursive and promise neither fairness nor starvation prevention.
 * @module sync
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public import time { Duration };
import error { Error };

/*
 * Advisory poison policy. This profile has no unwinding; explicit guard marks apply when the guard is released.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum PoisonPolicy implements Copy {
/*
 * Retain the chosen unwind policy; no automatic poison is generated before unwinding exists.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
PoisonOnUnwind;
/*
 * Use explicit advisory marks only, independently of future unwinding support.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Manual;
};

/*
 * A recoverable owned value extracted from poisoned storage; recovery never clears the original poison flag.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct Poisoned<T> implements Error {
/*
 * The retained value, including an exclusive borrow or linear guard when T is such a capability.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public T value;
/*
 * Consume this carrier and recover the retained value without cloning it.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void into_inner(Poisoned<T> error) {}
/*
 * Inspect the poison failure without consuming its retained value.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public string_view code(&Self self) { return "sync.poisoned"; }
/*
 * Summarize advisory poison; this does not validate or repair protected data.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public string_view message(&Self self) { return "The value was extracted from poisoned storage."; }
};

/*
 * Acquisition failure. Poisoned owns an already acquired guard; other variants own no guard.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum LockError<G> implements Error {
/*
 * The retained acquired capability. Dropping this variant releases that guard normally.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Poisoned {
/*
 * The already acquired guard or exclusive data borrow; consuming recovery retains the same capability.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public G guard;
};
/*
 * The reader count reached its finite representable limit; no guard was acquired.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
ReaderOverflow;
/*
 * The requested duration is outside the supported finite timeout domain; no guard was acquired.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
InvalidTimeout;
/*
 * Inspect the failure without consuming a possibly acquired guard.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public string_view code(&Self self) {
    switch (self) {
        case LockError<G>.Poisoned: return "sync.poisoned"; break;
        case LockError<G>.ReaderOverflow: return "sync.reader_overflow"; break;
        case LockError<G>.InvalidTimeout: return "sync.invalid_timeout"; break;
    }
}
/*
 * Summarize the failure while retaining any acquired guard.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public string_view message(&Self self) {
    switch (self) {
        case LockError<G>.Poisoned: return "The acquired guard protects advisory poisoned storage."; break;
        case LockError<G>.ReaderOverflow: return "The reader count reached its representable limit."; break;
        case LockError<G>.InvalidTimeout: return "The timeout exceeds INT64_MAX nanoseconds."; break;
    }
}
};

/*
 * A linear exclusive mutex capability. It is neither Copy, Send nor Sync; derived borrows cannot outlive it.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct MutexGuard<T> {
T payload;
/*
 * Borrow the protected value for no longer than this guard and its original lock owner remain alive.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void as_ref(&MutexGuard<T> guard) {}
/*
 * Exclusively reborrow the protected value; no other guard-derived borrow may overlap it.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void as_mut(&mut MutexGuard<T> guard) {}
};

/*
 * A linear shared read capability. It is neither Copy, Send nor Sync; releasing it removes exactly one reader.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct RwLockReadGuard<T> {
T payload;
/*
 * Borrow the protected value for no longer than this guard and its original lock owner remain alive.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void as_ref(&RwLockReadGuard<T> guard) {}
};

/*
 * A linear exclusive writer capability. It is neither Copy, Send nor Sync; derived borrows cannot outlive it.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct RwLockWriteGuard<T> {
T payload;
/*
 * Borrow the protected value for no longer than this guard and its original lock owner remain alive.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void as_ref(&RwLockWriteGuard<T> guard) {}
/*
 * Exclusively reborrow the protected value; no other guard-derived borrow may overlap it.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void as_mut(&mut RwLockWriteGuard<T> guard) {}
};

/*
 * An inline owner of initialized T and mutex state. Construction has no per-lock allocation; moving requires all waiters and guards to have ended.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct Mutex<T> {
T payload;
/*
 * Construct an inline mutex with PoisonOnUnwind metadata without allocating or cloning T.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void new(T value) {}
/*
 * Construct an inline mutex retaining the explicit policy without allocating or cloning T.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void with_policy(T value, PoisonPolicy policy) {}
/*
 * Block for exclusive access. A poison error retains the acquired MutexGuard<T>.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void lock(&Mutex<T> lock) {}
/*
 * Attempt acquisition once. Ok(None) means busy; a poison error retains its acquired guard.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void try_lock(&Mutex<T> lock) {}
/*
 * Wait for a finite duration. Ok(None) means timeout; a zero duration attempts once.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void lock_timeout(&Mutex<T> lock, Duration timeout) {}
/*
 * Snapshot the published advisory poison flag without accessing protected T.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void is_poisoned(&Mutex<T> lock) {}
/*
 * Read the retained policy without acquiring protected T.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void poison_policy(&Mutex<T> lock) {}
/*
 * Bypass acquisition under a genuine exclusive owner borrow, retaining the data borrow on poison.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void get_mut(&mut Mutex<T> lock) {}
/*
 * Consume an unborrowed owner and return T; poison returns Poisoned<T> retaining the same value.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void into_inner(Mutex<T> lock) {}
/*
 * Read the value while retaining both this guard and its original owner loan.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void data(&MutexGuard<T> guard) {}
/*
 * Exclusively reborrow the value while retaining this guard and its owner loan.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void data_mut(&mut MutexGuard<T> guard) {}
/*
 * Consume and release exactly one exclusive guard after all derived loans end.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void unlock(MutexGuard<T> guard) {}
/*
 * Schedule an explicit advisory poison mark for this guard release; it does not repair or unlock.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void mark_poisoned(&mut MutexGuard<T> guard) {}
/*
 * Clear published and pending poison while exclusively held; application repair remains the caller responsibility.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void clear_poison(&mut MutexGuard<T> guard) {}
};

/*
 * An inline owner with multiple-reader or exclusive-writer access. No implicit read-to-write upgrade is provided.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct RwLock<T> {
T payload;
/*
 * Construct inline reader/writer state and T with PoisonOnUnwind metadata, without per-lock allocation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void new(T value) {}
/*
 * Construct inline reader/writer state retaining the selected advisory policy.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void with_policy(T value, PoisonPolicy policy) {}
/*
 * Acquire one reader. Poison errors retain the acquired reader; saturated counts return ReaderOverflow.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void read(&RwLock<T> lock) {}
/*
 * Acquire the exclusive writer; poison errors retain the acquired writer.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void write(&RwLock<T> lock) {}
/*
 * Attempt one read acquisition; Ok(None) means unavailable.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void try_read(&RwLock<T> lock) {}
/*
 * Attempt one write acquisition; Ok(None) means unavailable.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void try_write(&RwLock<T> lock) {}
/*
 * Try for a finite requested duration; Ok(None) means timeout without any retained reader.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void read_timeout(&RwLock<T> lock, Duration timeout) {}
/*
 * Try for a finite requested duration; Ok(None) means timeout without a retained writer.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void write_timeout(&RwLock<T> lock, Duration timeout) {}
/*
 * Snapshot advisory poison without acquiring or validating protected T.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void is_poisoned(&RwLock<T> lock) {}
/*
 * Read the retained policy without acquiring data access.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void poison_policy(&RwLock<T> lock) {}
/*
 * Obtain an exclusive data borrow under genuine unique owner access; poison retains that borrow.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void get_mut(&mut RwLock<T> lock) {}
/*
 * Consume an unborrowed owner; a poison error retains the extracted T in Poisoned<T>.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void into_inner(RwLock<T> lock) {}
/*
 * Read protected T while this reader capability and its owner loan remain live.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void read_data(&RwLockReadGuard<T> guard) {}
/*
 * Read protected T through a held exclusive writer.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void write_data(&RwLockWriteGuard<T> guard) {}
/*
 * Exclusively reborrow T through the held writer; the borrow cannot outlive that guard.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void write_data_mut(&mut RwLockWriteGuard<T> guard) {}
/*
 * Consume and release precisely one reader capability.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void unlock_read(RwLockReadGuard<T> guard) {}
/*
 * Consume and release precisely one writer capability, applying any pending explicit poison mark.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void unlock_write(RwLockWriteGuard<T> guard) {}
/*
 * Schedule an advisory poison mark for writer release; read guards cannot mark poison.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void mark_poisoned(&mut RwLockWriteGuard<T> guard) {}
/*
 * Clear native and pending poison while exclusively held after explicit application repair.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void clear_poison(&mut RwLockWriteGuard<T> guard) {}
};
)AEVIXDOC";
}
