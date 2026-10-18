# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_task_ready_fault_cli_tests.cmake
#
# Description :
#     실제 Ready 생성의 새 프로세스 할당 실패, 이동 인수 정리와 작업자 부재를 검증한다.
#     Verify fresh-process Ready allocation failures, moved-input cleanup and zero worker creation in real generated code.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED CLANG OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC, CLANG and TEST_DIR are required")
endif()
get_filename_component(repo "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(DEFINED SOURCE_ROOT)
    set(repo "${SOURCE_ROOT}")
endif()
set(task_runtime "${repo}/runtime/Threading/TaskRuntime.cpp")
set(includes "-I${repo}/include")
if(DEFINED DRAFT_ROOT)
    set(task_runtime "${DRAFT_ROOT}/runtime/Threading/TaskRuntime.cpp")
    list(PREPEND includes "-I${DRAFT_ROOT}/include")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
file(WRITE "${TEST_DIR}/ready_fault.avx" [=[
import task { Task, TaskScope, Executor, ScopeOptions, ScopeReport, SpawnError };
import memory { Shared };
import atomic { Atomic, MemoryOrder };
extern "C" i32 ready_fault_stage();
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; default: Never.unreachable(); break; } }
struct Owned implements Drop {
    public Shared<Atomic<i32>> count;
    public i32 value;
    public void drop(&mut Self self) { success(Atomic.fetch_add(Shared.as_ref(&self.count),1,MemoryOrder.SeqCst)); return; }
};
i32 main() {
    i32 stage = ready_fault_stage();
    mut bool had_result = false;
    Shared<Atomic<i32>> counter = success(Shared.new(Atomic.new(0)));
    Executor executor = Executor.native_threads(); TaskScope scope = success(Task.open_scope(&executor,ScopeOptions.defaults()));
    Owned input = Owned { count = success(Shared.clone(&counter)), value = 42 };
    Result<Task<Owned>,SpawnError> prepared = Task.ready(&scope,input);
    switch (move prepared) {
        case Result<Task<Owned>,SpawnError>.Ok { Task<Owned> work = value }:
            had_result = true;
            { Owned result = success(Task.join(work)); if (result.value != 42 || success(Atomic.load(Shared.as_ref(&counter),MemoryOrder.SeqCst)) != 0) return 2; }
            break;
        case Result<Task<Owned>,SpawnError>.Err { SpawnError reason = error }:
            switch (reason) {
                case SpawnError.AllocationFailure: if (stage > 7) return 4; break;
                case SpawnError.ScopeClosed: if (stage != 10) return 5; break;
                default: return 6; break;
            }
            break;
    }
    if (success(Atomic.load(Shared.as_ref(&counter),MemoryOrder.SeqCst)) != 1 || Shared.strong_count(&counter) != 1usize) return 7;
    ScopeReport report = success(Task.close_scope(scope));
    return report.accepted == (had_result ? 1u64 : 0u64) && report.completed == report.accepted && report.observed == report.accepted ? 0 : 8;
}
]=])
execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/ready_fault.avx" RESULT_VARIABLE status OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 90)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Ready fault source: ${error}")
endif()
string(REPLACE "@main(" "@ready_fault_main(" ir "${ir}")
string(REPLACE "@aevix_task_scope_prepare_ready(" "@ready_fault_prepare(" ir "${ir}")
file(WRITE "${TEST_DIR}/ready_fault.ll" "${ir}")
file(WRITE "${TEST_DIR}/ready_fault.cpp" [=[
#include "aevix/Runtime/TaskScopeRuntime.hpp"
#include "aevix/Runtime/CancellationRuntime.hpp"
#include "aevix/Runtime/SharedRuntime.hpp"
#include <cstdlib>
#include <cstdint>
static int stage=0,successful=0;
static bool invalidPreparation=false;
extern "C" std::int32_t ready_fault_stage(){return stage;}
extern "C" std::int32_t ready_fault_succeeded(){return successful;}
extern "C" std::int32_t ready_fault_prepare(std::uint64_t scope,std::uint64_t bytes,std::uint64_t align,
    aevix::TaskDrop drop,std::uint64_t* owner,void** storage,std::uint64_t* identity) noexcept {
    aevix_task_test_fail_threads(1);aevix_task_test_fail_reap(3);
    if(stage<=7)aevix_task_test_fail_after(std::uint64_t(stage));
    if(stage==10 && aevix_task_scope_close_admission(scope))invalidPreparation=true;
    const auto status=aevix_task_scope_prepare_ready(scope,bytes,align,drop,owner,storage,identity);
    aevix_task_test_fail_after(UINT64_MAX);
    successful=status==0?1:0;
    if(status==0) {
        std::uint64_t active=99;
        if(aevix_task_scope_active_count(scope,&active) || active!=1 || !*owner || !*storage || !*identity)invalidPreparation=true;
        if(stage==8 && aevix_task_scope_close_admission(scope))invalidPreparation=true;
    }else if(*owner || *storage || *identity)invalidPreparation=true;
    return status;
}
extern "C" int ready_fault_main();
int main(int argc,char** argv) {
    if(argc!=2)return 30;
    char* tail=nullptr;const auto selected=std::strtol(argv[1],&tail,10);
    if(!tail || *tail || selected<0 || selected>10)return 30;stage=int(selected);
    const auto result=ready_fault_main();if(result)return 40+result;
    if(invalidPreparation || aevix_task_test_thread_creations()!=0 || aevix_task_test_native_reaps()!=0)return 31;
    if(aevix_task_scope_runtime_drain())return 32;
    return aevix_task_test_live_controls()==0 && aevix_task_scope_test_live_handles()==0 && aevix_task_scope_test_live_scopes()==0 && aevix_cancel_test_live_states()==0 && aevix_shared_test_live_controls()==0?0:33;
}
]=])
foreach(opt 0 2)
    execute_process(COMMAND "${CLANG}" --driver-mode=g++ -std=c++20 "-O${opt}" -fno-exceptions -fno-rtti -pthread
        -DAEVIX_TASK_RUNTIME_TESTING -DAEVIX_SHARED_RUNTIME_TESTING ${includes}
        "${TEST_DIR}/ready_fault.ll" "${TEST_DIR}/ready_fault.cpp" "${task_runtime}"
        "${repo}/runtime/Memory/SharedRuntime.cpp" "${repo}/runtime/Threading/AtomicRuntime.cpp"
        "${repo}/runtime/Native/NativeBoundary.cpp" "${repo}/runtime/System/OsRuntime.cpp"
        -o "${TEST_DIR}/ready_fault-O${opt}" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 180)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Ready fault build O${opt}: ${output}${error}")
    endif()
    foreach(stage RANGE 0 10)
        execute_process(COMMAND "${TEST_DIR}/ready_fault-O${opt}" "${stage}" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
        if(NOT status EQUAL 0)
            message(FATAL_ERROR "Ready fault O${opt}/${stage}: ${status}\n${output}${error}")
        endif()
    endforeach()
endforeach()
message(STATUS "Ready fault: 22 fresh processes; exact moved-input cleanup and zero OS worker creation/reaping")
