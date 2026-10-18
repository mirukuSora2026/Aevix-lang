// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_task_ready_runtime_tests.cpp
//
// Description :
//     작업자 없는 Ready의 준비·확정, 실패 원자성, 정확한 정리와 스코프 대기를 검증한다.
//     Verify workerless Ready preparation/publication, failure atomicity, exact cleanup and scope waiting.
//
// ========================
#include "aevix/Runtime/TaskScopeRuntime.hpp"
#include "aevix/Runtime/CancellationRuntime.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <thread>
#include <array>

namespace
{
    unsigned checks=0, failures=0;
    void expect(bool good,const char* name) { ++checks; if(!good){++failures;std::fprintf(stderr,"FAIL: %s\n",name);} }
    struct alignas(64) Owned { std::atomic<unsigned>* dropped; int* value; };
    void drop(void* raw) noexcept
    { auto& owned=*static_cast<Owned*>(raw); ++*owned.dropped; delete owned.value; }
    std::int32_t prepare(std::uint64_t scope,std::uint64_t& owner,void*& storage,std::uint64_t& identity)
    { return aevix_task_scope_prepare_ready(scope,sizeof(Owned),alignof(Owned),drop,&owner,&storage,&identity); }
    void empty()
    {
        expect(!aevix_task_test_live_controls(),"no leaked controls");
        expect(!aevix_task_scope_test_live_handles(),"no leaked observations");
        expect(!aevix_task_scope_test_live_scopes(),"no leaked scopes");
        expect(!aevix_cancel_test_live_states(),"no leaked cancellation state");
    }
    void prepareCommit()
    {
        std::uint64_t scope=0,owner=0,id=0,active=99;void* storage=nullptr;
        std::atomic<unsigned> dropped{0};
        expect(aevix_task_scope_open(&scope)==0,"open ready scope");
        expect(prepare(scope,owner,storage,id)==0 && owner && id && storage && std::uintptr_t(storage)%64==0,"aligned Ready preparation returns identity/storage");
        expect(aevix_task_scope_active_count(scope,&active)==0 && active==1,"prepared Ready is active");
        expect(aevix_scoped_task_wait(owner)==5,"uncommitted Ready cannot be observed");
        expect(aevix_scoped_task_commit(owner)==5,"threaded commit rejects Ready without consuming transaction");
        std::uint32_t disposition=99;
        expect(aevix_scoped_task_request_cancel(owner,&disposition)==0 && disposition==0,"prepared cancellation is only a request");
        expect(aevix_task_scope_close_admission(scope)==0,"close admission after preparing Ready");
        std::uint64_t rejected=99,rejectedId=99;void* rejectedStorage=reinterpret_cast<void*>(1);
        expect(prepare(scope,rejected,rejectedStorage,rejectedId)==5 && !rejected && !rejectedId && !rejectedStorage,"closed admission rejects new Ready atomically");
        new(storage) Owned{&dropped,new int(42)};
        aevix_task_test_fail_after(0);
        const auto committed=aevix_scoped_task_ready_commit(owner);
        aevix_task_test_fail_after(UINT64_MAX);
        expect(committed==0,"valid Ready commit cannot allocate/fail after admission closure");
        expect(aevix_task_scope_active_count(scope,&active)==0 && !active,"committed Ready is terminal");
        aevix::TaskSnapshot state{};
        expect(aevix_scoped_task_snapshot(owner,&state)==0 && state.phase==3 && state.cancelRequested==1,"precomputed Ready remains Completed after cancellation request");
        expect(aevix_scoped_task_request_cancel(owner,&disposition)==0 && disposition==1,"terminal Ready rejects a new cancellation transition");
        expect(aevix_scoped_task_wait_timeout(owner,0)==0,"zero-timeout wait sees terminal Ready");
        Owned result{};
        expect(aevix_scoped_task_join(owner,&result,sizeof(result)-1)==3,"wrong output size preserves Ready observation");
        expect(aevix_scoped_task_join(owner,&result,sizeof(result))==0 && *result.value==42 && !dropped,"join moves Ready value without dropping it");
        drop(&result);expect(dropped==1,"caller owns exactly one moved result");
        aevix::TaskScopeReport report{};
        expect(aevix_task_scope_close(scope,&report)==0 && report.accepted==1 && report.observed==1 && report.completed==1 && !report.cancelled,"Ready reports Completed independent of cancellation request");
        empty();
    }
    void droppedPaths()
    {
        std::uint64_t scope=0;expect(aevix_task_scope_open(&scope)==0,"open cleanup scope");
        std::atomic<unsigned> dropped{0};
        for(unsigned path=0;path<4;++path)
        {
            std::uint64_t owner=0,id=0;void* storage=nullptr;
            expect(prepare(scope,owner,storage,id)==0,"prepare cleanup Ready");
            new(storage) Owned{&dropped,new int(int(path))};
            expect(aevix_scoped_task_ready_commit(owner)==0,"commit cleanup Ready");
            if(path==0) { Owned result{};expect(aevix_scoped_task_try_take(owner,&result,sizeof(result))==0 && *result.value==0,"terminal Ready try_take moves once");drop(&result); }
            if(path==1) expect(aevix_scoped_task_discard(owner)==0,"discard retains scope cleanup obligation");
            if(path==2) expect(aevix_scoped_task_wait(owner)==0,"wait retains unobserved Ready result");
        }
        expect(dropped==1,"only observed caller value dropped before close");
        aevix::TaskScopeReport report{};
        expect(aevix_task_scope_close(scope,&report)==0 && report.accepted==4 && report.completed==4 && report.observed==1,"scope closes all workerless results");
        expect(dropped==4,"each unclaimed Ready result drops exactly once");empty();
    }
    void failuresAndAbort()
    {
        std::uint64_t scope=0;expect(aevix_task_scope_open(&scope)==0,"open fault scope");
        unsigned failed=0,succeeded=0;
        for(unsigned budget=0;budget<16;++budget)
        {
            std::uint64_t owner=99,id=99;void* storage=reinterpret_cast<void*>(1);
            aevix_task_test_fail_after(budget);
            const auto status=prepare(scope,owner,storage,id);
            aevix_task_test_fail_after(UINT64_MAX);
            if(status==0) { ++succeeded;expect(aevix_scoped_task_abort(owner)==0,"abort untouched Ready preparation"); }
            else { ++failed;expect(status==1 && !owner && !id && !storage,"allocation failure publishes no partial Ready"); }
            expect(!aevix_task_test_live_controls() && !aevix_task_scope_test_live_handles(),"prepare failure/abort releases all native state");
        }
        expect(failed>=4 && succeeded,"exercise reusable control allocation failures and successful rollback");
        std::uint64_t owner=99,id=99;void* storage=reinterpret_cast<void*>(1);
        expect(aevix_task_scope_prepare_ready(scope,UINT64_MAX,1,drop,&owner,&storage,&id)==2 && !owner && !id && !storage,"overflow failure is atomic");
        expect(aevix_task_scope_prepare_ready(scope,0,0,nullptr,&owner,&storage,&id)==3 && !owner && !id && !storage,"invalid alignment is atomic");
        expect(aevix_task_scope_prepare_ready(scope,0,1,nullptr,&owner,&storage,&id)==0 && storage && id,"zero-size Ready has valid storage and identity");
        expect(aevix_scoped_task_ready_commit(owner)==0,"zero-size Ready commits");
        std::byte unused{};expect(aevix_scoped_task_try_take(owner,&unused,0)==0,"zero-size Ready can be observed");
        aevix::TaskScopeReport report{};
        expect(aevix_task_scope_close(scope,&report)==0 && report.accepted==succeeded+1 && report.aborted==succeeded && report.observed==1,"failed prepare never increments accepted");
        empty();
    }
    void closeWaitsForPublication()
    {
        std::uint64_t scope=0,owner=0,id=0;void* storage=nullptr;std::atomic<unsigned>dropped{0};
        expect(aevix_task_scope_open(&scope)==0 && prepare(scope,owner,storage,id)==0,"prepare before concurrent scope close");
        std::atomic<bool> started{false},finished{false};std::int32_t closed=99;aevix::TaskScopeReport report{};
        std::thread closer([&]{started=true;closed=aevix_task_scope_close(scope,&report);finished=true;});
        while(!started.load()) std::this_thread::yield();
        std::uint32_t status=0;while(aevix_task_scope_status(scope,&status)==0 && !status) std::this_thread::yield();
        expect(!finished.load(),"close waits for prepared Ready transaction");
        new(storage) Owned{&dropped,new int(42)};
        expect(aevix_scoped_task_ready_commit(owner)==0,"existing Ready can publish during close");
        closer.join();
        expect(closed==0 && report.completed==1 && dropped==1,"concurrent close drains exactly one published Ready result");empty();
    }
}
int main()
{
    // 어떤 Ready 경로도 생성·회수 실패에 의존하거나 숨은 작업자를 만들면 안 된다.
    // No Ready path may depend on thread creation/reaping or create a hidden worker.
    aevix_task_test_fail_threads(1);aevix_task_test_fail_reap(3);aevix_task_test_hold_worker_tail(1);
#if defined(AEVIX_TASK_TEST_ID_MAX)
    static_assert(AEVIX_TASK_TEST_ID_MAX==1);
    std::uint64_t scope=0,owner=0,id=0;void* storage=nullptr;
    expect(aevix_task_scope_open(&scope)==0,"open identity-exhaustion scope");
    expect(aevix_task_scope_prepare_ready(scope,0,1,nullptr,&owner,&storage,&id)==0 && id==1,"Ready issues last serial once");
    expect(aevix_scoped_task_abort(owner)==0,"aborting Ready does not recycle serial");
    for(unsigned retry=0;retry<3;++retry) {
        owner=99;id=99;storage=reinterpret_cast<void*>(1);
        expect(aevix_task_scope_prepare_ready(scope,0,1,nullptr,&owner,&storage,&id)==2 && !owner && !storage && !id,"Ready exhaustion is atomic and never wraps");
    }
    aevix::TaskScopeReport report{};
    expect(aevix_task_scope_close(scope,&report)==0 && report.accepted==1 && report.aborted==1,"rejected exhausted Ready never enters scope");
    empty();
#else
    prepareCommit();droppedPaths();failuresAndAbort();closeWaitsForPublication();
#endif
    expect(aevix_task_test_thread_creations()==0,"Ready creates zero OS workers");
    expect(aevix_task_test_native_reaps()==0,"Ready performs zero native reap operations");
    aevix_task_test_hold_worker_tail(0);
    expect(aevix_task_scope_runtime_drain()==0,"workerless final drain completes");
    std::printf("%u checks, %u failures; zero OS worker creations/reaps\n",checks,failures);
    return failures?1:0;
}
