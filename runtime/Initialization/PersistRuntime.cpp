// ========================
//
// Created By : miruku_sora2026
//
// File name : PersistRuntime.cpp
//
// Description :
//     스레드 안전 일회 초기화, 의존 순환 감지와 완료 역순 정리를 구현한다.
//     Implement thread-safe once initialization, dependency-cycle detection, and reverse-completion cleanup.
//
// ========================
#include "aevix/Runtime/PersistRuntime.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

namespace
{
#if defined(_WIN32)
    using ThreadId = DWORD;
    ThreadId currentThread() { return GetCurrentThreadId(); }
    bool sameThread(ThreadId left, ThreadId right) { return left == right; }
#else
    using ThreadId = pthread_t;
    ThreadId currentThread() { return pthread_self(); }
    bool sameThread(ThreadId left, ThreadId right) { return pthread_equal(left, right) != 0; }
#endif
    enum class Phase { Running, Draining, Cleaning, Closed };
    enum class State { Empty, Initializing, Ready, Cleaned };
    struct Domain;
    struct Guard final
    {
        Domain* domain = nullptr;
        State state = State::Empty;
        ThreadId owner{};
        Guard* waitingFor = nullptr;
        Guard* nextAllocated = nullptr;
        Guard* previousCompleted = nullptr;
        const char* name = nullptr;
        AevixPersistCallback cleanup = nullptr;
    };
    struct Domain final
    {
        Phase phase = Phase::Running;
        std::size_t activeInitializations = 0;
        Guard* allocated = nullptr;
        Guard* completed = nullptr;
        Domain* next = nullptr;
        ThreadId shutdownOwner{};
    };
    struct Frame final
    {
        Guard* guard;
        Frame* previous;
    };
#if defined(_WIN32)
    // 네이티브 Windows 동기화로 생성 프로그램의 pthread 의존성을 피한다.
    // Use native Windows synchronization without a pthread dependency in generated programs.
    SRWLOCK mutex = SRWLOCK_INIT;
    CONDITION_VARIABLE changed = CONDITION_VARIABLE_INIT;
#else
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
#endif
    Domain* domains = nullptr;
    bool registeredExitCleanup = false;
#if defined(_WIN32)
    // 종료 함수가 실행되는 동안에도 유효한 프로세스 수명의 TLS 슬롯을 사용한다.
    // Keep native TLS slots valid for the process lifetime, including native finalizers.
    INIT_ONCE tlsReady = INIT_ONCE_STATIC_INIT;
    DWORD activeSlot = TLS_OUT_OF_INDEXES;
    DWORD cleaningSlot = TLS_OUT_OF_INDEXES;
    [[noreturn]] void fail(const char* reason, const char* name);
    BOOL CALLBACK initializeTls(PINIT_ONCE, PVOID, PVOID*)
    {
        activeSlot = TlsAlloc();
        cleaningSlot = TlsAlloc();
        if (activeSlot != TLS_OUT_OF_INDEXES && cleaningSlot != TLS_OUT_OF_INDEXES) return TRUE;
        if (activeSlot != TLS_OUT_OF_INDEXES) TlsFree(activeSlot);
        if (cleaningSlot != TLS_OUT_OF_INDEXES) TlsFree(cleaningSlot);
        return FALSE;
    }
    void prepareTls()
    {
        if (!InitOnceExecuteOnce(&tlsReady, initializeTls, nullptr, nullptr)) fail("cannot allocate initialization TLS slots", nullptr);
    }
    Frame* activeFrame() { prepareTls(); return static_cast<Frame*>(TlsGetValue(activeSlot)); }
    Domain* cleaningDomain() { prepareTls(); return static_cast<Domain*>(TlsGetValue(cleaningSlot)); }
    void setActiveFrame(Frame* frame)
    {
        prepareTls();
        if (!TlsSetValue(activeSlot, frame)) fail("cannot update initialization TLS slot", nullptr);
    }
    void setCleaningDomain(Domain* domain)
    {
        prepareTls();
        if (!TlsSetValue(cleaningSlot, domain)) fail("cannot update cleanup TLS slot", nullptr);
    }
#else
    thread_local Frame* active = nullptr;
    thread_local Domain* cleaning = nullptr;
    Frame* activeFrame() { return active; }
    Domain* cleaningDomain() { return cleaning; }
    void setActiveFrame(Frame* frame) { active = frame; }
    void setCleaningDomain(Domain* domain) { cleaning = domain; }
#endif

