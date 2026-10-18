# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_cancellation_cli_tests.cmake
#
# Description :
#     독립 취소 소유자의 역할·이동·공유 상태·실패 보존·정확한 소멸을 검사한다.
#     Check independent cancellation roles, moves, shared state, failure preservation, and exact cleanup.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(header [=[
import task { Task, CancellationSource, CancellationToken, CancellationError, CancelRequest };
T success<T, E>(Result<T, E> input) {
    switch (move input) {
        case Result<T, E>.Ok { T value = value }: return value; break;
        case Result<T, E>.Err: Never.unreachable(); break;
    }
}
]=])
function(cancel_run name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(cancel_accept name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${header}\n${source}\n")
    cancel_run("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    cancel_run("${name} AOT O0" 0 "" --run -O0 "${TEST_DIR}/${name}.avx")
    cancel_run("${name} AOT O2" 0 "" --run -O2 "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        cancel_run("${name} JIT O0" 0 "" --jit -O0 "${TEST_DIR}/${name}.avx")
        cancel_run("${name} JIT O2" 0 "" --jit -O2 "${TEST_DIR}/${name}.avx")
    endif()
endfunction()
function(cancel_reject name source pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${header}\n${source}\n")
    cancel_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()
cancel_accept(shared_request [=[
i32 main() {
    CancellationSource source = success(CancellationSource.new());
    CancellationToken token = success(CancellationSource.token(&source));
    CancellationToken copy = success(CancellationToken.clone(&token));
    CancellationSource authority = success(CancellationSource.clone(&source));
    if (CancellationToken.requested(&token) || Task.cancel_requested(&copy)) return 1;
    CancelRequest first = success(CancellationSource.request(&authority));
    switch (first) { case CancelRequest.NewlyRequested: break; case CancelRequest.AlreadyRequested: return 2; break; }
    if (!Task.cancel_requested(&token) || !CancellationToken.requested(&copy)) return 3;
    CancelRequest repeated = success(Task.cancel_request(&source));
    switch (repeated) { case CancelRequest.NewlyRequested: return 4; break; case CancelRequest.AlreadyRequested: break; }
    return 0;
}
]=])
cancel_accept(independent_observer [=[
CancellationToken retained() {
    CancellationSource source = success(Task.cancel_source());
    CancellationToken token = success(Task.cancel_token(&source));
    success(Task.cancel_request(&source));
    return token;
}
i32 main() {
    CancellationToken retained_token = retained();
    CancellationToken clone = success(Task.cancel_clone(&retained_token));
    CancellationSource independent = success(Task.cancel_source());
    CancellationToken clean = success(Task.cancel_token(&independent));
    return Task.cancel_requested(&retained_token) && CancellationToken.requested(&clone) && !Task.cancel_requested(&clean) ? 0 : 1;
}
]=])
cancel_accept(move_aggregates [=[
struct Holder { public CancellationToken token; };
CancellationSource identity(CancellationSource input) { return input; }
i32 main() {
    CancellationSource initial = success(Task.cancel_source());
    CancellationSource source = identity(initial);
    Holder first = Holder { token = success(Task.cancel_token(&source)) };
    Holder second = first;
    success(Task.cancel_request(&source));
    return Task.cancel_requested(&second.token) ? 0 : 1;
}
]=])
cancel_accept(errors [=[
i32 main() {
    CancellationError allocation = CancellationError.AllocationFailure;
    CancellationError overflow = CancellationError.Overflow;
    return string.equals(allocation.code(), "task.cancellation_allocation_failure") && string.equals(overflow.code(), "task.cancellation_overflow") && string.byte_len(allocation.message()) > 0 && string.byte_len(overflow.message()) > 0 ? 0 : 1;
}
]=])
cancel_reject(wrong_request_role [=[i32 main() { CancellationSource source = success(Task.cancel_source()); CancellationToken token = success(Task.cancel_token(&source)); Task.cancel_request(&token); return 0; }]=] "No matching|requires|overload")
cancel_reject(wrong_query_role [=[i32 main() { CancellationSource source = success(Task.cancel_source()); Task.cancel_requested(&source); return 0; }]=] "No matching|requires|overload")
cancel_reject(no_private_identity [=[i32 main() { CancellationSource source = success(Task.cancel_source()); return source.state_id == 0 ? 0 : 1; }]=] "private")
cancel_reject(no_source_forgery [=[i32 main() { CancellationSource forged = CancellationSource { state_id = 1u64 }; return 0; }]=] "cannot be constructed|private")
cancel_reject(no_token_forgery [=[i32 main() { CancellationToken forged = CancellationToken { state_id = 1u64 }; return 0; }]=] "cannot be constructed|private")
cancel_reject(no_implicit_copy [=[i32 main() { CancellationSource source = success(Task.cancel_source()); CancellationSource moved = source; Task.cancel_token(&source); return 0; }]=] "moved|move|unavailable")
cancel_reject(no_token_copy [=[i32 main() { CancellationSource source = success(Task.cancel_source()); CancellationToken token = success(Task.cancel_token(&source)); CancellationToken moved = token; Task.cancel_requested(&token); return 0; }]=] "moved|move|unavailable")
cancel_reject(no_copy_container [=[struct Holder implements Copy { public CancellationToken token; }; i32 main() { return 0; }]=] "Copy|copy")
cancel_reject(no_raw_extern [=[extern "C" i32 aevix_cancel_source_new(u64 value); i32 main() { return 0; }]=] "reserved|compiler|aevix_")
cancel_reject(no_spawn [=[i32 main() { Task.spawn([]() -> i32 { return 1; }); return 0; }]=] "Task|scope|argument|requires|matching|member|declaration")

file(WRITE "${TEST_DIR}/namespace.avx" [=[
struct CancellationSource { public i32 new() { return 7; } };
struct CancellationToken { public i32 requested(i32 token) { return token + 2; } };
struct Task { public i32 cancel_source() { return 11; } };
T identity<T>(T value) { return value; }
i32 main() {
    return identity(CancellationSource.new()) == 7 && identity(CancellationToken.requested(3)) == 5 && identity(Task.cancel_source()) == 11 ? 0 : 1;
}
]=])
cancel_run("ordinary namespace check" 0 "" --check "${TEST_DIR}/namespace.avx")
cancel_run("ordinary namespace AOT O0" 0 "" --run -O0 "${TEST_DIR}/namespace.avx")
cancel_run("ordinary namespace AOT O2" 0 "" --run -O2 "${TEST_DIR}/namespace.avx")
if(version MATCHES "ORC JIT enabled")
    cancel_run("ordinary namespace JIT O0" 0 "" --jit -O0 "${TEST_DIR}/namespace.avx")
    cancel_run("ordinary namespace JIT O2" 0 "" --jit -O2 "${TEST_DIR}/namespace.avx")
endif()
file(WRITE "${TEST_DIR}/import_required.avx" "i32 main() { CancellationSource source; return 0; }\n")
cancel_run("cancellation requires import" 1 "Unknown|unknown|undeclared" --check "${TEST_DIR}/import_required.avx")

if(DEFINED CLANG AND EXISTS "${CLANG}")
    get_filename_component(repo "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    file(WRITE "${TEST_DIR}/failure.avx" "${header}\n")
    file(APPEND "${TEST_DIR}/failure.avx" [=[
extern "C" i32 cancellation_test_stage();
extern "C" void cancellation_test_arm(i32 stage);
extern "C" void cancellation_test_reset();
bool expected_error<T>(Result<T, CancellationError> input, bool overflow) {
    switch (move input) {
        case Result<T, CancellationError>.Ok: return false; break;
        case Result<T, CancellationError>.Err { CancellationError error = error }:
            switch (error) { case CancellationError.AllocationFailure: return !overflow; break; case CancellationError.Overflow: return overflow; break; }
            break;
    }
}
i32 main() {
    i32 stage = cancellation_test_stage();
    if (stage == 0) return expected_error(Task.cancel_source(), false) ? 0 : 1;
    CancellationSource source = success(Task.cancel_source());
    CancellationToken token = success(Task.cancel_token(&source));
    cancellation_test_arm(stage);
    mut bool rejected = false;
    if (stage == 1 || stage == 4) rejected = expected_error(CancellationSource.clone(&source), stage >= 4);
    if (stage == 2 || stage == 5) rejected = expected_error(Task.cancel_token(&source), stage >= 4);
    if (stage == 3 || stage == 6) rejected = expected_error(Task.cancel_clone(&token), stage >= 4);
    cancellation_test_reset();
    if (!rejected || Task.cancel_requested(&token)) return 2;
    success(Task.cancel_request(&source));
    if (!Task.cancel_requested(&token)) return 3;
    CancellationToken recovered = success(Task.cancel_clone(&token));
    return Task.cancel_requested(&recovered) ? 0 : 4;
}
]=])
    cancel_run("failure fixture check" 0 "" --check "${TEST_DIR}/failure.avx")
    execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/failure.avx" RESULT_VARIABLE status OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 90)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Cancellation failure fixture IR: ${error}")
    endif()
    string(REPLACE "@main(" "@cancellation_test_main(" ir "${ir}")
    file(WRITE "${TEST_DIR}/failure.ll" "${ir}")
    file(WRITE "${TEST_DIR}/failure.cpp" [=[
#include "aevix/Runtime/TaskRuntime.hpp"
#include "aevix/Runtime/CancellationRuntime.hpp"
#include <cstdint>
#include <limits>
static std::int32_t stage;
extern "C" std::int32_t cancellation_test_stage() { return stage; }
extern "C" void cancellation_test_arm(std::int32_t selected) {
    if (selected < 4) aevix_task_test_fail_after(0);
    else aevix_cancel_test_owner_limit(2);
}
extern "C" void cancellation_test_reset() {
    aevix_task_test_fail_after(std::numeric_limits<std::uint64_t>::max());
    aevix_cancel_test_owner_limit(std::numeric_limits<std::uint64_t>::max());
}
extern "C" int cancellation_test_main();
int main(int argc, char** argv) {
    if (argc != 2 || argv[1][0] < '0' || argv[1][0] > '6' || argv[1][1]) return 30;
    stage = argv[1][0] - '0';
    cancellation_test_reset();
    if (stage == 0) aevix_task_test_fail_after(0);
    int result = cancellation_test_main();
    cancellation_test_reset();
    if (result) return 10 + result;
    return aevix_cancel_test_live_states() == 0 ? 0 : 20;
}
]=])
    foreach(opt IN ITEMS 0 2)
        execute_process(COMMAND "${CLANG}" --driver-mode=g++ -std=c++20 -O${opt} -fno-exceptions -fno-rtti -pthread -DAEVIX_TASK_RUNTIME_TESTING "-I${repo}/include"
            "${TEST_DIR}/failure.ll" "${TEST_DIR}/failure.cpp" "${repo}/runtime/Threading/TaskRuntime.cpp" "${repo}/runtime/Native/NativeBoundary.cpp" "${repo}/runtime/System/OsRuntime.cpp"
            -o "${TEST_DIR}/failure-O${opt}" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 180)
        if(NOT status EQUAL 0)
            message(FATAL_ERROR "Cancellation native failure harness O${opt}: ${output}${error}")
        endif()
        # Fresh processes prevent cached metadata slots from bypassing the intended allocation failure.
        foreach(pass RANGE 1 8)
            foreach(stage RANGE 0 6)
                execute_process(COMMAND "${TEST_DIR}/failure-O${opt}" "${stage}" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
                if(NOT status EQUAL 0)
                    message(FATAL_ERROR "Cancellation failure/Drop harness O${opt} pass${pass} stage${stage}: ${status}\n${output}${error}")
                endif()
            endforeach()
        endforeach()
    endforeach()
else()
    message(STATUS "Cancellation deterministic failure harness requires CLANG; ordinary source gates still ran")
endif()
message(STATUS "Cancellation source roles, retained state, lifecycle and failure-preservation gates passed")
