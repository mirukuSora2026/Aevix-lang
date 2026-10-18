// ========================
//
// Created By : miruku_sora2026
//
// File name : v03_task_identity_runtime_tests.cpp
//
// Description :
//     진단 번호의 유일성, 실패 원자성, 병행 생성과 소진을 독립 네이티브 경계에서 검증한다.
//     Verify diagnostic uniqueness, failure atomicity, concurrent creation, and exhaustion at the native boundary.
//
// ========================
#include "aevix/Runtime/TaskRuntime.hpp"
#include "aevix/Runtime/TaskScopeRuntime.hpp"
#include "aevix/Runtime/CancellationRuntime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#if !defined(AEVIX_TASK_RUNTIME_TESTING)
#error "This dedicated test requires AEVIX_TASK_RUNTIME_TESTING."
#endif
namespace
{
    unsigned checks = 0, failures = 0;
    void expect(bool valid, const char* name)
    { ++checks; if (!valid) { ++failures; std::fprintf(stderr,"FAIL: %s\n",name); } }
    std::int32_t invoke(void*, void* result, const aevix::TaskContext*) noexcept
    { *static_cast<std::int32_t*>(result) = 42; return 0; }
    std::int32_t prepare(std::uint64_t& owner, void*& capture, std::uint64_t& serial)
    { return aevix_task_prepare_identified(0,1,4,4,invoke,nullptr,nullptr,&owner,&capture,&serial); }
    void clean()
    {
        expect(aevix_task_test_live_controls()==0,"all native controls released");
        expect(aevix_task_scope_test_live_handles()==0,"all scoped observations released");
        expect(aevix_task_scope_test_live_scopes()==0,"all scopes released");
        expect(aevix_cancel_test_live_states()==0,"all cancellation states released");
    }
    void invalidOutputs()
    {
        std::uint64_t owner = 99, serial = 99; void* storage = reinterpret_cast<void*>(1);
        expect(aevix_task_prepare_identified(0,1,4,4,invoke,nullptr,nullptr,&owner,&storage,nullptr)==3
            && !owner && !storage,"missing identity output fails atomically");
        expect(aevix_task_prepare_identified(0,1,4,4,invoke,nullptr,nullptr,&owner,&storage,&owner)==3
            && !owner && !storage,"identity cannot overlap owner output");
        alignas(8) std::array<std::uint64_t,2> shared{99,99};
        auto* sameStorage = reinterpret_cast<void**>(shared.data());
        expect(aevix_task_prepare_identified(0,1,4,4,invoke,nullptr,nullptr,shared.data(),sameStorage,&serial)==3
            && !shared[0] && !serial,"capture pointer cannot overlap owner output");
        expect(aevix_task_prepare_identified(0,0,4,4,invoke,nullptr,nullptr,&owner,&storage,&serial)==3
            && !owner && !storage && !serial,"invalid alignment clears identity");
        aevix_task_test_fail_after(0);
        const auto failed = prepare(owner,storage,serial);
        aevix_task_test_fail_after(UINT64_MAX);
        expect(failed==1 && !owner && !storage && !serial,"allocation failure publishes no identity");
        clean();
    }
#if defined(AEVIX_TASK_TEST_ID_MAX)
    void exhausted()
    {
        static_assert(AEVIX_TASK_TEST_ID_MAX==3,"This boundary fixture uses exactly three issued identities.");
        std::uint64_t owner = 0, serial = 0, scope = 0; void* storage = nullptr;
        expect(prepare(owner,storage,serial)==0 && serial==1,"first identity issued");
        expect(aevix_task_abort(owner)==0,"first preparation aborted");
        aevix_task_test_fail_threads(1);
        expect(prepare(owner,storage,serial)==6 && !owner && !storage && !serial,"failed native thread burns second identity");
        aevix_task_test_fail_threads(0);
        expect(aevix_task_scope_open(&scope)==0,"scope remains constructible before identity exhaustion");
        expect(aevix_task_scope_prepare_identified(scope,0,1,4,4,invoke,nullptr,nullptr,&owner,&storage,&serial)==0
            && serial==3,"last identity issued exactly once");
        expect(aevix_scoped_task_abort(owner)==0,"last identity observation aborted");
        std::uint64_t active=99; expect(aevix_task_scope_active_count(scope,&active)==0 && active==0,"aborted final identity is inactive");
        for (unsigned attempt=0; attempt<3; ++attempt)
        {
            owner=99; serial=99; storage=reinterpret_cast<void*>(1);
            expect(aevix_task_scope_prepare_identified(scope,0,1,4,4,invoke,nullptr,nullptr,&owner,&storage,&serial)==2
                && !owner && !serial && !storage,"exhausted scope preparation cannot wrap or publish outputs");
        }
        aevix::TaskScopeReport report{};
        expect(aevix_task_scope_close(scope,&report)==0 && report.accepted==1 && report.aborted==1,"rejected exhaustion never increments accepted");
        owner=99; storage=reinterpret_cast<void*>(1);
        expect(aevix_task_prepare(0,1,4,4,invoke,nullptr,nullptr,&owner,&storage)==2 && !owner && !storage,
            "legacy ABI shares exhausted serial domain");
        clean();
    }
#else
    void sequential()
    {
        std::uint64_t owner=0, serial=0; void* storage=nullptr;
        expect(prepare(owner,storage,serial)==0 && owner && storage && serial,"identity is nonzero");
        const auto first=serial, firstOwner=owner;
        expect(aevix_task_commit(owner)==0,"identified preparation commits");
        std::int32_t result=0;
        expect(aevix_task_join(owner,&result,4)==0 && result==42 && serial==first,"caller identity survives terminal observation");
        aevix_task_test_fail_threads(1);
        expect(prepare(owner,storage,serial)==6 && !owner && !storage && !serial,"failed thread publishes no diagnostic identity");
        aevix_task_test_fail_threads(0);
        expect(prepare(owner,storage,serial)==0 && serial>=first+2 && owner!=firstOwner,"failed creation cannot recycle diagnostic serial");
        const auto second=serial;
        expect(aevix_task_abort(owner)==0,"abort prepared observation");
        expect(aevix_task_prepare(0,1,4,4,invoke,nullptr,nullptr,&owner,&storage)==0,"legacy preparation preserved");
        expect(aevix_task_abort(owner)==0,"legacy preparation abort preserved");
        expect(prepare(owner,storage,serial)==0 && serial>=second+2,"legacy ABI participates in one diagnostic domain");
        expect(aevix_task_abort(owner)==0,"latest observation aborted");
        std::uint64_t previous=serial;
        for (unsigned attempt=0;attempt<4;++attempt)
        {
            std::uint64_t scope=0; expect(aevix_task_scope_open(&scope)==0,"new scope created");
            expect(aevix_task_scope_prepare_identified(scope,0,1,4,4,invoke,nullptr,nullptr,&owner,&storage,&serial)==0
                && serial>previous,"scope changes do not reset identity");
            previous=serial;
            expect(aevix_scoped_task_commit(owner)==0,"scoped identity commits");
            expect(aevix_scoped_task_join(owner,&result,4)==0 && result==42,"scoped observation consumed");
            aevix::TaskScopeReport report{};
            expect(aevix_task_scope_close(scope,&report)==0 && report.accepted==1 && report.observed==1,
                "identity metadata preserves scope report");
        }
        clean();
    }
    void concurrent()
    {
        constexpr std::size_t creators=6, tasks=12;
        std::array<std::uint64_t,creators*tasks> identities{};
        std::array<std::thread,creators> workers;
        std::atomic<unsigned> bad{0};
        for (std::size_t creator=0;creator<creators;++creator) workers[creator]=std::thread([&,creator] {
            std::uint64_t scope=0;
            if (aevix_task_scope_open(&scope)) { ++bad; return; }
            for (std::size_t task=0;task<tasks;++task)
            {
                std::uint64_t owner=0, serial=0; void* storage=nullptr; std::int32_t result=0;
                if (aevix_task_scope_prepare_identified(scope,0,1,4,4,invoke,nullptr,nullptr,&owner,&storage,&serial)
                    || !serial || aevix_scoped_task_commit(owner) || aevix_scoped_task_join(owner,&result,4) || result!=42) { ++bad; break; }
                identities[creator*tasks+task]=serial;
            }
            aevix::TaskScopeReport report{};
            if (aevix_task_scope_close(scope,&report) || report.accepted!=tasks || report.observed!=tasks) ++bad;
        });
        for (auto& worker:workers) worker.join();
        expect(!bad,"concurrent scopes preserve construction and result ownership");
        std::sort(identities.begin(),identities.end());
        expect(identities.front()!=0 && std::adjacent_find(identities.begin(),identities.end())==identities.end(),
            "concurrent identities are globally unique in the runtime instance");
        clean();
    }
#endif
}
int main()
{
    invalidOutputs();
#if defined(AEVIX_TASK_TEST_ID_MAX)
    exhausted();
#else
    sequential(); concurrent();
#endif
    expect(aevix_task_scope_runtime_drain()==0,"final drain completes");
    std::printf("%u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