    void writeText(const char* text)
    {
        if (!text) return;
        std::size_t remaining = std::strlen(text);
        while (remaining)
        {
#if defined(_WIN32)
            DWORD written = 0;
            const auto amount = static_cast<DWORD>(remaining > MAXDWORD ? MAXDWORD : remaining);
            if (!WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, amount, &written, nullptr) || written == 0) return;
#else
            const auto written = ::write(STDERR_FILENO, text, remaining);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return;
#endif
            text += written;
            remaining -= static_cast<std::size_t>(written);
        }
    }
    [[noreturn]] void fail(const char* reason, const char* name = nullptr)
    {
        writeText("Aevix persist initialization error: ");
        writeText(reason);
        if (name) { writeText(" ["); writeText(name); writeText("]"); }
        writeText("\n");
        std::abort();
    }
    void lock()
    {
#if defined(_WIN32)
        AcquireSRWLockExclusive(&mutex);
#else
        if (pthread_mutex_lock(&mutex) != 0) fail("cannot lock initialization registry");
#endif
    }
    void unlock()
    {
#if defined(_WIN32)
        ReleaseSRWLockExclusive(&mutex);
#else
        if (pthread_mutex_unlock(&mutex) != 0) fail("cannot unlock initialization registry");
#endif
    }
    void wait()
    {
#if defined(_WIN32)
        if (!SleepConditionVariableSRW(&changed, &mutex, INFINITE, 0)) fail("cannot wait for initialization");
#else
        if (pthread_cond_wait(&changed, &mutex) != 0) fail("cannot wait for initialization");
#endif
    }
    void notify()
    {
#if defined(_WIN32)
        WakeAllConditionVariable(&changed);
#else
        if (pthread_cond_broadcast(&changed) != 0) fail("cannot notify initialization waiters");
#endif
    }
    void freeMetadata()
    {
        // 코드 모듈이 이미 내려갔을 수 있으므로 슬롯이나 콜백에는 접근하지 않는다.
        // Modules may already be unloaded, so metadata cleanup never touches slots or callbacks.
        for (auto* domain = domains; domain; domain = domain->next)
        {
            if (domain->phase == Phase::Running)
            {
                void* slot = domain;
                aevix_persist_shutdown(&slot);
            }
        }
        for (auto* domain = domains; domain; domain = domain->next)
        {
            for (auto* guard = domain->allocated; guard;)
            {
                auto* following = guard->nextAllocated;
                std::free(guard);
                guard = following;
            }
            domain->allocated = nullptr;
            domain->completed = nullptr;
        }
        // 모듈의 native 종료 함수가 atexit 뒤에도 슬롯을 읽을 수 있으므로 닫힌 도메인은 유지한다.
        // Keep closed domain sentinels alive because native module finalizers may read slots after atexit.
    }
    Domain* domainFor(void** slot)
    {
        if (!slot) fail("missing module registry slot");
        if (*slot) return static_cast<Domain*>(*slot);
        auto* domain = static_cast<Domain*>(std::malloc(sizeof(Domain)));
        if (!domain) fail("cannot allocate initialization registry");
        new (domain) Domain{};
        domain->next = domains;
        domains = domain;
        *slot = domain;
        if (!registeredExitCleanup)
        {
            if (std::atexit(freeMetadata) != 0) fail("cannot register registry cleanup");
            registeredExitCleanup = true;
        }
        return domain;
    }
    Guard* guardFor(Domain* domain, void** slot, const char* name, AevixPersistCallback cleanup)
    {
        if (!slot) fail("missing persist guard slot", name);
        if (*slot)
        {
            auto* guard = static_cast<Guard*>(*slot);
            if (guard->domain != domain) fail("guard belongs to a different module", name);
            return guard;
        }
        auto* guard = static_cast<Guard*>(std::malloc(sizeof(Guard)));
        if (!guard) fail("cannot allocate persist guard", name);
        new (guard) Guard{};
        guard->domain = domain;
        guard->name = name;
        guard->cleanup = cleanup;
        guard->nextAllocated = domain->allocated;
        domain->allocated = guard;
        *slot = guard;
        return guard;
    }
}

