// ========================
//
// Created By : miruku_sora2026
//
// File name : TaskModuleSource.hpp
//
// Description :
//     소유 작업·스코프와 독립 취소 역할의 신뢰된 공개 선언을 제공한다.
//     Provide trusted public declarations for owned Task/Scope and independent cancellation roles.
//
// ========================
#pragma once
#include <string_view>

namespace aevix
{
    inline constexpr std::string_view TaskModuleSource = R"AEVIXDOC(
/*
 * Structured owned Task/Scope execution and independent cooperative cancellation.
 * A task observation borrows its scope; a cancellation observer alone implies neither work ownership nor completion.
 * @module task
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public import error { TaskError };
public import time { Duration };
import error { Error, TimeError };
import time { monotonic_ns };

/*
 * Checked monotonic deadline construction failure; no suspended timer or wall-clock fallback is involved.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum TimerError implements Copy, Error {
/*
 * Adding the requested duration would overflow the monotonic nanosecond representation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Overflow;
/*
 * The real monotonic clock failed; its original typed reason is preserved.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Clock {
/*
 * The exact time-module failure returned by the native clock boundary.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public TimeError reason;
};
/*
 * Return a static allocation-free error code.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public string_view code(&Self self) {
    switch (*self) {
        case Self.Overflow: return "task.timer_overflow"; break;
        case Self.Clock: return "task.timer_clock_failure"; break;
    }
}
/*
 * Return an allocation-free explanation, preserving the clock's original message.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public string_view message(&Self self) {
    switch (*self) {
        case Self.Overflow: return "The monotonic deadline exceeds the supported nanosecond range."; break;
        case Self.Clock { TimeError reason = reason }: return reason.message(); break;
    }
}
};

/*
 * A checked instant in the same monotonic clock domain as time.monotonic_ns, not a timer registration.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct Deadline implements Copy {
    u64 instant_nanos;
/*
 * Read the real monotonic clock and checked-add a duration without allocating or scheduling work.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<Deadline,TimerError> after(Duration duration) {
    switch (monotonic_ns()) {
        case Result<u64,TimeError>.Ok { u64 current = value }:
            u64 amount = duration.as_nanos();
            if (current > 18446744073709551615u64 - amount) return Result.err(TimerError.Overflow);
            return Result.ok(Deadline { instant_nanos = current + amount });
            break;
        case Result<u64,TimeError>.Err { TimeError reason = error }:
            return Result.err(TimerError.Clock { reason = reason });
            break;
    }
}
/*
 * Read the checked instant for comparisons in this monotonic clock domain; this is not a wall-clock timestamp.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public u64 monotonic_nanos(&Self self) { return self.instant_nanos; }
};

/*
 * Recoverable notification-state allocation or owner-count exhaustion; failed operations preserve input owners.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum CancellationError implements Copy, Error {
/*
 * Allocating notification state or owner metadata failed.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
AllocationFailure;
/*
 * The checked owner count or metadata identity space is exhausted.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Overflow;
/*
 * Return a static error code.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public string_view code(&Self self) {
    switch (*self) {
        case Self.AllocationFailure: return "task.cancellation_allocation_failure"; break;
        case Self.Overflow: return "task.cancellation_overflow"; break;
    }
}
/*
 * Return a static English explanation without allocation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public string_view message(&Self self) {
    switch (*self) {
        case Self.AllocationFailure: return "Allocating cancellation state or owner metadata failed."; break;
        case Self.Overflow: return "The cancellation owner count or metadata identity space is exhausted."; break;
    }
}
};

/*
 * Whether an idempotent request changed the shared notification flag; neither outcome means work has stopped.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum CancelRequest implements Copy {
/*
 * This call changed the request flag from false to true.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
NewlyRequested;
/*
 * A previous call already requested cancellation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
AlreadyRequested;
};

/*
 * A linear request-authority owner; private native identity cannot be constructed or converted by source.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct CancellationSource {
    u64 state_id;
/*
 * Create independent unrequested state; may allocate.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<CancellationSource, CancellationError> new() {}
/*
 * Explicitly retain request authority; allocation or count failure preserves source.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<CancellationSource, CancellationError> clone(&CancellationSource source) {}
/*
 * Create an owned readonly observer that may outlive this source; may allocate.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<CancellationToken, CancellationError> token(&CancellationSource source) {}
/*
 * Idempotently request cancellation without completing or rolling back work.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<CancelRequest, CancellationError> request(&CancellationSource source) {}
};

/*
 * A linear readonly notification owner with independent state lifetime.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct CancellationToken {
    u64 state_id;
/*
 * Retain readonly observation; this never grants request authority.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<CancellationToken, CancellationError> clone(&CancellationToken token) {}
/*
 * Read a synchronized request snapshot; false is not a completion guarantee.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public bool requested(&CancellationToken token) {}
};

/*
 * A cooperative checkpoint error; it neither completes work nor rolls back effects.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum Cancelled implements Copy, Error {
/*
 * The observed notification state has received a cancellation request.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Requested;
/*
 * Return the stable checkpoint error code without allocation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public string_view code(&Self self) { return "task.cancelled"; }
/*
 * Return a static explanation; a request alone does not prove completion.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public string_view message(&Self self) { return "Cooperative cancellation was requested."; }
};

/*
 * An execution-backend fact, never a task identity, native handle, or capability.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum ExecutorId implements Copy {
/*
 * The selected CPU domain uses native threads for spawned work; Task.ready publishes precomputed values without a worker.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
NativeThreads;
};

/*
 * An explicit descriptor for native-thread CPU execution and scope-tracked precomputed values.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct Executor implements Copy {
    u8 backend;
/*
 * Select the implemented native-thread backend without allocating a worker or scope.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Executor native_threads() { return Executor { backend = 0u8 }; }
};

/*
 * Default-only scope options. Unsupported scheduling policies are not silently accepted.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct ScopeOptions implements Copy {
    u8 profile;
/*
 * Construct the implemented default scope policy without allocating.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public ScopeOptions defaults() { return ScopeOptions { profile = 0u8 }; }
};

/*
 * A linear structured work owner. Drop drains accepted children; it is not transferable to workers.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct TaskScope { u64 state_id; };

/*
 * Allocation, representation or native worker-creation failure before admission succeeds; unstarted captures are cleaned.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum SpawnError implements Copy, Error {
/*
 * Allocating scope, capture, result or worker metadata failed.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
AllocationFailure;
/*
 * A checked storage or identity bound was exceeded.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Overflow;
/*
 * The scope no longer admits new children.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
ScopeClosed;
/*
 * Creating the native worker failed before work could run.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
ThreadFailure {
/*
 * The native thread-creation error code.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public i32 os_code;
/*
 * The platform error domain accompanying os_code.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public i32 domain;
};
/*
 * Borrow this failure and return a static code.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public string_view code(&Self self) {
    switch (*self) {
        case Self.AllocationFailure: return "task.spawn_allocation_failure"; break;
        case Self.Overflow: return "task.spawn_overflow"; break;
        case Self.ScopeClosed: return "task.spawn_scope_closed"; break;
        case Self.ThreadFailure: return "task.spawn_thread_failure"; break;
    }
}
/*
 * Borrow this failure and return a static English explanation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public string_view message(&Self self) {
    switch (*self) {
        case Self.AllocationFailure: return "Allocating task or scope storage failed."; break;
        case Self.Overflow: return "The requested task storage or identity space exceeds its bound."; break;
        case Self.ScopeClosed: return "The scope no longer accepts new work."; break;
        case Self.ThreadFailure: return "The operating system could not create the worker thread."; break;
    }
}
};

/*
 * A non-consuming scope failure or an owned closed scope retained for safe retry; owned retry errors still drain or safely stop.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum ScopeError implements Error {
/*
 * Retain the actual closed scope and its native failure reason; work is never forgotten.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Retry {
/*
 * The closed scope, including every remaining child and lifetime obligation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public TaskScope scope;
/*
 * The precise lifecycle failure, including native code and domain when present.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public TaskError reason;
};
/*
 * A failure from a borrowed scope operation; the caller keeps the scope owner.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Failure {
/*
 * The precise lifecycle failure; this variant contains no scope ownership.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public TaskError reason;
};
/*
 * Inspect the retained lifecycle reason without consuming the scope.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public string_view code(&Self self) {
    switch (*self) {
        case Self.Retry { &TaskError failure = reason }: return failure.code(); break;
        case Self.Failure { &TaskError failure = reason }: return failure.code(); break;
    }
}
/*
 * Inspect the retained lifecycle reason without consuming the scope.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public string_view message(&Self self) {
    switch (*self) {
        case Self.Retry { &TaskError failure = reason }: return failure.message(); break;
        case Self.Failure { &TaskError failure = reason }: return failure.message(); break;
    }
}
};

/*
 * Admission state of a live scope; successful close consumes the owner rather than returning a Drained snapshot.
 * Zero active children does not prove that reaping, result cleanup, or scope-loan use has ended.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum ScopeStatus implements Copy {
/*
 * The scope currently permits new accepted preparations.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Open;
/*
 * New admission has ended; accepted work and cleanup obligations may remain.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Closing;
};

/*
 * A synchronized phase snapshot; it never grants ownership of the result.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum TaskPhase implements Copy {
/*
 * The Prepared observation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Prepared;
/*
 * The Queued observation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Queued;
/*
 * The Running observation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Running;
/*
 * The Completed observation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Completed;
/*
 * The Cancelled observation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Cancelled;
/*
 * The Failed observation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Failed;
/*
 * The Aborted observation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Aborted;
/*
 * The Consumed observation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Consumed;
};

/*
 * Finite readiness observation; timeout neither cancels nor releases captured resources.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum WaitStatus implements Copy {
/*
 * The Ready observation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
Ready;
/*
 * The TimedOut observation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
TimedOut;
};

/*
 * Task-context cancellation disposition; an already terminal task remains unchanged.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public enum TaskCancelRequest implements Copy {
/*
 * The NewlyRequested observation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
NewlyRequested;
/*
 * The AlreadyRequested observation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
AlreadyRequested;
/*
 * The AlreadyTerminal observation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
AlreadyTerminal;
};

/*
 * A snapshot of phase and the request flag; it conveys neither a result borrow nor completion ownership.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct TaskStatus implements Copy {
/*
 * The current language-work phase, or Consumed for an already collected handle.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public TaskPhase phase;
/*
 * Whether cooperative cancellation was requested; work may still complete successfully.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public bool cancel_requested;
};

/*
 * Final accounting after every admitted child and unclaimed result is drained.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct ScopeReport implements Copy {
/*
 * The number of accepted child observations or outcomes.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public u64 accepted;
/*
 * The number of observed child observations or outcomes.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public u64 observed;
/*
 * The number of discarded child observations or outcomes.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public u64 discarded;
/*
 * The number of completed child observations or outcomes.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public u64 completed;
/*
 * The number of cancelled child observations or outcomes.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public u64 cancelled;
/*
 * The number of failed child observations or outcomes.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public u64 failed;
/*
 * The number of aborted child observations or outcomes.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public u64 aborted;
};

/*
 * A linear result observation tied to a live TaskScope; discarding it does not detach work.
 * Owned captures/results only in this implementation profile; worker effects require compiler proof.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct Task<T> {
    T result_type;


/*
 * Create independent unrequested cancellation state; no task is spawned.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<CancellationSource, CancellationError> cancel_source() {}
/*
 * Derive an owned observer; this development signature reports allocation/count failure.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<CancellationToken, CancellationError> cancel_token(&CancellationSource source) {}
/*
 * Explicitly clone an observer; this development signature reports allocation/count failure.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<CancellationToken, CancellationError> cancel_clone(&CancellationToken token) {}
/*
 * Request cooperative cancellation through request authority.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<CancelRequest, CancellationError> cancel_request(&CancellationSource source) {}
/*
 * Read a notification snapshot without acquiring request authority.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public bool cancel_requested(&CancellationToken token) {}
/*
 * Allocate a lexical structured scope for the explicit supported native-thread backend.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void open_scope(&Executor executor, ScopeOptions options) {}
/*
 * Stop admission and drain every child; a reaping failure returns ownership of the closed scope.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void close_scope(TaskScope scope) {}
/*
 * Idempotently stop new admission without draining or consuming the borrowed scope.
 * Previously accepted preparations may still commit or abort; failure returns no scope owner.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void close_admission(&TaskScope scope) {}
/*
 * Observe Prepared, Queued and Running children, including discarded observations.
 * The scan performs O(n) child observations plus registry lookups, allocates nothing and never waits for work completion.
 * Per-child snapshots are not one coherent instant; zero leaves reaping and result-cleanup obligations intact.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void active_count(&TaskScope scope) {}
/*
 * Observe Open or Closing without consuming the scope or releasing its loans.
 * A successful close consumes the owner; this query never invents Drained from counts.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void scope_status(&TaskScope scope) {}
/*
 * Consume zero-argument owned work and register its gated native worker before commit.
 * Admission, allocation or thread-creation failure destroys the unstarted owned callable exactly once.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void spawn_owned<F>(&TaskScope scope, F work) {}
/*
 * Consume and register safe zero-argument owned work; borrowed captures remain rejected.
 * Admission, allocation or thread-creation failure destroys the unstarted owned callable exactly once.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void spawn<F>(&TaskScope scope, F work) {}
/*
 * Consume observation, wait and move T once; an application Result stays nested.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void join(Task<T> task) {}
/*
 * Nonblockingly take a terminal result once; running work returns absence and keeps the handle.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void try_take(&mut Task<T> task) {}
/*
 * Consume only observation without waiting; its scope retains all work and cleanup obligations.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void discard_result(Task<T> task) {}
/*
 * Observe phase and cancellation without taking a result or ending resource use.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void status(&Task<T> task) {}
/*
 * Observe readiness for a bounded duration; timeout neither cancels nor consumes the handle.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void wait_timeout(&Task<T> task, Duration timeout) {}
/*
 * Request cooperative cancellation; successful work may win the request race.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void request_cancel(&Task<T> task) {}
/*
 * Read a terminal-phase snapshot without taking the result or ending the scope loan.
 * A consumed observation remains finished; false is not a progress guarantee.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void is_finished(&Task<T> task) {}
/*
 * Read the current cooperative request flag without consuming observation or releasing its scope loan.
 * A consumed observation reports false; this snapshot never guarantees future progress or completion.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void is_cancel_requested(&Task<T> task) {}
/*
 * Allocate an independent cancellation observer while retaining the original Task on every outcome.
 * The observer owns notification state only, not work, scope, captures, results, or executable code.
 * Allocation/count failures and consumed observations return exact TaskError values.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void cancellation(&Task<T> task) {}
/*
 * Compute a checked monotonic Deadline through the ordinary source-defined Deadline.after implementation.
 * This operation neither registers an asynchronous timer nor promises an exact wakeup time.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<Deadline,TimerError> deadline_after(Duration duration) {}
/*
 * Wait for native completion without consuming observation; failures preserve the Task.
 * Completion may wait indefinitely and does not imply a successful application result.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void wait(&Task<T> task) {}
/*
 * Request cancellation and consume observation by joining its actual outcome.
 * Normal completion may win; reaping failure transfers cleanup to the retaining scope.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void cancel_and_join(Task<T> task) {}
/*
 * Read an owned cancellation observer and return a typed checkpoint error if requested.
 * This O(1) operation allocates nothing and never kills a worker or releases its captures.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void check_cancel(&CancellationToken token) {}
/*
 * Observe the selected execution backend, including after result consumption.
 * The returned fact is not a native thread identifier, registry identity, or capability.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public void executor(&Task<T> task) {}
/*
 * Read an opaque diagnostic identity, including after result observation is consumed.
 * This is neither an OS thread ID, a native owner token, nor a permission to access task state.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public TaskId id(&Task<T> task) {}
/*
 * Move an owned precomputed value into this scope without creating or scheduling a native worker.
 * Failed construction consumes and cleans the by-value input once; the result remains owned by scope until observed or cleaned.
 * The initial profile requires owned data with compiler-approved cleanup; borrowed values and hidden Task loans are rejected.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public Result<Task<T>,SpawnError> ready(&TaskScope scope, T value) {}
};

/*
 * An opaque Copy diagnostic serial unique within this linked runtime instance.
 * IDs may contain gaps; they are never recycled or reset by observation consumption or JIT invocation.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public struct TaskId implements Copy {
    u64 serial;
/*
 * Compare diagnostic identities without allocation or native state access.
 * @since 0.3
 * @deprecated_at none
 * @removed_at none
 */
public bool equals(&Self self, Self other) { return self.serial == other.serial; }
};

)AEVIXDOC";
}
