# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_reference_payload_cli_tests.cmake
#
# Description :
#     enum 내부 참조 값의 이동·복사와 필드 자체에 대한 새 대여를 구분해 검사한다.
#     Distinguish moving or copying stored enum reference values from borrowing the fields themselves.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
function(run_reference name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
file(WRITE "${TEST_DIR}/main.avx" [=[
import result;
enum Loan { Some { public &mut i32 reference; }; };
&mut i32 extract(Loan owner) {
    switch (move owner) {
        case Loan.Some { &mut i32 reference = reference }: return reference; break;
    }
}
enum Shared { Some { public &i32 reference; }; };
i32 main() {
    mut i32 target = 1;
    Loan owner = Loan.Some { reference = &mut target };
    &mut i32 view = extract(owner);
    *view = 2;
    if (*view != 2) return 1;
    Result<i32, &mut i32> failure = Result.err(&mut target);
    Optional<&mut i32> maybe = result.err_optional(failure);
    switch (move maybe) {
        case Optional<&mut i32>.Some { &mut i32 reference = value }: *reference = 7; break;
        case Optional<&mut i32>.None: return 2; break;
    }
    Shared shared = Shared.Some { reference = &target };
    switch (shared) {
        case Shared.Some { &i32 reference = reference }: if (*reference != 7) return 3; break;
    }
    switch (shared) { case Shared.Some: break; }
    return 0;
}
]=])
run_reference(check 0 "" --check "${TEST_DIR}/main.avx")
run_reference(aot 0 "" --run "${TEST_DIR}/main.avx")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
if(version MATCHES "ORC JIT enabled")
    run_reference(jit 0 "" --jit "${TEST_DIR}/main.avx")
endif()
file(WRITE "${TEST_DIR}/copy_mutable.avx" [=[
enum Loan { Some { public &mut i32 reference; }; };
i32 bad(Loan owner) {
    switch (owner) { case Loan.Some { &mut i32 reference = reference }: *reference = 2; break; }
    return 0;
}
i32 main() { return 0; }
]=])
run_reference(reject_mutable_copy 1 "non-Copy payload" --check "${TEST_DIR}/copy_mutable.avx")
file(WRITE "${TEST_DIR}/borrow_consumed.avx" [=[
enum Number { Some { public i32 value; }; };
i32 bad(Number owner) {
    switch (move owner) { case Number.Some { &i32 reference = &value }: return *reference; break; }
}
i32 main() { return 0; }
]=])
run_reference(reject_new_borrow 1 "consuming switch cannot" --check "${TEST_DIR}/borrow_consumed.avx")
message(STATUS "Stored reference payload movement and borrowing passed")
