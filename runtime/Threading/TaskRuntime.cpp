// ========================
//
// Created By : miruku_sora2026
//
// File name : TaskRuntime.cpp
//
// Description :
//     세대 토큰, 확정 전 실행 차단과 정리 수명 보호자를 사용하는 네이티브 CPU 작업을 구현한다.
//     Implement native CPU tasks with generational tokens, pre-commit gating, and cleanup lifetime pins.
//
// ========================
#include "aevix/Runtime/TaskRuntime.hpp"
#include "aevix/Runtime/TaskScopeRuntime.hpp"
#include "aevix/Runtime/CancellationRuntime.hpp"
#include "aevix/Runtime/NativeBoundary.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

namespace aevix
{
    struct TaskContext final
    {
        const std::atomic<bool> *requested = nullptr;
    };
}
namespace
{
    using aevix::TaskPhase;
    using aevix::TaskRuntimeStatus;
    constexpr std::int32_t good = 0;
    constexpr std::int32_t status(TaskRuntimeStatus value) noexcept
    {
        return std::int32_t(value);
    }
    thread_local std::int32_t nativeError = 0, errorDomain = 0;
    thread_local std::uint32_t callbackDepth = 0;
#if defined(AEVIX_TASK_RUNTIME_TESTING)
    thread_local std::uint64_t allocationBudget = UINT64_MAX;
    thread_local bool failThreadCreation = false;
    thread_local std::uint32_t failReap = 0;
    std::atomic<bool> holdWorkerTail{false};
    std::atomic<std::uint64_t> threadCreationCalls{0}, nativeReapCalls{0};
#endif
    void begin() noexcept
    {
        nativeError = 0;
        errorDomain = 0;
    }
    std::int32_t threadFailure(std::int32_t code, std::int32_t domain = 1) noexcept
    {
        nativeError = code;
        errorDomain = domain;
        return status(TaskRuntimeStatus::ThreadFailure);
    }
    bool allowAllocation() noexcept
    {
#if defined(AEVIX_TASK_RUNTIME_TESTING)
        if (!allocationBudget)
            return false;
        if (allocationBudget != UINT64_MAX)
            --allocationBudget;
#endif
        return true;
    }
    // 취소 소유자는 작업 Control과 독립적인 작은 상태만 유지한다.
    // Cancellation owners retain only small state independent of Task controls.
    struct CancellationState final
    {
        std::atomic<bool> requested{false};
        std::uint64_t owners = 1;
    };
    struct CancelEntry final
    {
        CancelEntry *next = nullptr;
        CancellationState *state = nullptr;
        std::uint32_t slot = 0, generation = 0;
        bool token = false;
    };
    struct CancelRegistry final
    {
        std::mutex mutex;
        CancelEntry *head = nullptr;
        std::uint64_t nextSlot = 1, liveStates = 0;
        ~CancelRegistry() noexcept
        {
            // 네이티브 API 정지와 명시적인 모든 소유자의 해제가 먼저 필요하다.
            // Native API quiescence and explicit release of all owners must come first.
            if (liveStates)
                return;
            while (head)
            {
                auto *next = head->next;
                delete head;
                head = next;
            }
        }
    };
    CancelRegistry &cancelRegistry() noexcept
    {
        static CancelRegistry value;
        return value;
    }
    constexpr std::uint64_t cancelSourceTag = UINT64_C(0xc000000000000000),
                            cancelTokenTag = UINT64_C(0xd000000000000000);
    constexpr std::uint32_t cancelGenerationMaximum = 0x0fffffffu;
#if defined(AEVIX_TASK_RUNTIME_TESTING)
    std::atomic<std::uint64_t> cancelOwnerLimit{UINT64_MAX};
#endif
    std::uint64_t cancellationOwnerLimit() noexcept
    {
#if defined(AEVIX_TASK_RUNTIME_TESTING)
        return cancelOwnerLimit.load(std::memory_order_relaxed);
#else
        return UINT64_MAX;
#endif
    }
    std::int32_t createCancellationState(CancellationState *&output) noexcept
    {
        output = nullptr;
        if (!cancellationOwnerLimit())
            return status(TaskRuntimeStatus::SizeOverflow);
        auto *state = allowAllocation() ? new (std::nothrow) CancellationState : nullptr;
        if (!state)
            return status(TaskRuntimeStatus::AllocationFailure);
        {
            std::lock_guard lock(cancelRegistry().mutex);
            if (cancelRegistry().liveStates == UINT64_MAX)
            {
                delete state;
                return status(TaskRuntimeStatus::SizeOverflow);
            }
            ++cancelRegistry().liveStates;
        }
        output = state;
        return good;
    }
    void releaseCancellationState(CancellationState *state) noexcept
    {
        if (!state)
            return;
        bool final = false;
        {
            std::lock_guard lock(cancelRegistry().mutex);
            if (!--state->owners)
            {
                --cancelRegistry().liveStates;
                final = true;
            }
        }
        if (final)
            delete state;
    }
    CancelEntry *lookupCancellation(std::uint64_t owner, bool tokenRole) noexcept
    {
        const auto tag = tokenRole ? cancelTokenTag : cancelSourceTag;
        if ((owner & UINT64_C(0xf000000000000000)) != tag)
            return nullptr;
        const auto slot = std::uint32_t(owner), generation = std::uint32_t((owner >> 32) & cancelGenerationMaximum);
        if (!slot || !generation)
            return nullptr;
        for (auto *entry = cancelRegistry().head; entry; entry = entry->next)
            if (entry->slot == slot && entry->generation == generation && entry->token == tokenRole && entry->state)
                return entry;
        return nullptr;
    }
    std::int32_t cancellationEntry(CancelEntry *&selected) noexcept
    {
        selected = nullptr;
        for (auto *entry = cancelRegistry().head; entry; entry = entry->next)
            if (!entry->state && entry->generation < cancelGenerationMaximum)
            {
                selected = entry;
                break;
            }
        if (!selected)
        {
            if (cancelRegistry().nextSlot > UINT32_MAX)
                return status(TaskRuntimeStatus::SizeOverflow);
            selected = allowAllocation() ? new (std::nothrow) CancelEntry : nullptr;
            if (!selected)
                return status(TaskRuntimeStatus::AllocationFailure);
            selected->slot = std::uint32_t(cancelRegistry().nextSlot++);
            selected->next = cancelRegistry().head;
            cancelRegistry().head = selected;
        }
        ++selected->generation;
        return good;
    }
    std::int32_t publishCancellation(CancellationState *state, bool tokenRole, bool retain,
                                     std::uint64_t *output) noexcept
    {
        std::lock_guard lock(cancelRegistry().mutex);
        if (retain && state->owners >= cancellationOwnerLimit())
            return status(TaskRuntimeStatus::SizeOverflow);
        CancelEntry *entry = nullptr;
        if (const auto result = cancellationEntry(entry))
            return result;
        if (retain)
            ++state->owners;
        entry->state = state;
        entry->token = tokenRole;
        *output =
            (tokenRole ? cancelTokenTag : cancelSourceTag) | (std::uint64_t(entry->generation) << 32) | entry->slot;
        return good;
    }
    std::int32_t cloneCancellation(std::uint64_t owner, bool inputToken, bool outputToken,
                                   std::uint64_t *output) noexcept
    {
        if (!output)
            return status(TaskRuntimeStatus::InvalidInput);
        *output = 0;
        std::lock_guard lock(cancelRegistry().mutex);
        const auto *input = lookupCancellation(owner, inputToken);
        if (!input)
            return status(TaskRuntimeStatus::InvalidToken);
        if (input->state->owners >= cancellationOwnerLimit())
            return status(TaskRuntimeStatus::SizeOverflow);
        CancelEntry *entry = nullptr;
        if (const auto result = cancellationEntry(entry))
            return result;
        ++input->state->owners;
        entry->state = input->state;
        entry->token = outputToken;
        *output =
            (outputToken ? cancelTokenTag : cancelSourceTag) | (std::uint64_t(entry->generation) << 32) | entry->slot;
        return good;
    }
    std::int32_t releaseCancellationOwner(std::uint64_t owner, bool tokenRole) noexcept
    {
        if (!owner)
            return good;
        CancellationState *released = nullptr;
        {
            std::lock_guard lock(cancelRegistry().mutex);
            auto *entry = lookupCancellation(owner, tokenRole);
            if (!entry)
                return status(TaskRuntimeStatus::InvalidToken);
            auto *state = entry->state;
            entry->state = nullptr;
            if (!--state->owners)
            {
                --cancelRegistry().liveStates;
                released = state;
            }
        }
        if (released)
            delete released;
        return good;
    }
    struct Buffer final
    {
        void *base = nullptr;
        void *data = nullptr;
        std::uint64_t size = 0, alignment = 0;
    };
    bool validLayout(std::uint64_t size, std::uint64_t alignment) noexcept
    {
        constexpr auto maximum = std::uint64_t(PTRDIFF_MAX);
        const auto stored = size ? size : 1;
        return alignment && !(alignment & (alignment - 1)) && alignment <= maximum && stored <= maximum &&
               stored <= maximum - (alignment - 1);
    }
    std::int32_t allocateBuffer(Buffer &output, std::uint64_t size, std::uint64_t alignment) noexcept
    {
        const auto stored = size ? size : 1, bytes = stored + alignment - 1;
        output.base = allowAllocation() ? std::malloc(std::size_t(bytes)) : nullptr;
        if (!output.base)
            return status(TaskRuntimeStatus::AllocationFailure);
        const auto base = reinterpret_cast<std::uintptr_t>(output.base);
        if (base > UINTPTR_MAX - bytes)
        {
            std::free(output.base);
            output.base = nullptr;
            return status(TaskRuntimeStatus::SizeOverflow);
        }
        output.data = reinterpret_cast<void *>((base + alignment - 1) & ~(std::uintptr_t(alignment) - 1));
        output.size = size;
        output.alignment = alignment;
        return good;
    }
    struct Entry;
    enum class TaskExecution { Threaded, Ready };
    struct Control final
    {
        Entry *entry = nullptr;
        TaskExecution execution = TaskExecution::Threaded;
        std::mutex consume;
        std::atomic<TaskPhase> phase{TaskPhase::Prepared};
        std::atomic<bool> retired{false};
        CancellationState *cancellation = nullptr;
        aevix::TaskContext context{};
        Buffer capture, result;
        aevix::TaskInvoke invoke = nullptr;
        aevix::TaskDrop captureDrop = nullptr, resultDrop = nullptr;
        std::uint64_t pins = 0, diagnosticSerial = 0;
        std::int32_t outcome = 0;
        bool resultInitialized = false, created = false, reaped = false;
#if defined(AEVIX_TASK_RUNTIME_TESTING)
        std::atomic<bool> waitingNativeReap{false};
#endif
#if defined(_WIN32)
        HANDLE thread = nullptr;
        unsigned threadId = 0;
#else
        pthread_t thread{};
#endif
    };
    thread_local Control *currentWorker = nullptr;
    struct Entry final
    {
        Entry *next = nullptr;
        Control *control = nullptr;
        std::uint32_t slot = 0, generation = 0;
    };
    struct Registry final
    {
        std::mutex mutex;
        Entry *head = nullptr;
        std::uint64_t nextSlot = 1, nextIdentity = 1;
        std::atomic<std::uint64_t> workers{0};
        bool closing = false, identitiesExhausted = false;
        ~Registry() noexcept
        {
            // 명시적 drain과 네이티브 호출 정지가 먼저 필요하다. 여기서 사용자 정리를 실행하지 않는다.
            // Explicit drain and native-call quiescence must come first; never invoke user cleanup here.
            for (auto *entry = head; entry; entry = entry->next)
                if (entry->control)
                    return;
            while (head)
            {
                auto *next = head->next;
                delete head;
                head = next;
            }
        }
    };
    // 식별자는 런타임 인스턴스 전체의 단조 증가 진단 번호이며 슬롯·세대와 독립적이다.
    // Identity is a monotonic diagnostic serial for this runtime instance, independent of slots/generations.
#if defined(AEVIX_TASK_RUNTIME_TESTING) && defined(AEVIX_TASK_TEST_ID_MAX)
    constexpr std::uint64_t taskIdentityMaximum = AEVIX_TASK_TEST_ID_MAX;
    static_assert(taskIdentityMaximum > 0);
#else
    constexpr std::uint64_t taskIdentityMaximum = UINT64_MAX;
#endif
    bool disjointOutput(const void* left, std::size_t leftSize, const void* right, std::size_t rightSize) noexcept
    {
        const auto first = reinterpret_cast<std::uintptr_t>(left), second = reinterpret_cast<std::uintptr_t>(right);
        return first <= second ? second - first >= leftSize : first - second >= rightSize;
    }
    void resetPrepareOutputs(std::uint64_t* owner, void** capture, std::uint64_t* identity) noexcept
    {
        // 잘못 겹친 출력도 타입 별칭을 역참조하지 않고 실패 값으로 지운다.
        // Clear even invalid overlapping output storage without dereferencing conflicting typed aliases.
        const std::uint64_t empty = 0;
        void* emptyStorage = nullptr;
        if (owner) std::memcpy(owner,&empty,sizeof(empty));
        if (capture) std::memcpy(capture,&emptyStorage,sizeof(emptyStorage));
        if (identity) std::memcpy(identity,&empty,sizeof(empty));
    }
    bool validPrepareOutputs(std::uint64_t* owner, void** capture, std::uint64_t* identity) noexcept
    {
        return owner && capture && identity && disjointOutput(owner,sizeof(*owner),capture,sizeof(*capture))
            && disjointOutput(owner,sizeof(*owner),identity,sizeof(*identity))
            && disjointOutput(capture,sizeof(*capture),identity,sizeof(*identity));
    }
    Registry &registry() noexcept
    {
        static Registry value;
        return value;
    }
    void freeControl(Control *control) noexcept
    {
        std::free(control->capture.base);
        std::free(control->result.base);
        releaseCancellationState(control->cancellation);
        delete control;
    }
    std::uint64_t token(const Entry &entry) noexcept
    {
        return (std::uint64_t(entry.generation) << 32) | entry.slot;
    }
    Entry *lookup(std::uint64_t owner) noexcept
    {
        const auto slot = std::uint32_t(owner), generation = std::uint32_t(owner >> 32);
        if (!slot || !generation)
            return nullptr;
        for (auto *entry = registry().head; entry; entry = entry->next)
            if (entry->slot == slot && entry->generation == generation && entry->control &&
                !entry->control->retired.load())
                return entry;
        return nullptr;
    }
    void releasePin(Control *control) noexcept
    {
        Control *released = nullptr;
        {
            std::lock_guard lock(registry().mutex);
            --control->pins;
            if (!control->pins && control->retired.load())
            {
                control->entry->control = nullptr;
                released = control;
            }
        }
        if (released)
            freeControl(released);
    }
    struct Lease final
    {
        Control *control = nullptr;
        Lease() noexcept = default;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        ~Lease() noexcept
        {
            if (control)
                releasePin(control);
        }
    };
    std::int32_t pin(std::uint64_t owner, Lease &lease) noexcept
    {
        std::lock_guard lock(registry().mutex);
        auto *entry = lookup(owner);
        if (!entry)
            return status(TaskRuntimeStatus::InvalidToken);
        if (entry->control->pins == UINT64_MAX)
            return status(TaskRuntimeStatus::SizeOverflow);
        ++entry->control->pins;
        lease.control = entry->control;
        return good;
    }
    void retire(Control &control) noexcept
    {
        std::lock_guard lock(registry().mutex);
        control.retired.store(true, std::memory_order_release);
    }
    bool terminal(TaskPhase phase) noexcept
    {
        return phase == TaskPhase::Completed || phase == TaskPhase::Cancelled || phase == TaskPhase::Failed;
    }
    bool self(const Control &control) noexcept
    {
        return currentWorker == &control;
    }
    void drop(aevix::TaskDrop thunk, void *storage) noexcept
    {
        if (!thunk)
            return;
        ++callbackDepth;
        thunk(storage);
        --callbackDepth;
    }
    void run(Control *control) noexcept
    {
        auto *runtime = &registry();
        TaskPhase phase;
        do
        {
            phase = control->phase.load(std::memory_order_acquire);
            if (phase == TaskPhase::Prepared)
                std::this_thread::yield();
        } while (phase == TaskPhase::Prepared);
        if (phase != TaskPhase::Aborted)
        {
            if (control->cancellation->requested.load(std::memory_order_acquire))
            {
                drop(control->captureDrop, control->capture.data);
                control->outcome = std::int32_t(aevix::TaskInvokeOutcome::Cancelled);
                control->phase.store(TaskPhase::Cancelled, std::memory_order_release);
            }
            else
            {
                control->phase.store(TaskPhase::Running, std::memory_order_release);
                ++callbackDepth;
                const auto outcome = control->invoke(control->capture.data, control->result.data, &control->context);
                --callbackDepth;
                control->outcome = outcome;
                control->resultInitialized = outcome == std::int32_t(aevix::TaskInvokeOutcome::Completed);
                phase = outcome == std::int32_t(aevix::TaskInvokeOutcome::Completed)   ? TaskPhase::Completed
                        : outcome == std::int32_t(aevix::TaskInvokeOutcome::Cancelled) ? TaskPhase::Cancelled
                                                                                       : TaskPhase::Failed;
                control->phase.store(phase, std::memory_order_release);
            }
        }
        // 완료 게시 이후에는 사용자 페이로드·결과에 다시 접근하지 않는다. 아래는 메타데이터 꼬리뿐이다.
        // Never access user capture/result storage after terminal publication; only native metadata remains below.
#if defined(AEVIX_TASK_RUNTIME_TESTING)
        while (holdWorkerTail.load(std::memory_order_acquire))
            std::this_thread::yield();
#endif
        releasePin(control);
        runtime->workers.fetch_sub(1, std::memory_order_release);
    }
#if defined(_WIN32)
    unsigned __stdcall entryPoint(void *value) noexcept
    {
        currentWorker = static_cast<Control *>(value);
        run(currentWorker);
        currentWorker = nullptr;
        return 0;
    }
#else
    void *entryPoint(void *value) noexcept
    {
        currentWorker = static_cast<Control *>(value);
        run(currentWorker);
        currentWorker = nullptr;
        return nullptr;
    }
#endif
    std::int32_t createThread(Control &control) noexcept
    {
#if defined(AEVIX_TASK_RUNTIME_TESTING)
        threadCreationCalls.fetch_add(1,std::memory_order_relaxed);
        if (failThreadCreation)
            return threadFailure(EAGAIN);
#endif
#if defined(_WIN32)
        const auto thread = _beginthreadex(nullptr, 0, entryPoint, &control, 0, &control.threadId);
        if (!thread)
            return threadFailure(errno);
        control.thread = reinterpret_cast<HANDLE>(thread);
#else
        const auto error = pthread_create(&control.thread, nullptr, entryPoint, &control);
        if (error)
            return threadFailure(error);
#endif
        control.created = true;
        return good;
    }
    std::int32_t reap(Control &control, bool automatic) noexcept
    {
        // Ready는 OS 작업자나 회수 의무가 없으며 스레드 실패 주입 경로에도 진입하지 않는다.
        // Ready has no OS worker or reaping obligation and never enters thread-failure injection paths.
        if (control.execution == TaskExecution::Ready) return good;
        if (control.reaped)
            return good;
        if (self(control))
            return status(TaskRuntimeStatus::WouldDeadlock);
#if defined(AEVIX_TASK_RUNTIME_TESTING)
        nativeReapCalls.fetch_add(1,std::memory_order_relaxed);
        if (failReap & (automatic ? 2u : 1u))
            return threadFailure(EAGAIN);
        struct MarkWait final
        {
            std::atomic<bool> &flag;
            bool active;
            MarkWait(std::atomic<bool> &value, bool mark) noexcept : flag(value), active(mark)
            {
                if (active)
                    flag.store(true);
            }
            ~MarkWait() noexcept
            {
                if (active)
                    flag.store(false);
            }
        } markWait(control.waitingNativeReap, !automatic);
#endif
#if defined(_WIN32)
        if (!automatic && WaitForSingleObject(control.thread, INFINITE) != WAIT_OBJECT_0)
            return threadFailure(std::int32_t(GetLastError()), 2);
        if (!CloseHandle(control.thread))
            return threadFailure(std::int32_t(GetLastError()), 2);
        control.thread = nullptr;
#else
        const auto error = automatic ? pthread_detach(control.thread) : pthread_join(control.thread, nullptr);
        if (error)
            return threadFailure(error);
#endif
        control.reaped = true;
        return good;
    }
    std::int32_t output(Control &control, void *destination, std::uint64_t size) noexcept
    {
        const auto checked = aevix::NativeOutputBytes::checkedRaw(destination, size);
        if (!checked)
            return status(checked.status == aevix::NativeBufferStatus::NullWithNonzeroLength
                              ? TaskRuntimeStatus::InvalidInput
                              : TaskRuntimeStatus::SizeOverflow);
        if (size != control.result.size)
            return status(TaskRuntimeStatus::InvalidInput);
        if (!size)
            return good;
        const auto start = reinterpret_cast<std::uintptr_t>(destination);
        const auto end = start + size;
        if (start & (control.result.alignment - 1))
            return status(TaskRuntimeStatus::InvalidInput);
        for (const auto &buffer : {control.capture, control.result})
        {
            const auto begin = reinterpret_cast<std::uintptr_t>(buffer.data);
            if (buffer.size && start < begin + buffer.size && begin < end)
                return status(TaskRuntimeStatus::InvalidInput);
        }
        return good;
    }
    std::int32_t take(std::uint64_t owner, void *destination, std::uint64_t size, bool nonblocking) noexcept
    {
        Lease lease;
        if (const auto result = pin(owner, lease))
            return result;
        auto &control = *lease.control;
        if (const auto result = output(control, destination, size))
            return result;
        if (self(control))
            return status(TaskRuntimeStatus::WouldDeadlock);
        std::unique_lock lock(control.consume, std::defer_lock);
        if (nonblocking)
        {
            if (!lock.try_lock())
                return status(TaskRuntimeStatus::NotReady);
        }
        else
            lock.lock();
        if (control.retired.load())
            return status(TaskRuntimeStatus::InvalidToken);
        const auto phase = control.phase.load(std::memory_order_acquire);
        if (phase == TaskPhase::Prepared || phase == TaskPhase::Aborted)
            return status(TaskRuntimeStatus::InvalidState);
        if (nonblocking && !terminal(phase))
            return status(TaskRuntimeStatus::NotReady);
        if (const auto result = reap(control, nonblocking))
            return result;
        const auto completed = control.phase.load(std::memory_order_acquire);
        std::int32_t result = status(TaskRuntimeStatus::Failed);
        if (completed == TaskPhase::Completed)
        {
            if (size)
                std::memcpy(destination, control.result.data, std::size_t(size));
            control.resultInitialized = false;
            result = good;
        }
        else if (completed == TaskPhase::Cancelled)
            result = status(TaskRuntimeStatus::Cancelled);
        retire(control);
        lock.unlock();
        return result;
    }
}

