# ========================
#
# Created By : miruku_sora2026
#
# File name : parallel_cli_tests.cmake
#
# Description :
#     병렬 모듈의 AOT·JIT 실행과 import·대여·작업 함수 효과 제한을 검사한다.
#     Check AOT/JIT parallel execution, explicit imports, borrows, and worker effect restrictions.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
file(WRITE "${TEST_DIR}/main.avx" [=[
import parallel;
void add(&mut i32 item) { *item += 3; }
void transform(&mut i32 item) { add(&mut *item); *item *= 2; }
i32 main() {
    mut i32 values[1025] = [1; 1025];
    parallel.for_each(Slice.from_array_mut(&mut values), transform);
    for (i32 value : values) { if (value != 8) return 1; }
    usize workers = parallel.worker_count();
    if (workers < 1 || workers > 32) return 2;
    return 0;
}
]=])
function(run_parallel name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
    if(NOT "${result}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${result}: ${output}${error}")
    endif()
endfunction()
run_parallel(check 0 "" --check "${TEST_DIR}/main.avx")
run_parallel(ir 0 "aevix_parallel_for_each" --emit-llvm "${TEST_DIR}/main.avx")
run_parallel(run 0 "" --run "${TEST_DIR}/main.avx")
run_parallel(run_optimized 0 "" --run -O2 "${TEST_DIR}/main.avx")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
if(version MATCHES "ORC JIT enabled")
    run_parallel(jit 0 "" --jit "${TEST_DIR}/main.avx")
endif()
file(WRITE "${TEST_DIR}/missing_import.avx" "void work(&mut i32 item) { *item += 1; } i32 main() { mut i32 a[2] = [0; 2]; parallel.for_each(Slice.from_array_mut(&mut a), work); return 0; }\n")
run_parallel(missing_import 1 "Unknown|Unresolved|import|not found|resolve" --check "${TEST_DIR}/missing_import.avx")
file(WRITE "${TEST_DIR}/global.avx" "import parallel; mut i32 total = 0; void work(&mut i32 item) { total += *item; } i32 main() { mut i32 a[2] = [0; 2]; parallel.for_each(Slice.from_array_mut(&mut a), work); return 0; }\n")
run_parallel(global 1 "global|Parallel" --check "${TEST_DIR}/global.avx")
file(WRITE "${TEST_DIR}/indirect_global.avx" "import parallel; persist i32 total = 1; i32 bad() { return total; } void work(&mut i32 item) { *item += bad(); } i32 main() { mut i32 a[2] = [0; 2]; parallel.for_each(Slice.from_array_mut(&mut a), work); return 0; }\n")
run_parallel(indirect_global 1 "global|persist|Parallel" --check "${TEST_DIR}/indirect_global.avx")
file(WRITE "${TEST_DIR}/ffi.avx" "import parallel; extern \"C\" i32 foreign(i32 value); void work(&mut i32 item) { *item = foreign(*item); } i32 main() { mut i32 a[2] = [0; 2]; parallel.for_each(Slice.from_array_mut(&mut a), work); return 0; }\n")
run_parallel(ffi 1 "external|Parallel" --check "${TEST_DIR}/ffi.avx")
file(WRITE "${TEST_DIR}/wrong_worker.avx" "import parallel; void work(&i32 item) { return; } i32 main() { mut i32 a[2] = [0; 2]; parallel.for_each(Slice.from_array_mut(&mut a), work); return 0; }\n")
run_parallel(wrong_worker 1 "worker.*signature|worker.*Element" --check "${TEST_DIR}/wrong_worker.avx")
file(WRITE "${TEST_DIR}/readonly.avx" "import parallel; void work(&mut i32 item) { *item += 1; } i32 main() { i32 a[2] = [0; 2]; parallel.for_each(Slice.from_array(&a), work); return 0; }\n")
run_parallel(readonly 1 "MutSlice|exclusive|mutable" --check "${TEST_DIR}/readonly.avx")
message(STATUS "Parallel AOT/JIT execution and worker safety checks passed")
