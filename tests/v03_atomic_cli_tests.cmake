# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_atomic_cli_tests.cmake
#
# Description :
#     CPU 원자 셀의 순서, 산술, 오류, 독점 접근과 실행 백엔드를 검사한다.
#     Check CPU atomic ordering, arithmetic, errors, exclusivity, and execution backends.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version TIMEOUT 10)
set(header [=[
import atomic { Atomic, MemoryOrder, AtomicError, CasOutcome, WaitOutcome, Duration };
import memory { Shared };
import error { AnyError };
T success<T, E>(Result<T, E> input) {
    switch (move input) { case Result<T, E>.Ok { T value = value }: return value; break; case Result<T, E>.Err: Never.unreachable(); break; }
}
bool invalid_order<T>(Result<T, AtomicError> input) {
    switch (move input) {
        case Result<T, AtomicError>.Ok: return false; break;
        case Result<T, AtomicError>.Err { AtomicError failure = error }:
            switch (failure) { case AtomicError.InvalidOrder: return true; break; default: return false; break; }
            break;
    }
}
]=])
function(atomic_run name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(atomic_accept name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${header}\n${source}\n")
    atomic_run("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    atomic_run("${name} AOT" 0 "" --run "${TEST_DIR}/${name}.avx")
    atomic_run("${name} AOT O2" 0 "" --run -O2 "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        atomic_run("${name} JIT" 0 "" --jit "${TEST_DIR}/${name}.avx")
        atomic_run("${name} JIT O2" 0 "" --jit -O2 "${TEST_DIR}/${name}.avx")
    endif()
endfunction()
function(atomic_reject name source pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${header}\n${source}\n")
    atomic_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()
atomic_accept(arithmetic [=[
i32 main() {
    Atomic<i8> tiny = Atomic.new(127i8);
    if (success(Atomic.fetch_add(&tiny, 1i8, MemoryOrder.Relaxed)) != 127i8) return 1;
    if (success(Atomic.load(&tiny, MemoryOrder.Acquire)) != -128i8) return 2;
    if (success(Atomic.saturating_fetch_sub(&tiny, 1i8, MemoryOrder.SeqCst)) != -128i8) return 3;
    Result<i8, AtomicError> overflow = Atomic.checked_fetch_sub(&tiny, 1i8, MemoryOrder.Relaxed);
    switch (move overflow) {
        case Result<i8, AtomicError>.Ok: return 4; break;
        case Result<i8, AtomicError>.Err { AtomicError failure = error }:
            switch (failure) { case AtomicError.Overflow: break; default: return 5; break; }
            break;
    }
    if (success(Atomic.fetch_max(&tiny, 7i8, MemoryOrder.AcqRel)) != -128i8) return 6;
    if (success(Atomic.fetch_min(&tiny, 3i8, MemoryOrder.Release)) != 7i8) return 7;
    if (success(Atomic.swap(&tiny, 2i8, MemoryOrder.Acquire)) != 3i8) return 8;
    if (success(Atomic.fetch_and(&tiny, 3i8, MemoryOrder.Relaxed)) != 2i8) return 9;
    if (success(Atomic.fetch_or(&tiny, 4i8, MemoryOrder.Relaxed)) != 2i8) return 10;
    if (success(Atomic.fetch_xor(&tiny, 1i8, MemoryOrder.Relaxed)) != 6i8) return 11;
    if (success(Atomic.fetch_nand(&tiny, 3i8, MemoryOrder.Relaxed)) != 7i8) return 12;
    if (success(Atomic.load(&tiny, MemoryOrder.Relaxed)) != -4i8) return 13;
    Atomic<u8> small = Atomic.new(255u8);
    if (success(Atomic.fetch_add(&small, 1u8, MemoryOrder.Relaxed)) != 255u8 || success(Atomic.load(&small, MemoryOrder.Relaxed)) != 0u8) return 14;
    Atomic<i16> medium = Atomic.new(12i16);
    Atomic<u16> medium_unsigned = Atomic.new(13u16);
    Atomic<i32> regular = Atomic.new(14);
    Atomic<u32> regular_unsigned = Atomic.new(15u32);
    Atomic<i64> wide = Atomic.new(-16i64);
    Atomic<u64> wide_unsigned = Atomic.new(17u64);
    Atomic<isize> address = Atomic.new(18isize);
    Atomic<usize> size = Atomic.new(19usize);
    if (success(Atomic.checked_fetch_add(&medium, 1i16, MemoryOrder.SeqCst)) != 12i16) return 15;
    if (success(Atomic.saturating_fetch_add(&medium_unsigned, 1u16, MemoryOrder.SeqCst)) != 13u16) return 16;
    if (success(Atomic.fetch_sub(&regular, 1, MemoryOrder.SeqCst)) != 14) return 17;
    success(Atomic.store(&regular_unsigned, 25u32, MemoryOrder.Release));
    if (success(Atomic.load(&regular_unsigned, MemoryOrder.Acquire)) != 25u32) return 18;
    if (Atomic.into_inner(wide) != -16i64 || Atomic.into_inner(wide_unsigned) != 17u64) return 19;
    return Atomic.into_inner(address) == 18isize && Atomic.into_inner(size) == 19usize ? 0 : 20;
}
]=])
atomic_accept(compare_bool [=[
i32 main() {
    Atomic<i32> cell = Atomic.new(7);
    CasOutcome<i32> mismatch = success(Atomic.compare_exchange(&cell, 6, 9, MemoryOrder.Release, MemoryOrder.SeqCst));
    if (mismatch.changed || mismatch.observed != 7) return 1;
    CasOutcome<i32> changed = success(Atomic.compare_exchange(&cell, 7, 9, MemoryOrder.Relaxed, MemoryOrder.Acquire));
    if (!changed.changed || changed.observed != 7) return 2;
    CasOutcome<i32> weak = success(Atomic.compare_exchange_weak(&cell, 8, 12, MemoryOrder.AcqRel, MemoryOrder.Relaxed));
    if (weak.changed || weak.observed != 9) return 3;
    Atomic<bool> flag = Atomic.new(true);
    if (!success(Atomic.fetch_and(&flag, false, MemoryOrder.Relaxed))) return 4;
    if (success(Atomic.fetch_or(&flag, true, MemoryOrder.Relaxed))) return 5;
    if (!success(Atomic.fetch_xor(&flag, true, MemoryOrder.Relaxed))) return 6;
    if (success(Atomic.fetch_nand(&flag, true, MemoryOrder.Relaxed))) return 7;
    return success(Atomic.load(&flag, MemoryOrder.SeqCst)) ? 0 : 8;
}
]=])
atomic_accept(dynamic_orders [=[
MemoryOrder dynamic(i32 select) { if (select == 0) return MemoryOrder.Relaxed; if (select == 1) return MemoryOrder.Acquire; return MemoryOrder.Release; }
i32 main() {
    Atomic<i32> cell = Atomic.new(7);
    if (!invalid_order(Atomic.load(&cell, dynamic(2)))) return 1;
    if (!invalid_order(Atomic.store(&cell, 9, dynamic(1)))) return 2;
    if (!invalid_order(Atomic.compare_exchange(&cell, 7, 9, MemoryOrder.SeqCst, dynamic(2)))) return 3;
    if (!invalid_order(Atomic.fence(dynamic(0)))) return 4;
    if (!invalid_order(Atomic.compiler_fence(dynamic(0)))) return 5;
    success(Atomic.fence(MemoryOrder.Acquire));
    success(Atomic.compiler_fence(MemoryOrder.Release));
    return success(Atomic.load(&cell, MemoryOrder.Relaxed)) == 7 ? 0 : 6;
}
]=])
atomic_accept(exclusive_wait_capabilities [=[
i32 main() {
    mut Atomic<i32> cell = Atomic.new(5);
    { &mut i32 inner = Atomic.get_mut(&mut cell); *inner = 7; }
    if (success(Atomic.wait(&cell, 5, MemoryOrder.Acquire)) != 7) return 1;
    WaitOutcome<i32> timed = success(Atomic.wait_timeout(&cell, 7, Duration.from_nanos(0), MemoryOrder.Relaxed));
    if (!timed.timed_out || timed.changed || timed.observed != 7) return 2;
    WaitOutcome<i32> ready = success(Atomic.wait_timeout(&cell, 5, Duration.from_nanos(0), MemoryOrder.SeqCst));
    if (ready.timed_out || !ready.changed || ready.observed != 7) return 3;
    Atomic.notify_one(&cell);
    Atomic.notify_all(&cell);
    if (!Atomic.is_supported<i64>() || Atomic.required_alignment<i64>() != 8) return 4;
    if (Atomic.is_supported<i128>() || Atomic.is_lock_free<i128>() || Atomic.required_alignment<i128>() != 0) return 5;
    if (!Atomic<bool>.is_supported()) return 6;
    return Atomic.into_inner(cell) == 7 ? 0 : 7;
}
]=])
atomic_accept(shared_inline_cell [=[
i32 main() {
    Shared<Atomic<i32>> owner = success(Shared.new(Atomic.new(3)));
    Shared<Atomic<i32>> alias = success(Shared.clone(&owner));
    success(Atomic.store(Shared.as_ref(&owner), 11, MemoryOrder.Release));
    if (success(Atomic.fetch_add(Shared.as_ref(&alias), 2, MemoryOrder.AcqRel)) != 11) return 1;
    return success(Atomic.load(Shared.as_ref(&owner), MemoryOrder.Acquire)) == 13 ? 0 : 2;
}
]=])
atomic_accept(error_erasure [=[
i32 main() {
    AtomicError invalid = AtomicError.InvalidOrder;
    if (!string.equals(invalid.code(), "atomic.invalid_order")) return 1;
    if (!string.equals(invalid.message(), "The memory order is invalid for this atomic operation.")) return 2;
    AnyError first = success(AnyError.pack(invalid));
    if (!string.equals(first.code(), "atomic.invalid_order")) return 3;
    AnyError second = success(AnyError.pack(AtomicError.Overflow));
    if (!string.equals(second.code(), "atomic.overflow")) return 4;
    if (!string.equals(second.message(), "The checked atomic arithmetic would overflow; this attempt performed no write.")) return 5;
    return 0;
}
]=])
atomic_reject(no_copy [=[i32 main() { Atomic<i32> original = Atomic.new(1); Atomic<i32> moved = original; return success(Atomic.load(&original, MemoryOrder.Relaxed)); }]=] "moved|unavailable")
atomic_reject(no_copy_field [=[struct Holder implements Copy { public Atomic<i32> value; }; i32 main() { return 0; }]=] "Copy|copy")
atomic_reject(no_private_construction [=[i32 main() { Atomic<i32> cell = Atomic<i32> { value = 7 }; return 0; }]=] "private|initializer|construct|nominal")
atomic_reject(no_float [=[i32 main() { Atomic<f64> cell = Atomic.new(1.0); return 0; }]=] "Atomic supports|CPU integers")
atomic_reject(no_i128 [=[i32 main() { Atomic<i128> cell = Atomic.new(1i128); return 0; }]=] "Atomic supports|CPU integers")
atomic_reject(bool_arithmetic [=[i32 main() { Atomic<bool> cell = Atomic.new(true); Atomic.fetch_add(&cell, true, MemoryOrder.Relaxed); return 0; }]=] "requires an integer")
atomic_reject(known_load_order [=[i32 main() { Atomic<i32> cell = Atomic.new(1); Atomic.load(&cell, MemoryOrder.Release); return 0; }]=] "Known invalid MemoryOrder")
atomic_reject(known_order_alias [=[i32 main() { Atomic<i32> cell = Atomic.new(1); MemoryOrder invalid = MemoryOrder.Release; Atomic.load(&cell, invalid); return 0; }]=] "Known invalid MemoryOrder")
atomic_reject(known_store_order [=[i32 main() { Atomic<i32> cell = Atomic.new(1); Atomic.store(&cell, 2, MemoryOrder.Acquire); return 0; }]=] "Known invalid MemoryOrder")
atomic_reject(known_cas_failure [=[i32 main() { Atomic<i32> cell = Atomic.new(1); Atomic.compare_exchange(&cell, 1, 2, MemoryOrder.SeqCst, MemoryOrder.AcqRel); return 0; }]=] "Known invalid MemoryOrder")
atomic_reject(known_fence_order [=[i32 main() { Atomic.fence(MemoryOrder.Relaxed); return 0; }]=] "Known invalid MemoryOrder")
atomic_reject(readonly_get_mut [=[i32 main() { Atomic<i32> cell = Atomic.new(1); Atomic.get_mut(&cell); return 0; }]=] "explicit compatible|borrow")
atomic_reject(live_exclusive [=[i32 main() { mut Atomic<i32> cell = Atomic.new(1); &mut i32 inner = Atomic.get_mut(&mut cell); Atomic.store(&cell, 2, MemoryOrder.Relaxed); return *inner; }]=] "borrow|loan")
atomic_reject(live_extract [=[i32 main() { mut Atomic<i32> cell = Atomic.new(1); &mut i32 inner = Atomic.get_mut(&mut cell); Atomic.into_inner(cell); return *inner; }]=] "borrow|loan")
atomic_reject(namespace_mismatch [=[i32 main() { Atomic<i32> cell = Atomic.new(1); Atomic<u32>.load(&cell, MemoryOrder.Relaxed); return 0; }]=] "scalar.*match|match.*cell")
atomic_reject(named_unsupported [=[i32 main() { Atomic<i32> cell = Atomic.new(value = 1); return 0; }]=] "positional")
message(STATUS "Atomic CPU frontend, error, ownership, AOT and JIT checks passed")