extern "C"
{
    std::int32_t aevix_task_prepare(std::uint64_t captureSize, std::uint64_t captureAlignment, std::uint64_t resultSize,
                                    std::uint64_t resultAlignment, aevix::TaskInvoke invoke,
                                    aevix::TaskDrop captureDrop, aevix::TaskDrop resultDrop, std::uint64_t *owner,
                                    void **capture) noexcept
    {
        std::uint64_t diagnosticSerial = 0;
        return aevix_task_prepare_identified(captureSize,captureAlignment,resultSize,resultAlignment,invoke,
            captureDrop,resultDrop,owner,capture,&diagnosticSerial);
    }
    static std::int32_t prepareTaskControl(TaskExecution execution, std::uint64_t captureSize, std::uint64_t captureAlignment,
                                    std::uint64_t resultSize, std::uint64_t resultAlignment, aevix::TaskInvoke invoke,
                                    aevix::TaskDrop captureDrop, aevix::TaskDrop resultDrop, std::uint64_t *owner,
                                    void **capture, std::uint64_t *diagnosticSerial) noexcept
    {
        begin();
        resetPrepareOutputs(owner,capture,diagnosticSerial);
        if (!validPrepareOutputs(owner,capture,diagnosticSerial) || (execution == TaskExecution::Threaded && !invoke)
            || (execution == TaskExecution::Ready && (invoke || captureSize || captureDrop)) || !captureAlignment || !resultAlignment ||
            (captureAlignment & (captureAlignment - 1)) || (resultAlignment & (resultAlignment - 1)))
            return status(TaskRuntimeStatus::InvalidInput);
        if (!validLayout(captureSize, captureAlignment) || !validLayout(resultSize, resultAlignment))
            return status(TaskRuntimeStatus::SizeOverflow);
        auto *control = allowAllocation() ? new (std::nothrow) Control : nullptr;
        if (!control)
            return status(TaskRuntimeStatus::AllocationFailure);
        if (const auto result = createCancellationState(control->cancellation))
        {
            freeControl(control);
            return result;
        }
        control->execution = execution;
        control->context.requested = &control->cancellation->requested;
        if (execution == TaskExecution::Threaded)
            if (auto result = allocateBuffer(control->capture, captureSize, captureAlignment))
            {
                freeControl(control);
                return result;
            }
        if (auto result = allocateBuffer(control->result, resultSize, resultAlignment))
        {
            freeControl(control);
            return result;
        }
        control->invoke = invoke;
        control->captureDrop = captureDrop;
        control->resultDrop = resultDrop;
        Lease creator;
        {
            std::lock_guard lock(registry().mutex);
            if (registry().closing)
            {
                freeControl(control);
                return status(TaskRuntimeStatus::InvalidState);
            }
            if (registry().identitiesExhausted)
            {
                freeControl(control);
                return status(TaskRuntimeStatus::SizeOverflow);
            }
            Entry *selected = nullptr;
            for (auto *entry = registry().head; entry; entry = entry->next)
                if (!entry->control && entry->generation < UINT32_MAX)
                {
                    selected = entry;
                    break;
                }
            if (!selected)
            {
                if (registry().nextSlot > UINT32_MAX)
                {
                    freeControl(control);
                    return status(TaskRuntimeStatus::SizeOverflow);
                }
                selected = allowAllocation() ? new (std::nothrow) Entry : nullptr;
                if (!selected)
                {
                    freeControl(control);
                    return status(TaskRuntimeStatus::AllocationFailure);
                }
                selected->slot = std::uint32_t(registry().nextSlot++);
                selected->next = registry().head;
                registry().head = selected;
            }
            // 마지막 번호도 정확히 한 번 사용하며 실패한 스레드 생성은 번호를 되돌리지 않는다.
            // Issue the last serial exactly once; a failed thread creation never rolls identity back.
            control->diagnosticSerial = registry().nextIdentity;
            if (registry().nextIdentity == taskIdentityMaximum) registry().identitiesExhausted = true;
            else ++registry().nextIdentity;
            ++selected->generation;
            selected->control = control;
            control->entry = selected;
            control->pins = execution == TaskExecution::Threaded ? 2 : 1;
            creator.control = control;
            if (execution == TaskExecution::Threaded) registry().workers.fetch_add(1, std::memory_order_relaxed);
        }
        if (execution == TaskExecution::Threaded)
            if (const auto result = createThread(*control))
            {
                registry().workers.fetch_sub(1, std::memory_order_release);
                releasePin(control);
                retire(*control);
                return result;
            }
        *owner = token(*control->entry);
        *capture = execution == TaskExecution::Threaded ? control->capture.data : control->result.data;
        *diagnosticSerial = control->diagnosticSerial;
        return good;
    }
    std::int32_t aevix_task_prepare_identified(std::uint64_t captureSize, std::uint64_t captureAlignment,
                                    std::uint64_t resultSize, std::uint64_t resultAlignment, aevix::TaskInvoke invoke,
                                    aevix::TaskDrop captureDrop, aevix::TaskDrop resultDrop, std::uint64_t *owner,
                                    void **capture, std::uint64_t *diagnosticSerial) noexcept
    {
        return prepareTaskControl(TaskExecution::Threaded,captureSize,captureAlignment,resultSize,resultAlignment,
            invoke,captureDrop,resultDrop,owner,capture,diagnosticSerial);
    }
    static std::int32_t commitReady(std::uint64_t owner) noexcept
    {
        begin();
        Lease lease;
        if (const auto result = pin(owner,lease)) return result;
        std::lock_guard lock(lease.control->consume);
        auto& control = *lease.control;
        if (control.retired.load()) return status(TaskRuntimeStatus::InvalidToken);
        if (control.execution != TaskExecution::Ready || control.phase.load() != TaskPhase::Prepared)
            return status(TaskRuntimeStatus::InvalidState);
        control.resultInitialized = true;
        control.phase.store(TaskPhase::Completed,std::memory_order_release);
        return good;
    }
    std::int32_t aevix_task_commit(std::uint64_t owner) noexcept
    {
        begin();
        Lease lease;
        if (const auto result = pin(owner, lease))
            return result;
        std::lock_guard lock(lease.control->consume);
        if (lease.control->retired.load())
            return status(TaskRuntimeStatus::InvalidToken);
        if (lease.control->execution != TaskExecution::Threaded || lease.control->phase.load() != TaskPhase::Prepared)
            return status(TaskRuntimeStatus::InvalidState);
        lease.control->phase.store(TaskPhase::Queued, std::memory_order_release);
        return good;
    }
    std::int32_t aevix_task_abort(std::uint64_t owner) noexcept
    {
        begin();
        Lease lease;
        if (const auto result = pin(owner, lease))
            return result;
        auto &control = *lease.control;
        if (self(control))
            return status(TaskRuntimeStatus::WouldDeadlock);
        std::unique_lock lock(control.consume);
        if (control.retired.load())
            return status(TaskRuntimeStatus::InvalidToken);
        const auto phase = control.phase.load();
        if (phase != TaskPhase::Prepared && phase != TaskPhase::Aborted)
            return status(TaskRuntimeStatus::InvalidState);
        control.phase.store(TaskPhase::Aborted, std::memory_order_release);
        if (const auto result = reap(control, false))
            return result;
        retire(control);
        return good;
    }
    std::int32_t aevix_task_join(std::uint64_t owner, void *destination, std::uint64_t size) noexcept
    {
        begin();
        return take(owner, destination, size, false);
    }
    std::int32_t aevix_task_try_take(std::uint64_t owner, void *destination, std::uint64_t size) noexcept
    {
        begin();
        return take(owner, destination, size, true);
    }
    std::int32_t aevix_task_wait(std::uint64_t owner) noexcept
    {
        begin();
        Lease lease;
        if (const auto result = pin(owner, lease))
            return result;
        auto &control = *lease.control;
        if (self(control))
            return status(TaskRuntimeStatus::WouldDeadlock);
        std::lock_guard lock(control.consume);
        if (control.retired.load())
            return status(TaskRuntimeStatus::InvalidToken);
        const auto phase = control.phase.load(std::memory_order_acquire);
        if (phase == TaskPhase::Prepared || phase == TaskPhase::Aborted)
            return status(TaskRuntimeStatus::InvalidState);
        return reap(control, false);
    }
    std::int32_t aevix_task_wait_timeout(std::uint64_t owner, std::uint64_t nanoseconds) noexcept
    {
        begin();
        if (nanoseconds > std::uint64_t(INT64_MAX))
            return status(TaskRuntimeStatus::InvalidInput);
        Lease lease;
        if (const auto result = pin(owner, lease))
            return result;
        auto &control = *lease.control;
        if (self(control))
            return status(TaskRuntimeStatus::WouldDeadlock);
        const auto start = std::chrono::steady_clock::now();
        for (;;)
        {
            if (control.retired.load())
                return status(TaskRuntimeStatus::InvalidToken);
            const auto phase = control.phase.load(std::memory_order_acquire);
            if (phase == TaskPhase::Prepared || phase == TaskPhase::Aborted)
                return status(TaskRuntimeStatus::InvalidState);
            if (terminal(phase))
                return good;
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed >= 0 && std::uint64_t(elapsed) >= nanoseconds)
                return status(TaskRuntimeStatus::TimedOut);
            std::this_thread::yield();
        }
    }
    std::int32_t aevix_task_cleanup(std::uint64_t owner) noexcept
    {
        if (!owner)
            return good;
        begin();
        Lease lease;
        if (const auto result = pin(owner, lease))
            return result;
        auto &control = *lease.control;
        if (self(control))
            return status(TaskRuntimeStatus::WouldDeadlock);
        std::unique_lock lock(control.consume);
        if (control.retired.load())
            return status(TaskRuntimeStatus::InvalidToken);
        const auto phase = control.phase.load(std::memory_order_acquire);
        if (phase == TaskPhase::Prepared || phase == TaskPhase::Aborted)
            return status(TaskRuntimeStatus::InvalidState);
        if (const auto result = reap(control, false))
            return result;
        const auto destroy = control.resultInitialized;
        control.resultInitialized = false;
        retire(control);
        lock.unlock();
        if (destroy)
            drop(control.resultDrop, control.result.data);
        return good;
    }
    std::int32_t aevix_task_runtime_drain() noexcept
    {
        begin();
        if (callbackDepth)
            return status(TaskRuntimeStatus::WouldDeadlock);
        {
            std::lock_guard lock(registry().mutex);
            registry().closing = true;
        }
        for (;;)
        {
            std::uint64_t owner = 0;
            bool retained = false;
            {
                std::lock_guard lock(registry().mutex);
                for (auto *entry = registry().head; entry; entry = entry->next)
                    if (entry->control)
                    {
                        retained = true;
                        const auto phase = entry->control->phase.load();
                        if (!entry->control->retired.load() && phase != TaskPhase::Prepared &&
                            phase != TaskPhase::Aborted)
                        {
                            owner = token(*entry);
                            break;
                        }
                    }
            }
            if (owner)
            {
                const auto result = aevix_task_cleanup(owner);
                if (result != good && result != status(TaskRuntimeStatus::InvalidToken))
                    return result;
            }
            else if (!retained && !registry().workers.load(std::memory_order_acquire))
                return good;
            else
                std::this_thread::yield();
        }
    }
    std::int32_t aevix_task_request_cancel(std::uint64_t owner, std::uint32_t *disposition) noexcept
    {
        begin();
        if (!disposition)
            return status(TaskRuntimeStatus::InvalidInput);
        *disposition = 0;
        Lease lease;
        if (const auto result = pin(owner, lease))
            return result;
        const auto phase = lease.control->phase.load(std::memory_order_acquire);
        if (terminal(phase))
        {
            *disposition = 1;
            return good;
        }
        if (phase == TaskPhase::Aborted)
            return status(TaskRuntimeStatus::InvalidState);
        *disposition = lease.control->cancellation->requested.exchange(true, std::memory_order_acq_rel) ? 2 : 0;
        return good;
    }
    std::int32_t aevix_task_snapshot(std::uint64_t owner, aevix::TaskSnapshot *output) noexcept
    {
        begin();
        if (!output)
            return status(TaskRuntimeStatus::InvalidInput);
        *output = {};
        Lease lease;
        if (const auto result = pin(owner, lease))
            return result;
        const auto phase = lease.control->phase.load(std::memory_order_acquire);
        output->phase = std::uint32_t(phase);
        output->cancelRequested = lease.control->cancellation->requested.load(std::memory_order_acquire);
        if (terminal(phase))
            output->invokeOutcome = lease.control->outcome;
        return good;
    }
    std::uint32_t aevix_task_context_cancel_requested(const aevix::TaskContext *context) noexcept
    {
        return context && context->requested && context->requested->load(std::memory_order_acquire);
    }
    std::int32_t aevix_task_last_error() noexcept
    {
        return nativeError;
    }
    std::int32_t aevix_task_error_domain() noexcept
    {
        return errorDomain;
    }
