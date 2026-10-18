# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_sync_cli_tests.cmake
#
# Description :
#     인라인 잠금 보호자의 해제·오염 복구·대여 수명과 위조 방지를 검사한다.
#     Check inline lock guard release, poison recovery, loan lifetimes, and forgery rejection.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(sync_header [=[
import sync { Mutex, RwLock, MutexGuard, RwLockReadGuard, RwLockWriteGuard, PoisonPolicy, Poisoned, LockError };
import time { Duration };
T success<T, E>(Result<T, E> input) {
    switch (move input) { case Result<T, E>.Ok { T value = value }: return value; break; case Result<T, E>.Err: Never.unreachable(); break; }
}
T present<T>(Optional<T> input) {
    switch (move input) { case Optional<T>.Some { T value = value }: return value; break; case Optional<T>.None: Never.unreachable(); break; }
}
]=])
function(sync_run name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(sync_accept name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${sync_header}\n${source}\n")
    sync_run("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    sync_run("${name} AOT" 0 "" --run "${TEST_DIR}/${name}.avx")
    sync_run("${name} AOT O2" 0 "" --run -O2 "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        sync_run("${name} JIT" 0 "" --jit "${TEST_DIR}/${name}.avx")
        sync_run("${name} JIT O2" 0 "" --jit -O2 "${TEST_DIR}/${name}.avx")
    endif()
endfunction()
function(sync_reject name source pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${sync_header}\n${source}\n")
    sync_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()

sync_accept(mutex_core [=[
i32 main() {
    Mutex<i32> lock = Mutex.new(7);
    if (Mutex.is_poisoned(&lock)) return 1;
    {
        mut MutexGuard<i32> guard = success(Mutex.lock(&lock));
        if (*Mutex.data(&guard) != 7 || *MutexGuard.as_ref(&guard) != 7) return 2;
        *Mutex.data_mut(&mut guard) = 11;
        if (*MutexGuard.as_ref(&guard) != 11) return 3;
        Optional<MutexGuard<i32>> busy = success(Mutex.try_lock(&lock));
        if (!Optional.is_none(&busy)) return 4;
        Optional<MutexGuard<i32>> timed = success(Mutex.lock_timeout(&lock, Duration.from_nanos(0)));
        if (!Optional.is_none(&timed)) return 5;
        Mutex.unlock(guard);
    }
    {
        mut MutexGuard<i32> guard = present(success(Mutex.try_lock(&lock)));
        *MutexGuard.as_mut(&mut guard) += 2;
    }
    return success(Mutex.into_inner(lock)) == 13 ? 0 : 6;
}
]=])

sync_accept(mutex_poison [=[
i32 main() {
    Mutex<i32> lock = Mutex.with_policy(3, PoisonPolicy.Manual);
    {
        mut MutexGuard<i32> guard = success(Mutex.lock(&lock));
        Mutex.mark_poisoned(&mut guard);
    }
    if (!Mutex.is_poisoned(&lock)) return 1;
    Result<MutexGuard<i32>, LockError<MutexGuard<i32>>> acquired = Mutex.lock(&lock);
    switch (move acquired) {
        case Result<MutexGuard<i32>, LockError<MutexGuard<i32>>>.Ok: return 2; break;
        case Result<MutexGuard<i32>, LockError<MutexGuard<i32>>>.Err { LockError<MutexGuard<i32>> error = error }:
            switch (move error) {
                case LockError<MutexGuard<i32>>.Poisoned { MutexGuard<i32> held = guard }:
                    mut MutexGuard<i32> recovered = held;
                    *Mutex.data_mut(&mut recovered) = 17;
                    Mutex.clear_poison(&mut recovered);
                    break;
                case LockError<MutexGuard<i32>>.ReaderOverflow: return 3; break;
                case LockError<MutexGuard<i32>>.InvalidTimeout: return 4; break;
            }
            break;
    }
    if (Mutex.is_poisoned(&lock)) return 5;
    return success(Mutex.into_inner(lock)) == 17 ? 0 : 6;
}
]=])

sync_accept(rwlock_core [=[
i32 main() {
    RwLock<i32> lock = RwLock.new(7);
    {
        RwLockReadGuard<i32> a = success(RwLock.read(&lock));
        RwLockReadGuard<i32> b = present(success(RwLock.try_read(&lock)));
        if (*RwLock.read_data(&a) != 7 || *RwLockReadGuard.as_ref(&b) != 7) return 1;
        Optional<RwLockWriteGuard<i32>> busy = success(RwLock.try_write(&lock));
        if (!Optional.is_none(&busy)) return 2;
        Optional<RwLockWriteGuard<i32>> timed = success(RwLock.write_timeout(&lock, Duration.from_nanos(0)));
        if (!Optional.is_none(&timed)) return 3;
        RwLock.unlock_read(b);
        RwLock.unlock_read(a);
    }
    {
        mut RwLockWriteGuard<i32> writer = success(RwLock.write(&lock));
        *RwLock.write_data_mut(&mut writer) = 13;
        if (*RwLock.write_data(&writer) != 13 || *RwLockWriteGuard.as_ref(&writer) != 13) return 4;
        *RwLockWriteGuard.as_mut(&mut writer) += 4;
        Optional<RwLockReadGuard<i32>> blocked = success(RwLock.try_read(&lock));
        if (!Optional.is_none(&blocked)) return 5;
        Optional<RwLockReadGuard<i32>> timed = success(RwLock.read_timeout(&lock, Duration.from_nanos(0)));
        if (!Optional.is_none(&timed)) return 6;
        RwLock.unlock_write(writer);
    }
    return success(RwLock.into_inner(lock)) == 17 ? 0 : 7;
}
]=])

sync_accept(unique_and_owned_drop [=[
mut i32 drops = 0;
struct Item implements Drop { public i32 value; public void drop(&mut Self self) { drops += self.value; return; } };
i32 main() {
    {
        Mutex<Item> lock = Mutex.new(Item { value = 3 });
        { MutexGuard<Item> guard = success(Mutex.lock(&lock)); if (Mutex.data(&guard).value != 3 || drops != 0) return 1; }
        if (drops != 0) return 2;
        Item taken = success(Mutex.into_inner(lock));
        if (taken.value != 3 || drops != 0) return 3;
    }
    if (drops != 3) return 4;
    { RwLock<Item> lock = RwLock.new(Item { value = 5 }); { RwLockReadGuard<Item> guard = success(RwLock.read(&lock)); if (RwLock.read_data(&guard).value != 5) return 5; } }
    if (drops != 8) return 6;
    mut Mutex<i32> unique = Mutex.new(10);
    { &mut i32 inner = success(Mutex.get_mut(&mut unique)); *inner = 12; }
    if (success(Mutex.into_inner(unique)) != 12) return 7;
    mut RwLock<i32> unique_rw = RwLock.new(15);
    { &mut i32 inner = success(RwLock.get_mut(&mut unique_rw)); *inner = 19; }
    return success(RwLock.into_inner(unique_rw)) == 19 ? 0 : 8;
}
]=])

sync_accept(poisoned_owners_and_policy [=[
i32 main() {
    mut Mutex<i32> lock = Mutex.with_policy(3, PoisonPolicy.Manual);
    switch (Mutex.poison_policy(&lock)) { case PoisonPolicy.Manual: break; case PoisonPolicy.PoisonOnUnwind: return 1; break; }
    { mut MutexGuard<i32> guard = success(Mutex.lock(&lock)); Mutex.mark_poisoned(&mut guard); }
    {
        Result<&mut i32, LockError<&mut i32>> borrowed = Mutex.get_mut(&mut lock);
        switch (move borrowed) {
            case Result<&mut i32, LockError<&mut i32>>.Ok: return 2; break;
            case Result<&mut i32, LockError<&mut i32>>.Err { LockError<&mut i32> error = error }:
                if (!string.equals(error.code(), "sync.poisoned")) return 3;
                switch (move error) {
                    case LockError<&mut i32>.Poisoned { &mut i32 held = guard }: *held = 31; break;
                    case LockError<&mut i32>.ReaderOverflow: return 4; break;
                    case LockError<&mut i32>.InvalidTimeout: return 5; break;
                }
                break;
        }
    }
    Result<i32, Poisoned<i32>> extracted = Mutex.into_inner(lock);
    switch (move extracted) {
        case Result<i32, Poisoned<i32>>.Ok: return 6; break;
        case Result<i32, Poisoned<i32>>.Err { Poisoned<i32> error = error }:
            if (!string.equals(error.code(), "sync.poisoned") || !string.equals(error.message(), "The value was extracted from poisoned storage.")) return 7;
            if (Poisoned.into_inner(error) != 31) return 8;
            break;
    }
    RwLock<i32> reader_writer = RwLock.with_policy(41, PoisonPolicy.Manual);
    switch (RwLock.poison_policy(&reader_writer)) { case PoisonPolicy.Manual: break; case PoisonPolicy.PoisonOnUnwind: return 9; break; }
    { mut RwLockWriteGuard<i32> guard = success(RwLock.write(&reader_writer)); RwLock.mark_poisoned(&mut guard); }
    if (!RwLock.is_poisoned(&reader_writer)) return 10;
    {
        Result<Optional<RwLockReadGuard<i32>>, LockError<RwLockReadGuard<i32>>> observed = RwLock.try_read(&reader_writer);
        switch (move observed) {
            case Result<Optional<RwLockReadGuard<i32>>, LockError<RwLockReadGuard<i32>>>.Ok: return 11; break;
            case Result<Optional<RwLockReadGuard<i32>>, LockError<RwLockReadGuard<i32>>>.Err { LockError<RwLockReadGuard<i32>> error = error }:
                switch (move error) {
                    case LockError<RwLockReadGuard<i32>>.Poisoned { RwLockReadGuard<i32> held = guard }: if (*RwLock.read_data(&held) != 41) return 12; break;
                    case LockError<RwLockReadGuard<i32>>.ReaderOverflow: return 13; break;
                    case LockError<RwLockReadGuard<i32>>.InvalidTimeout: return 14; break;
                }
                break;
        }
    }
    {
        Result<RwLockWriteGuard<i32>, LockError<RwLockWriteGuard<i32>>> acquired = RwLock.write(&reader_writer);
        switch (move acquired) {
            case Result<RwLockWriteGuard<i32>, LockError<RwLockWriteGuard<i32>>>.Ok: return 15; break;
            case Result<RwLockWriteGuard<i32>, LockError<RwLockWriteGuard<i32>>>.Err { LockError<RwLockWriteGuard<i32>> error = error }:
                switch (move error) {
                    case LockError<RwLockWriteGuard<i32>>.Poisoned { RwLockWriteGuard<i32> held = guard }:
                        mut RwLockWriteGuard<i32> recovered = held;
                        RwLock.mark_poisoned(&mut recovered);
                        RwLock.clear_poison(&mut recovered);
                        break;
                    case LockError<RwLockWriteGuard<i32>>.ReaderOverflow: return 16; break;
                    case LockError<RwLockWriteGuard<i32>>.InvalidTimeout: return 17; break;
                }
                break;
        }
    }
    if (RwLock.is_poisoned(&reader_writer)) return 18;
    return success(RwLock.into_inner(reader_writer)) == 41 ? 0 : 19;
}
]=])

sync_accept(timeouts_and_error_drop [=[
i32 main() {
    Mutex<i32> lock = Mutex.new(7);
    {
        Result<Optional<MutexGuard<i32>>, LockError<MutexGuard<i32>>> invalid = Mutex.lock_timeout(&lock, Duration.from_nanos(9223372036854775808u64));
        switch (move invalid) {
            case Result<Optional<MutexGuard<i32>>, LockError<MutexGuard<i32>>>.Ok: return 1; break;
            case Result<Optional<MutexGuard<i32>>, LockError<MutexGuard<i32>>>.Err { LockError<MutexGuard<i32>> error = error }:
                if (!string.equals(error.code(), "sync.invalid_timeout")) return 2;
                break;
        }
    }
    { MutexGuard<i32> guard = present(success(Mutex.lock_timeout(&lock, Duration.from_nanos(0)))); if (*Mutex.data(&guard) != 7) return 3; }
    { mut MutexGuard<i32> guard = success(Mutex.lock(&lock)); Mutex.mark_poisoned(&mut guard); }
    { Result<Optional<MutexGuard<i32>>, LockError<MutexGuard<i32>>> ignored_poison = Mutex.try_lock(&lock); }
    {
        Result<Optional<MutexGuard<i32>>, LockError<MutexGuard<i32>>> again = Mutex.try_lock(&lock);
        switch (move again) {
            case Result<Optional<MutexGuard<i32>>, LockError<MutexGuard<i32>>>.Ok: return 4; break;
            case Result<Optional<MutexGuard<i32>>, LockError<MutexGuard<i32>>>.Err { LockError<MutexGuard<i32>> error = error }:
                if (!string.equals(error.code(), "sync.poisoned")) return 5;
                break;
        }
    }
    RwLock<i32> rw = RwLock.new(13);
    { RwLockReadGuard<i32> guard = present(success(RwLock.read_timeout(&rw, Duration.from_nanos(0)))); if (*RwLock.read_data(&guard) != 13) return 6; }
    { RwLockWriteGuard<i32> guard = present(success(RwLock.write_timeout(&rw, Duration.from_nanos(0)))); if (*RwLock.write_data(&guard) != 13) return 7; }
    return success(RwLock.into_inner(rw)) == 13 ? 0 : 8;
}
]=])

sync_accept(poisoned_owned_drop [=[
mut i32 drops = 0;
struct Item implements Drop { public i32 value; public void drop(&mut Self self) { drops += self.value; return; } };
MutexGuard<Item> acquire(&Mutex<Item> owner) { return success(Mutex.lock(owner)); }
&Item inspect(&MutexGuard<Item> guard) { return Mutex.data(guard); }
i32 main() {
    {
        Mutex<Item> lock = Mutex.new(Item { value = 3 });
        { mut MutexGuard<Item> guard = acquire(&lock); if (inspect(&guard).value != 3) return 1; Mutex.mark_poisoned(&mut guard); }
        Result<Item, Poisoned<Item>> extracted = Mutex.into_inner(lock);
        switch (move extracted) {
            case Result<Item, Poisoned<Item>>.Ok: return 2; break;
            case Result<Item, Poisoned<Item>>.Err { Poisoned<Item> error = error }:
                Item recovered = Poisoned.into_inner(error);
                if (recovered.value != 3 || drops != 0) return 3;
                break;
        }
    }
    if (drops != 3) return 4;
    {
        RwLock<Item> lock = RwLock.new(Item { value = 5 });
        { mut RwLockWriteGuard<Item> guard = success(RwLock.write(&lock)); RwLock.mark_poisoned(&mut guard); }
        { Result<RwLockReadGuard<Item>, LockError<RwLockReadGuard<Item>>> ignored = RwLock.read(&lock); }
        if (drops != 3) return 5;
        Result<Item, Poisoned<Item>> abandoned = RwLock.into_inner(lock);
    }
    return drops == 8 ? 0 : 6;
}
]=])

sync_reject(local_guard_escape [=[
MutexGuard<i32> escape() { Mutex<i32> local = Mutex.new(1); return success(Mutex.lock(&local)); }
i32 main() { MutexGuard<i32> guard = escape(); return *Mutex.data(&guard); }
]=] "borrow|Borrow|local|lifetime")
sync_reject(guard_reference_escape [=[
&i32 escape(&Mutex<i32> lock) { MutexGuard<i32> guard = success(Mutex.lock(lock)); return Mutex.data(&guard); }
i32 main() { Mutex<i32> lock = Mutex.new(1); return *escape(&lock); }
]=] "borrow|Borrow|local|lifetime")
sync_reject(owner_move_with_guard [=[
i32 main() { Mutex<i32> lock = Mutex.new(1); MutexGuard<i32> guard = success(Mutex.lock(&lock)); i32 value = success(Mutex.into_inner(lock)); return *Mutex.data(&guard) + value; }
]=] "borrow|Borrow|move|Move")
sync_reject(unlock_with_data_borrow [=[
i32 main() { Mutex<i32> lock = Mutex.new(1); MutexGuard<i32> guard = success(Mutex.lock(&lock)); &i32 value = Mutex.data(&guard); Mutex.unlock(guard); return *value; }
]=] "borrow|Borrow|move|Move")
sync_reject(read_guard_mutation [=[
i32 main() { RwLock<i32> lock = RwLock.new(1); mut RwLockReadGuard<i32> guard = success(RwLock.read(&lock)); *RwLockReadGuard.as_ref(&guard) = 2; return 0; }
]=] "read-only|mutable|Mutable|modify")
sync_reject(guard_copy [=[
struct Bad implements Copy { public MutexGuard<i32> guard; };
i32 main() { return 0; }
]=] "Copy|copy")
sync_reject(private_payload_forgery [=[
i32 main() { MutexGuard<i32> guard = MutexGuard<i32> { payload = 1 }; return 0; }
]=] "private|Private|compiler|Compiler")
sync_reject(mutex_forgery [=[
i32 main() { Mutex<i32> lock = Mutex<i32> { payload = 1 }; return 0; }
]=] "private|Private|compiler|Compiler")
sync_reject(moved_guard_reuse [=[
i32 main() { Mutex<i32> lock = Mutex.new(1); MutexGuard<i32> guard = success(Mutex.lock(&lock)); Mutex.unlock(guard); return *Mutex.data(&guard); }
]=] "move|Move|uninitialized")
sync_reject(shared_unique_bypass [=[
i32 main() { Mutex<i32> lock = Mutex.new(1); &Mutex<i32> shared = &lock; &mut i32 value = success(Mutex.get_mut(shared)); return *value; }
]=] "mutable|Mutable|compatible|requires")
sync_reject(native_abi_spoof [=[
extern "C" i32 aevix_mutex_init(u64 address);
i32 main() { return aevix_mutex_init(0); }
]=] "reserved|Reserved|runtime|Runtime")

sync_accept(contained_reference_write [=[
&i32 relay(&MutexGuard<&i32> guard) { return *Mutex.data(guard); }
i32 main() {
    i32 first = 7;
    i32 second = 13;
    Mutex<&i32> lock = Mutex.new(&first);
    {
        mut MutexGuard<&i32> guard = success(Mutex.lock(&lock));
        *Mutex.data_mut(&mut guard) = &second;
        if (*relay(&guard) != 13) return 1;
    }
    { MutexGuard<&i32> guard = success(Mutex.lock(&lock)); if (**Mutex.data(&guard) != 13) return 2; }
    return 0;
}
]=])

sync_reject(helper_guard_reference_escape [=[
&i32 view(&MutexGuard<i32> guard) { return Mutex.data(guard); }
&i32 relay(&MutexGuard<i32> guard) { return view(guard); }
&i32 escape(&Mutex<i32> lock) { MutexGuard<i32> guard = success(Mutex.lock(lock)); return relay(&guard); }
i32 main() { Mutex<i32> lock = Mutex.new(1); return *escape(&lock); }
]=] "borrow|Borrow|local|lifetime")

sync_reject(short_reference_write [=[
i32 main() {
    i32 outer = 1;
    Mutex<&i32> lock = Mutex.new(&outer);
    {
        i32 short_lived = 2;
        mut MutexGuard<&i32> guard = success(Mutex.lock(&lock));
        *Mutex.data_mut(&mut guard) = &short_lived;
    }
    MutexGuard<&i32> guard = success(Mutex.lock(&lock));
    return **Mutex.data(&guard);
}
]=] "borrow|Borrow|local|lifetime|escape")

sync_reject(helper_short_reference_write [=[
void store(&mut MutexGuard<&i32> guard, &i32 input) { *Mutex.data_mut(guard) = input; return; }
i32 main() {
    i32 outer = 1;
    Mutex<&i32> lock = Mutex.new(&outer);
    {
        i32 short_lived = 2;
        mut MutexGuard<&i32> guard = success(Mutex.lock(&lock));
        store(&mut guard, &short_lived);
    }
    MutexGuard<&i32> guard = success(Mutex.lock(&lock));
    return **Mutex.data(&guard);
}
]=] "borrow|Borrow|local|lifetime|escape|nested")

sync_accept(inline_construction [=[
i32 extract(Result<i32, Poisoned<i32>> input) {
    switch (move input) {
        case Result<i32, Poisoned<i32>>.Ok { i32 value = value }: return value; break;
        case Result<i32, Poisoned<i32>>.Err { Poisoned<i32> error = error }: return Poisoned.into_inner(error); break;
    }
}
i32 main() {
    Mutex<i32> first = Mutex.new(17);
    Mutex<i32> moved = first;
    Result<i32, Poisoned<i32>> typed_mutex = Mutex.into_inner(moved);
    if (extract(typed_mutex) != 17) return 1;
    Mutex<i32> second = Mutex.new(23);
    if (success(Mutex.into_inner(second)) != 23) return 2;
    RwLock<i32> third = RwLock.new(31);
    RwLock<i32> moved_rw = third;
    Result<i32, Poisoned<i32>> typed_rw = RwLock.into_inner(moved_rw);
    if (extract(typed_rw) != 31) return 3;
    RwLock<i32> fourth = RwLock.new(37);
    if (success(RwLock.into_inner(fourth)) != 37) return 4;
    Poisoned<i32> retained = Poisoned<i32> { value = 41 };
    return Poisoned.into_inner(retained) == 41 ? 0 : 5;
}
]=])

# 실제 소스의 생성·이동·추출 경로에 숨겨진 할당이나 초기화 호출이 없음을 확인한다.
# Verify that actual source construction, movement, and extraction contain no hidden allocation or init calls.
execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/inline_construction.avx"
    RESULT_VARIABLE inline_status OUTPUT_VARIABLE inline_ir ERROR_VARIABLE inline_error TIMEOUT 90)
if(NOT "${inline_status}" STREQUAL "0")
    message(FATAL_ERROR "inline construction IR: ${inline_status}\n${inline_error}")
endif()
if(inline_ir MATCHES "call [^\n]*@(malloc|calloc|realloc|aevix_shared_prepare|aevix_task_prepare|aevix_mutex_init|aevix_rwlock_init)\\(")
    message(FATAL_ERROR "inline construction unexpectedly allocates or calls native initialization\n${inline_ir}")
endif()

# 선언 위치와 여러 독립 프로세스에서 명목 타입과 같은 이름의 변형이 선택되지 않는지 검사한다.
# Vary declaration pressure and fresh processes so a same-named enum variant never replaces the nominal type.
file(READ "${TEST_DIR}/inline_construction.avx" inline_source)
foreach(pressure IN ITEMS 0 1 2 4 8 16 32)
    set(prefix "")
    if(pressure GREATER 0)
        foreach(index RANGE 1 ${pressure})
            string(APPEND prefix "const i32 sync_pressure_${index} = ${index};\n")
        endforeach()
    endif()
    # import는 파일 맨 위에 유지하고 그 뒤에 무관한 선언을 삽입한다.
    # Keep imports first and insert unrelated declarations after the shared source header.
    file(WRITE "${TEST_DIR}/inline_pressure_${pressure}.avx" "${sync_header}\n${prefix}\n")
    string(LENGTH "${sync_header}" header_size)
    string(SUBSTRING "${inline_source}" ${header_size} -1 inline_body)
    file(APPEND "${TEST_DIR}/inline_pressure_${pressure}.avx" "${inline_body}")
    foreach(repeat RANGE 1 3)
        sync_run("inline type identity pressure ${pressure}/${repeat}" 0 "" --check "${TEST_DIR}/inline_pressure_${pressure}.avx")
    endforeach()
endforeach()

sync_accept(helper_write_valid [=[
&mut &i32 view(&mut MutexGuard<&i32> guard) { return Mutex.data_mut(guard); }
&mut &i32 relay(&mut MutexGuard<&i32> guard) { return view(guard); }
i32 main() { i32 first=1; i32 second=2; Mutex<&i32> lock=Mutex.new(&first); { mut MutexGuard<&i32> guard=success(Mutex.lock(&lock)); *relay(&mut guard)=&second; if (**Mutex.data(&guard)!=2) return 1; } { MutexGuard<&i32> guard=success(Mutex.lock(&lock)); if (**Mutex.data(&guard)!=2) return 2; } return 0; }
]=])

sync_reject(helper_write_newreferent_mutated [=[
&mut &i32 view(&mut MutexGuard<&i32> guard) { return Mutex.data_mut(guard); }
&mut &i32 relay(&mut MutexGuard<&i32> guard) { return view(guard); }
i32 main() { i32 first=1; mut i32 second=2; Mutex<&i32> lock=Mutex.new(&first); { mut MutexGuard<&i32> guard=success(Mutex.lock(&lock)); *relay(&mut guard)=&second; } second=3; MutexGuard<&i32> guard=success(Mutex.lock(&lock)); return **Mutex.data(&guard); }
]=] "borrow|Borrow|local|lifetime|move|Move|escape")

sync_reject(helper_write_short [=[
&mut &i32 view(&mut MutexGuard<&i32> guard) { return Mutex.data_mut(guard); }
&mut &i32 relay(&mut MutexGuard<&i32> guard) { return view(guard); }
i32 main() { i32 first=1; Mutex<&i32> lock=Mutex.new(&first); { i32 second=2; mut MutexGuard<&i32> guard=success(Mutex.lock(&lock)); *relay(&mut guard)=&second; } MutexGuard<&i32> guard=success(Mutex.lock(&lock)); return **Mutex.data(&guard); }
]=] "borrow|Borrow|local|lifetime|move|Move|escape")

sync_reject(helper_view_guard_dropped [=[
&mut &i32 view(&mut MutexGuard<&i32> guard) { return Mutex.data_mut(guard); }
&mut &i32 relay(&mut MutexGuard<&i32> guard) { return view(guard); }
i32 main() { i32 first=1; Mutex<&i32> lock=Mutex.new(&first); mut MutexGuard<&i32> guard=success(Mutex.lock(&lock)); &mut &i32 held=relay(&mut guard); Mutex.unlock(guard); return **held; }
]=] "borrow|Borrow|local|lifetime|move|Move|escape")