extern "C" void aevix_persist_ensure(void** domainSlot, void** guardSlot, const char* name,
                                      AevixPersistCallback initialize, AevixPersistCallback cleanup)
{
    if (cleaningDomain()) fail("persist access from cleanup is forbidden", name);
    lock();
    auto* domain = domainFor(domainSlot);
    auto* parentFrame = activeFrame();
    const bool nestedDuringDrain = parentFrame && parentFrame->guard->domain == domain;
    if (domain->phase == Phase::Cleaning || domain->phase == Phase::Closed
        || (domain->phase == Phase::Draining && !nestedDuringDrain))
        fail("persist access is forbidden during or after module cleanup", name);
    auto* guard = guardFor(domain, guardSlot, name, cleanup);
    auto* parent = parentFrame ? parentFrame->guard : nullptr;
    while (guard->state == State::Initializing)
    {
        if (sameThread(guard->owner, currentThread())) fail("cyclic initialization on the current thread", name);
        if (parent)
        {
            parent->waitingFor = guard;
            for (auto* dependency = guard; dependency; dependency = dependency->waitingFor)
                if (dependency == parent) fail("cyclic initialization across threads", name);
        }
        wait();
        if (parent) parent->waitingFor = nullptr;
    }
    if (guard->state == State::Ready) { unlock(); return; }
    if (guard->state == State::Cleaned) fail("persist storage was already cleaned", name);
    if (!initialize) fail("missing initializer callback", name);
    guard->state = State::Initializing;
    guard->owner = currentThread();
    if (parent) parent->waitingFor = guard;
    ++domain->activeInitializations;
    unlock();

    Frame frame{guard, parentFrame};
    setActiveFrame(&frame);
    initialize();
    setActiveFrame(frame.previous);

    lock();
    guard->state = State::Ready;
    guard->waitingFor = nullptr;
    guard->previousCompleted = domain->completed;
    domain->completed = guard;
    --domain->activeInitializations;
    if (parent) parent->waitingFor = nullptr;
    notify();
    unlock();
}

extern "C" void aevix_persist_shutdown(void** domainSlot)
{
    if (!domainSlot) fail("missing module registry slot");
    if (cleaningDomain()) fail("nested module shutdown from cleanup is forbidden");
    lock();
    if (!*domainSlot) { unlock(); return; }
    auto* domain = static_cast<Domain*>(*domainSlot);
    if (domain->phase == Phase::Closed) { unlock(); return; }
    auto* frame = activeFrame();
    if (frame && frame->guard->domain == domain) fail("cannot shut down a module from its initializer");
    if (domain->phase != Phase::Running)
    {
        if (sameThread(domain->shutdownOwner, currentThread())) fail("reentrant module shutdown");
        // 다른 종료 담당자가 정리를 끝낼 때까지 기다린다.
        // Wait until the thread already responsible for shutdown finishes cleanup.
        while (domain->phase != Phase::Closed) wait();
        unlock(); return;
    }
    domain->shutdownOwner = currentThread();
    domain->phase = Phase::Draining;
    while (domain->activeInitializations) wait();
    domain->phase = Phase::Cleaning;
    auto* next = domain->completed;
    unlock();
    while (next)
    {
        setCleaningDomain(domain);
        if (next->cleanup) next->cleanup();
        setCleaningDomain(nullptr);
        lock();
        next->state = State::Cleaned;
        next = next->previousCompleted;
        unlock();
    }
    lock();
    domain->phase = Phase::Closed;
    notify();
    unlock();
}