#if defined(AEVIX_TASK_RUNTIME_TESTING)
    void aevix_task_test_fail_after(std::uint64_t count) noexcept
    {
        allocationBudget = count;
    }
    void aevix_task_test_fail_threads(std::uint32_t fail) noexcept
    {
        failThreadCreation = fail != 0;
    }
    void aevix_task_test_fail_reap(std::uint32_t fail) noexcept
    {
        failReap = fail;
    }
    void aevix_task_test_hold_worker_tail(std::uint32_t hold) noexcept
    {
        holdWorkerTail.store(hold != 0, std::memory_order_release);
    }
    std::uint32_t aevix_task_test_is_reaping(std::uint64_t owner) noexcept
    {
        Lease lease;
        return pin(owner, lease) == good && lease.control->waitingNativeReap.load();
    }
    std::uint64_t aevix_task_test_thread_creations() noexcept
    { return threadCreationCalls.load(std::memory_order_relaxed); }
    std::uint64_t aevix_task_test_native_reaps() noexcept
    { return nativeReapCalls.load(std::memory_order_relaxed); }
    std::uint64_t aevix_task_test_live_controls() noexcept
    {
        std::lock_guard lock(registry().mutex);
        std::uint64_t result = 0;
        for (auto *entry = registry().head; entry; entry = entry->next)
            if (entry->control)
                ++result;
        return result;
    }
#endif
}

namespace
{
    // 스코프의 결과 관측 토큰은 기저 Task 토큰과 별개이며 버린 핸들도 작업 추적을 끝내지 않는다.
    // Scoped observation tokens are separate from underlying Task tokens; discarding one never untracks work.
    constexpr std::uint64_t scopeTag = UINT64_C(1) << 62, handleTag = UINT64_C(2) << 62;
    constexpr std::uint32_t scopeGenerationMaximum = 0x3fffffffu;
    enum class ObservationState : std::uint8_t
    {
        Ready,
        Busy,
        Discarded,
        Consumed
    };
    struct ScopeControl;
    struct ScopeChild;
    struct ScopeEntry final
    {
        ScopeEntry *next = nullptr;
        ScopeControl *scope = nullptr;
        ScopeChild *child = nullptr;
        std::uint32_t slot = 0, generation = 0;
        bool reserved = false;
    };
    struct ScopeChild final
    {
        ScopeChild *next = nullptr;
        ScopeControl *scope = nullptr;
        ScopeEntry *entry = nullptr;
        std::mutex consume;
        std::atomic<std::uint64_t> owner{0};
        std::atomic<ObservationState> observation{ObservationState::Ready};
        std::atomic<bool> transaction{true}, observed{false}, discarded{false};
        TaskPhase settled = TaskPhase::Prepared;
    };
    struct ScopeControl final
    {
        ScopeEntry *entry = nullptr;
        ScopeChild *children = nullptr;
        std::mutex admission;
        std::atomic<bool> closeActive{false};
        std::uint64_t accepted = 0, pins = 0;
        bool closing = false, retired = false;
    };
    struct ScopeRegistry final
    {
        std::mutex mutex;
        ScopeEntry *scopes = nullptr, *handles = nullptr;
        std::uint64_t nextScope = 1, nextHandle = 1;
        bool closing = false;
        ~ScopeRegistry() noexcept
        {
            // 정상 종료는 명시적 스코프 drain과 외부 API 정지를 먼저 요구한다.
            // Normal shutdown requires explicit scope drain and external API quiescence first.
            for (auto *entry = scopes; entry; entry = entry->next)
                if (entry->scope)
                    return;
            while (scopes)
            {
                auto *next = scopes->next;
                delete scopes;
                scopes = next;
            }
            while (handles)
            {
                auto *next = handles->next;
                delete handles;
                handles = next;
            }
        }
    };
    ScopeRegistry &scopeRegistry() noexcept
    {
        static ScopeRegistry value;
        return value;
    }
    std::uint64_t scopeToken(const ScopeEntry &entry, std::uint64_t tag) noexcept
    {
        return tag | (std::uint64_t(entry.generation) << 32) | entry.slot;
    }
    ScopeEntry *scopeEntry(std::uint64_t value, bool handle) noexcept
    {
        const auto tag = handle ? handleTag : scopeTag;
        if ((value & (UINT64_C(3) << 62)) != tag)
            return nullptr;
        const auto slot = std::uint32_t(value), generation = std::uint32_t((value >> 32) & scopeGenerationMaximum);
        if (!slot || !generation)
            return nullptr;
        for (auto *entry = handle ? scopeRegistry().handles : scopeRegistry().scopes; entry; entry = entry->next)
            if (entry->slot == slot && entry->generation == generation && !entry->reserved &&
                (handle ? entry->child != nullptr : entry->scope != nullptr))
                return entry;
        return nullptr;
    }
    ScopeEntry *reserveScopeEntry(bool handle) noexcept
    {
        auto *&head = handle ? scopeRegistry().handles : scopeRegistry().scopes;
        auto &next = handle ? scopeRegistry().nextHandle : scopeRegistry().nextScope;
        for (auto *entry = head; entry; entry = entry->next)
            if (!entry->reserved && !entry->scope && !entry->child && entry->generation < scopeGenerationMaximum)
            {
                ++entry->generation;
                entry->reserved = true;
                return entry;
            }
        if (next > UINT32_MAX)
            return nullptr;
        auto *entry = allowAllocation() ? new (std::nothrow) ScopeEntry : nullptr;
        if (!entry)
            return nullptr;
        entry->slot = std::uint32_t(next++);
        entry->generation = 1;
        entry->reserved = true;
        entry->next = head;
        head = entry;
        return entry;
    }
    void freeScope(ScopeControl *scope) noexcept
    {
        while (scope->children)
        {
            auto *next = scope->children->next;
            delete scope->children;
            scope->children = next;
        }
        delete scope;
    }
    void releaseScopePin(ScopeControl *scope) noexcept
    {
        bool release = false;
        {
            std::lock_guard lock(scopeRegistry().mutex);
            --scope->pins;
            if (!scope->pins && scope->retired)
            {
                scope->entry->scope = nullptr;
                release = true;
            }
        }
        if (release)
            freeScope(scope);
    }
    struct ScopeLease final
    {
        ScopeControl *scope = nullptr;
        ScopeChild *child = nullptr;
        ~ScopeLease() noexcept
        {
            if (scope)
                releaseScopePin(scope);
        }
    };
    std::int32_t pinScope(std::uint64_t value, ScopeLease &lease, bool handle = false) noexcept
    {
        std::lock_guard lock(scopeRegistry().mutex);
        auto *entry = scopeEntry(value, handle);
        if (!entry)
            return status(TaskRuntimeStatus::InvalidToken);
        auto *scope = handle ? entry->child->scope : entry->scope;
        if (scope->retired)
            return status(TaskRuntimeStatus::InvalidToken);
        if (scope->pins == UINT64_MAX)
            return status(TaskRuntimeStatus::SizeOverflow);
        ++scope->pins;
        lease.scope = scope;
        lease.child = handle ? entry->child : nullptr;
        return good;
    }
    void retireObservation(ScopeChild &child) noexcept
    {
        std::lock_guard lock(scopeRegistry().mutex);
        if (child.entry->child == &child)
            child.entry->child = nullptr;
    }
    bool scopeSelf(const ScopeControl &scope) noexcept
    {
        if (!currentWorker)
            return false;
        const auto current = token(*currentWorker->entry);
        for (auto *child = scope.children; child; child = child->next)
            if (child->owner.load(std::memory_order_acquire) == current)
                return true;
        return false;
    }
    bool childSelf(const ScopeChild &child) noexcept
    {
        return currentWorker && token(*currentWorker->entry) == child.owner.load(std::memory_order_acquire);
    }
    struct ClosingScope final
    {
        ScopeControl *scope;
        ClosingScope *parent;
    };
    thread_local ClosingScope *closingScopes = nullptr;
    bool reentrantClose(const ScopeControl &scope) noexcept
    {
        for (auto *frame = closingScopes; frame; frame = frame->parent)
            if (frame->scope == &scope)
                return true;
        return false;
    }
    bool consumedOutcome(std::int32_t value) noexcept
    {
        return value == good || value == status(TaskRuntimeStatus::Cancelled) ||
               value == status(TaskRuntimeStatus::Failed);
    }
    std::int32_t scopedTake(std::uint64_t handle, void *destination, std::uint64_t size, bool nonblocking) noexcept
    {
        ScopeLease lease;
        if (const auto result = pinScope(handle, lease, true))
            return result;
        auto &child = *lease.child;
        if (childSelf(child))
            return status(TaskRuntimeStatus::WouldDeadlock);
        std::unique_lock lock(child.consume, std::defer_lock);
        if (nonblocking)
        {
            if (!lock.try_lock())
                return status(TaskRuntimeStatus::NotReady);
        }
        else
            lock.lock();
        auto expected = ObservationState::Ready;
        if (!child.observation.compare_exchange_strong(expected, ObservationState::Busy))
            return status(TaskRuntimeStatus::InvalidToken);
        const auto owner = child.owner.load();
        const auto result =
            nonblocking ? aevix_task_try_take(owner, destination, size) : aevix_task_join(owner, destination, size);
        if (consumedOutcome(result))
        {
            child.settled = result == good                                   ? TaskPhase::Completed
                            : result == status(TaskRuntimeStatus::Cancelled) ? TaskPhase::Cancelled
                                                                             : TaskPhase::Failed;
            child.observed = true;
            child.owner = 0;
            child.observation = ObservationState::Consumed;
            retireObservation(child);
        }
        else
            child.observation = ObservationState::Ready;
        return result;
    }
}

extern "C"
{
    std::int32_t aevix_task_scope_open(std::uint64_t *output) noexcept
    {
        begin();
        if (!output)
            return status(TaskRuntimeStatus::InvalidInput);
        *output = 0;
        auto *scope = allowAllocation() ? new (std::nothrow) ScopeControl : nullptr;
        if (!scope)
            return status(TaskRuntimeStatus::AllocationFailure);
        {
            std::lock_guard lock(scopeRegistry().mutex);
            if (scopeRegistry().closing)
            {
                delete scope;
                return status(TaskRuntimeStatus::InvalidState);
            }
            auto *entry = reserveScopeEntry(false);
            if (!entry)
            {
                delete scope;
                return status(TaskRuntimeStatus::AllocationFailure);
            }
            scope->entry = entry;
            entry->scope = scope;
            entry->reserved = false;
            *output = scopeToken(*entry, scopeTag);
        }
        return good;
    }
    std::int32_t aevix_task_scope_prepare(std::uint64_t scopeTokenValue, std::uint64_t captureSize,
                                          std::uint64_t captureAlignment, std::uint64_t resultSize,
                                          std::uint64_t resultAlignment, aevix::TaskInvoke invoke,
                                          aevix::TaskDrop captureDrop, aevix::TaskDrop resultDrop,
                                          std::uint64_t *handle, void **storage) noexcept
    {
        std::uint64_t diagnosticSerial = 0;
        return aevix_task_scope_prepare_identified(scopeTokenValue,captureSize,captureAlignment,resultSize,resultAlignment,
            invoke,captureDrop,resultDrop,handle,storage,&diagnosticSerial);
    }
    static std::int32_t prepareScopedControl(TaskExecution execution, std::uint64_t scopeTokenValue, std::uint64_t captureSize,
                                          std::uint64_t captureAlignment, std::uint64_t resultSize,
                                          std::uint64_t resultAlignment, aevix::TaskInvoke invoke,
                                          aevix::TaskDrop captureDrop, aevix::TaskDrop resultDrop,
                                          std::uint64_t *handle, void **storage, std::uint64_t *diagnosticSerial) noexcept
    {
        begin();
        resetPrepareOutputs(handle,storage,diagnosticSerial);
        if (!validPrepareOutputs(handle,storage,diagnosticSerial))
            return status(TaskRuntimeStatus::InvalidInput);
        ScopeLease lease;
        if (const auto result = pinScope(scopeTokenValue, lease))
            return result;
        auto &scope = *lease.scope;
        std::lock_guard admission(scope.admission);
        if (scope.closing)
            return status(TaskRuntimeStatus::InvalidState);
        if (scope.accepted == UINT64_MAX)
            return status(TaskRuntimeStatus::SizeOverflow);
        auto *child = allowAllocation() ? new (std::nothrow) ScopeChild : nullptr;
        if (!child)
            return status(TaskRuntimeStatus::AllocationFailure);
        ScopeEntry *entry;
        {
            std::lock_guard lock(scopeRegistry().mutex);
            entry = reserveScopeEntry(true);
        }
        if (!entry)
        {
            delete child;
            return status(TaskRuntimeStatus::AllocationFailure);
        }
        std::uint64_t nativeOwner = 0, serial = 0;
        const auto result = prepareTaskControl(execution,captureSize, captureAlignment, resultSize, resultAlignment, invoke,
                                               captureDrop, resultDrop, &nativeOwner, storage, &serial);
        if (result)
        {
            {
                std::lock_guard lock(scopeRegistry().mutex);
                entry->reserved = false;
            }
            delete child;
            return result;
        }
        child->scope = &scope;
        child->entry = entry;
        child->owner = nativeOwner;
        child->next = scope.children;
        scope.children = child;
        ++scope.accepted;
        {
            std::lock_guard lock(scopeRegistry().mutex);
            entry->child = child;
            entry->reserved = false;
        }
        *handle = ::scopeToken(*entry, handleTag);
        *diagnosticSerial = serial;
        return good;
    }
    std::int32_t aevix_task_scope_prepare_identified(std::uint64_t scopeTokenValue, std::uint64_t captureSize,
                                          std::uint64_t captureAlignment, std::uint64_t resultSize,
                                          std::uint64_t resultAlignment, aevix::TaskInvoke invoke,
                                          aevix::TaskDrop captureDrop, aevix::TaskDrop resultDrop,
                                          std::uint64_t *handle, void **storage, std::uint64_t *diagnosticSerial) noexcept
    {
        return prepareScopedControl(TaskExecution::Threaded,scopeTokenValue,captureSize,captureAlignment,resultSize,resultAlignment,
            invoke,captureDrop,resultDrop,handle,storage,diagnosticSerial);
    }
    std::int32_t aevix_task_scope_prepare_ready(std::uint64_t scopeTokenValue, std::uint64_t resultSize,
                                          std::uint64_t resultAlignment, aevix::TaskDrop resultDrop,
                                          std::uint64_t *handle, void **storage, std::uint64_t *diagnosticSerial) noexcept
    {
        return prepareScopedControl(TaskExecution::Ready,scopeTokenValue,0,1,resultSize,resultAlignment,
            nullptr,nullptr,resultDrop,handle,storage,diagnosticSerial);
    }
    std::int32_t aevix_scoped_task_ready_commit(std::uint64_t handle) noexcept
    {
        begin();
        ScopeLease lease;
        if (const auto result = pinScope(handle,lease,true)) return result;
        auto& child = *lease.child;
        std::lock_guard lock(child.consume);
        auto expected = ObservationState::Ready;
        if (!child.observation.compare_exchange_strong(expected,ObservationState::Busy)) return status(TaskRuntimeStatus::InvalidToken);
        const auto result = commitReady(child.owner.load());
        if (!result) child.transaction.store(false,std::memory_order_release);
        child.observation = ObservationState::Ready;
        return result;
    }
    std::int32_t aevix_scoped_task_commit(std::uint64_t handle) noexcept
    {
        begin();
        ScopeLease lease;
        if (const auto result = pinScope(handle, lease, true))
            return result;
        auto &child = *lease.child;
        std::lock_guard lock(child.consume);
        auto expected = ObservationState::Ready;
        if (!child.observation.compare_exchange_strong(expected, ObservationState::Busy))
            return status(TaskRuntimeStatus::InvalidToken);
        const auto result = aevix_task_commit(child.owner.load());
        if (!result)
            child.transaction.store(false, std::memory_order_release);
        child.observation = ObservationState::Ready;
        return result;
    }
    std::int32_t aevix_scoped_task_abort(std::uint64_t handle) noexcept
    {
        begin();
        ScopeLease lease;
        if (const auto result = pinScope(handle, lease, true))
            return result;
        auto &child = *lease.child;
        if (childSelf(child))
            return status(TaskRuntimeStatus::WouldDeadlock);
        std::lock_guard lock(child.consume);
        auto expected = ObservationState::Ready;
        if (!child.observation.compare_exchange_strong(expected, ObservationState::Busy))
            return status(TaskRuntimeStatus::InvalidToken);
        const auto result = aevix_task_abort(child.owner.load());
        if (!result)
        {
            child.settled = TaskPhase::Aborted;
            child.owner = 0;
            child.observation = ObservationState::Consumed;
            child.transaction.store(false, std::memory_order_release);
            retireObservation(child);
        }
        else
            child.observation = ObservationState::Ready;
        return result;
    }
    std::int32_t aevix_scoped_task_join(std::uint64_t handle, void *output, std::uint64_t size) noexcept
    {
        begin();
        return scopedTake(handle, output, size, false);
    }
    std::int32_t aevix_scoped_task_try_take(std::uint64_t handle, void *output, std::uint64_t size) noexcept
    {
        begin();
        return scopedTake(handle, output, size, true);
    }
    std::int32_t aevix_scoped_task_wait(std::uint64_t handle) noexcept
    {
        begin();
        ScopeLease lease;
        if (const auto result = pinScope(handle, lease, true))
            return result;
        return aevix_task_wait(lease.child->owner.load());
    }
    std::int32_t aevix_scoped_task_wait_timeout(std::uint64_t handle, std::uint64_t nanoseconds) noexcept
    {
        begin();
        ScopeLease lease;
        if (const auto result = pinScope(handle, lease, true))
            return result;
        return aevix_task_wait_timeout(lease.child->owner.load(), nanoseconds);
    }
    std::int32_t aevix_scoped_task_discard(std::uint64_t handle) noexcept
    {
        begin();
        ScopeLease lease;
        if (const auto result = pinScope(handle, lease, true))
            return result;
        auto &child = *lease.child;
        std::unique_lock lock(child.consume, std::try_to_lock);
        if (!lock.owns_lock())
            return status(TaskRuntimeStatus::NotReady);
        if (child.transaction.load(std::memory_order_acquire))
            return status(TaskRuntimeStatus::InvalidState);
        auto expected = ObservationState::Ready;
        if (!child.observation.compare_exchange_strong(expected, ObservationState::Discarded))
            return status(expected == ObservationState::Busy ? TaskRuntimeStatus::NotReady
                                                             : TaskRuntimeStatus::InvalidToken);
        child.discarded = true;
        retireObservation(child);
        return good;
    }
    std::int32_t aevix_scoped_task_request_cancel(std::uint64_t handle, std::uint32_t *disposition) noexcept
    {
        begin();
        ScopeLease lease;
        if (const auto result = pinScope(handle, lease, true))
            return result;
        return aevix_task_request_cancel(lease.child->owner.load(), disposition);
    }
    std::int32_t aevix_scoped_task_snapshot(std::uint64_t handle, aevix::TaskSnapshot *snapshot) noexcept
    {
        begin();
        ScopeLease lease;
        if (const auto result = pinScope(handle, lease, true))
            return result;
        return aevix_task_snapshot(lease.child->owner.load(), snapshot);
    }
    std::int32_t aevix_task_scope_close_admission(std::uint64_t owner) noexcept
    {
        begin();
        ScopeLease lease;
        if (const auto result = pinScope(owner, lease))
            return result;
        std::lock_guard admission(lease.scope->admission);
        lease.scope->closing = true;
        return good;
    }
    std::int32_t aevix_task_scope_active_count(std::uint64_t owner, std::uint64_t *output) noexcept
    {
        begin();
        if (!output)
            return status(TaskRuntimeStatus::InvalidInput);
        *output = 0;
        ScopeLease lease;
        if (const auto result = pinScope(owner, lease))
            return result;
        // 입장 잠금은 목록을 안정화한다. 자식이나 네이티브 소비 잠금으로 대기하지 않는다.
        // Admission stabilizes the child list; no child/native consumer lock is awaited.
        std::lock_guard admission(lease.scope->admission);
        std::uint64_t active = 0;
        for (auto *child = lease.scope->children; child; child = child->next)
        {
            const auto task = child->owner.load(std::memory_order_acquire);
            if (!task)
                continue;
            Lease native;
            const auto pinned = pin(task, native);
            // 동시에 수취·정리된 세대는 미완료 작업이 아니다. 다른 오류는 출력 없이 전달한다.
            // A concurrently consumed generation is not unfinished work; preserve other errors without partial output.
            if (pinned == status(TaskRuntimeStatus::InvalidToken))
                continue;
            if (pinned)
                return pinned;
            const auto phase = native.control->phase.load(std::memory_order_acquire);
            if (phase == TaskPhase::Prepared || phase == TaskPhase::Queued || phase == TaskPhase::Running)
            {
                if (active == UINT64_MAX)
                    return status(TaskRuntimeStatus::SizeOverflow);
                ++active;
            }
        }
        *output = active;
        return good;
    }
    std::int32_t aevix_task_scope_status(std::uint64_t owner, std::uint32_t *output) noexcept
    {
        begin();
        if (!output)
            return status(TaskRuntimeStatus::InvalidInput);
        *output = 0;
        ScopeLease lease;
        if (const auto result = pinScope(owner, lease))
            return result;
        std::lock_guard admission(lease.scope->admission);
        *output = std::uint32_t(lease.scope->closing ? aevix::TaskScopeState::Closing : aevix::TaskScopeState::Open);
        return good;
    }
    std::int32_t aevix_task_scope_snapshot(std::uint64_t owner, aevix::TaskScopeSnapshot *output) noexcept
    {
        begin();
        if (!output)
            return status(TaskRuntimeStatus::InvalidInput);
        *output = {};
        ScopeLease lease;
        if (const auto result = pinScope(owner, lease))
            return result;
        std::lock_guard admission(lease.scope->admission);
        output->accepted = lease.scope->accepted;
        output->closing = lease.scope->closing;
        for (auto *child = lease.scope->children; child; child = child->next)
        {
            const auto state = child->observation.load();
            if (state == ObservationState::Ready || state == ObservationState::Busy)
                ++output->live_handles;
        }
        return good;
    }
    std::int32_t aevix_task_scope_request_cancel(std::uint64_t owner, std::uint64_t *requested) noexcept
    {
        begin();
        if (!requested)
            return status(TaskRuntimeStatus::InvalidInput);
        *requested = 0;
        ScopeLease lease;
        if (const auto result = pinScope(owner, lease))
            return result;
        std::lock_guard admission(lease.scope->admission);
        for (auto *child = lease.scope->children; child; child = child->next)
            if (const auto task = child->owner.load())
            {
                std::uint32_t disposition = 0;
                const auto result = aevix_task_request_cancel(task, &disposition);
                if (result && result != status(TaskRuntimeStatus::InvalidToken))
                    return result;
                if (!result && disposition != 1)
                    ++*requested;
            }
        return good;
    }
    std::int32_t aevix_task_scope_close(std::uint64_t owner, aevix::TaskScopeReport *output) noexcept
    {
        begin();
        if (!output)
            return status(TaskRuntimeStatus::InvalidInput);
        *output = {};
        ScopeLease lease;
        if (const auto result = pinScope(owner, lease))
            return result;
        auto &scope = *lease.scope;
        if (reentrantClose(scope))
            return status(TaskRuntimeStatus::WouldDeadlock);
        bool active = false;
        if (!scope.closeActive.compare_exchange_strong(active, true))
            return status(TaskRuntimeStatus::NotReady);
        struct CloseGuard final
        {
            ScopeControl &scope;
            ClosingScope frame;
            explicit CloseGuard(ScopeControl &value) noexcept : scope(value), frame{&value, closingScopes}
            {
                closingScopes = &frame;
            }
            ~CloseGuard() noexcept
            {
                closingScopes = frame.parent;
                scope.closeActive.store(false, std::memory_order_release);
            }
        } closing(scope);
        {
            std::lock_guard admission(scope.admission);
            if (scopeSelf(scope))
                return status(TaskRuntimeStatus::WouldDeadlock);
            scope.closing = true;
        }
        for (auto *child = scope.children; child; child = child->next)
        {
            for (;;)
            {
                std::unique_lock lock(child->consume);
                if (child->transaction.load(std::memory_order_acquire))
                {
                    lock.unlock();
                    std::this_thread::yield();
                    continue;
                }
                const auto task = child->owner.load();
                if (!task)
                    break;
                child->observation = ObservationState::Consumed;
                retireObservation(*child);
                Lease nativeLease;
                const auto pinned = pin(task, nativeLease);
                if (pinned && pinned != status(TaskRuntimeStatus::InvalidToken))
                    return pinned;
                lock.unlock();
                // 관측은 이미 끝났고 스코프 닫기는 한 호출만 수행하므로 정리 중 어떤 소비 잠금도 필요 없다.
                // Observation is retired and only one close is active, so no consume lock is needed across user cleanup.
                const auto result = aevix_task_cleanup(task);
                if (result && result != status(TaskRuntimeStatus::InvalidToken))
                    return result;
                lock.lock();
                child->settled = nativeLease.control ? nativeLease.control->phase.load(std::memory_order_acquire)
                                                     : TaskPhase::Aborted;
                child->owner = 0;
                break;
            }
        }
        output->accepted = scope.accepted;
        for (auto *child = scope.children; child; child = child->next)
        {
            output->observed += child->observed.load();
            output->discarded += child->discarded.load();
            if (child->settled == TaskPhase::Completed)
                ++output->completed;
            else if (child->settled == TaskPhase::Cancelled)
                ++output->cancelled;
            else if (child->settled == TaskPhase::Failed)
                ++output->failed;
            else if (child->settled == TaskPhase::Aborted)
                ++output->aborted;
        }
        {
            std::lock_guard lock(scopeRegistry().mutex);
            scope.retired = true;
        }
        return good;
    }
    std::int32_t aevix_task_scope_cleanup(std::uint64_t owner) noexcept
    {
        if (!owner)
            return good;
        aevix::TaskScopeReport report;
        return aevix_task_scope_close(owner, &report);
    }
    std::int32_t aevix_task_scope_runtime_drain() noexcept
    {
        begin();
        if (callbackDepth)
            return status(TaskRuntimeStatus::WouldDeadlock);
        {
            std::lock_guard lock(scopeRegistry().mutex);
            scopeRegistry().closing = true;
        }
        for (;;)
        {
            std::uint64_t owner = 0;
            bool retained = false;
            {
                std::lock_guard lock(scopeRegistry().mutex);
                for (auto *entry = scopeRegistry().scopes; entry; entry = entry->next)
                    if (entry->scope)
                    {
                        retained = true;
                        if (!entry->scope->retired)
                        {
                            owner = scopeToken(*entry, scopeTag);
                            break;
                        }
                    }
            }
            if (owner)
            {
                const auto result = aevix_task_scope_cleanup(owner);
                if (result && result != status(TaskRuntimeStatus::InvalidToken) &&
                    result != status(TaskRuntimeStatus::NotReady))
                    return result;
            }
            else if (!retained)
                return aevix_task_runtime_drain();
            std::this_thread::yield();
        }
    }
#if defined(AEVIX_TASK_RUNTIME_TESTING)
    std::uint64_t aevix_task_scope_test_live_scopes() noexcept
    {
        std::lock_guard lock(scopeRegistry().mutex);
        std::uint64_t count = 0;
        for (auto *entry = scopeRegistry().scopes; entry; entry = entry->next)
            if (entry->scope)
                ++count;
        return count;
    }
    std::uint32_t aevix_task_scope_test_is_reaping(std::uint64_t owner) noexcept
    {
        ScopeLease lease;
        if (pinScope(owner, lease))
            return 0;
        std::lock_guard admission(lease.scope->admission);
        for (auto *child = lease.scope->children; child; child = child->next)
        {
            Lease nativeLease;
            if (!pin(child->owner.load(), nativeLease) && nativeLease.control->waitingNativeReap.load())
                return 1;
        }
        return 0;
    }
    std::uint64_t aevix_task_scope_test_live_handles() noexcept
    {
        std::lock_guard lock(scopeRegistry().mutex);
        std::uint64_t count = 0;
        for (auto *entry = scopeRegistry().handles; entry; entry = entry->next)
            if (entry->child)
                ++count;
        return count;
    }
#endif
}

extern "C"
{
    std::int32_t aevix_cancel_source_new(std::uint64_t *output) noexcept
    {
        begin();
        if (!output)
            return status(TaskRuntimeStatus::InvalidInput);
        *output = 0;
        CancellationState *state = nullptr;
        if (const auto result = createCancellationState(state))
            return result;
        const auto result = publishCancellation(state, false, false, output);
        if (result)
            releaseCancellationState(state);
        return result;
    }
    std::int32_t aevix_cancel_source_clone(std::uint64_t owner, std::uint64_t *output) noexcept
    {
        begin();
        return cloneCancellation(owner, false, false, output);
    }
    std::int32_t aevix_cancel_token_from_source(std::uint64_t owner, std::uint64_t *output) noexcept
    {
        begin();
        return cloneCancellation(owner, false, true, output);
    }
    std::int32_t aevix_cancel_token_clone(std::uint64_t owner, std::uint64_t *output) noexcept
    {
        begin();
        return cloneCancellation(owner, true, true, output);
    }
    std::int32_t aevix_cancel_source_request(std::uint64_t owner, std::uint32_t *already) noexcept
    {
        begin();
        if (!already)
            return status(TaskRuntimeStatus::InvalidInput);
        *already = 0;
        std::lock_guard lock(cancelRegistry().mutex);
        auto *entry = lookupCancellation(owner, false);
        if (!entry)
            return status(TaskRuntimeStatus::InvalidToken);
        *already = entry->state->requested.exchange(true, std::memory_order_acq_rel);
        return good;
    }
    std::int32_t aevix_cancel_token_requested(std::uint64_t owner, std::uint32_t *requested) noexcept
    {
        begin();
        if (!requested)
            return status(TaskRuntimeStatus::InvalidInput);
        *requested = 0;
        std::lock_guard lock(cancelRegistry().mutex);
        const auto *entry = lookupCancellation(owner, true);
        if (!entry)
            return status(TaskRuntimeStatus::InvalidToken);
        *requested = entry->state->requested.load(std::memory_order_acquire);
        return good;
    }
    std::int32_t aevix_cancel_source_release(std::uint64_t owner) noexcept
    {
        begin();
        return releaseCancellationOwner(owner, false);
    }
    std::int32_t aevix_cancel_token_release(std::uint64_t owner) noexcept
    {
        begin();
        return releaseCancellationOwner(owner, true);
    }
    std::int32_t aevix_task_cancel_token(std::uint64_t owner, std::uint64_t *output) noexcept
    {
        begin();
        if (!output)
            return status(TaskRuntimeStatus::InvalidInput);
        *output = 0;
        Lease lease;
        if (const auto result = pin(owner, lease))
            return result;
        return publishCancellation(lease.control->cancellation, true, true, output);
    }
    std::int32_t aevix_scoped_task_cancel_token(std::uint64_t handle, std::uint64_t *output) noexcept
    {
        begin();
        if (!output)
            return status(TaskRuntimeStatus::InvalidInput);
        *output = 0;
        ScopeLease lease;
        if (const auto result = pinScope(handle, lease, true))
            return result;
        // 스코프 pin만으로는 결과를 동시에 소비하는 작업 Control을 유지할 수 없다.
        // A Scope pin alone cannot keep a Task control alive during concurrent result consumption.
        return aevix_task_cancel_token(lease.child->owner.load(), output);
    }
#if defined(AEVIX_TASK_RUNTIME_TESTING)
    void aevix_cancel_test_owner_limit(std::uint64_t limit) noexcept
    {
        cancelOwnerLimit.store(limit, std::memory_order_relaxed);
    }
    std::uint64_t aevix_cancel_test_live_states() noexcept
    {
        std::lock_guard lock(cancelRegistry().mutex);
        return cancelRegistry().liveStates;
    }
    std::uint64_t aevix_cancel_test_owner_count(std::uint64_t owner) noexcept
    {
        std::lock_guard lock(cancelRegistry().mutex);
        auto *entry = lookupCancellation(owner, true);
        if (!entry)
            entry = lookupCancellation(owner, false);
        return entry ? entry->state->owners : 0;
    }
#endif
}
