# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_shared_cli_tests.cmake
#
# Description :
#     공유·약한 소유권의 수명, 독점 접근, 오류 복구 및 종료 정리를 검사한다.
#     Check shared/weak lifetimes, exclusive access, error recovery, and cleanup.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(shared_header [=[
import memory { Shared, Weak, RefCountError };
T success<T, E>(Result<T, E> input) {
    switch (move input) { case Result<T, E>.Ok { T value = value }: return value; break; case Result<T, E>.Err: Never.unreachable(); break; }
}
T present<T>(Optional<T> input) {
    switch (move input) { case Optional<T>.Some { T value = value }: return value; break; case Optional<T>.None: Never.unreachable(); break; }
}
]=])
function(shared_run name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(shared_accept name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${shared_header}\n${source}\n")
    shared_run("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    shared_run("${name} AOT" 0 "" --run "${TEST_DIR}/${name}.avx")
    shared_run("${name} AOT O2" 0 "" --run -O2 "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        shared_run("${name} JIT" 0 "" --jit "${TEST_DIR}/${name}.avx")
        shared_run("${name} JIT O2" 0 "" --jit -O2 "${TEST_DIR}/${name}.avx")
    endif()
endfunction()
function(shared_reject name source pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${shared_header}\n${source}\n")
    shared_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()

shared_accept(strong_and_unique [=[
i32 main() {
    mut Shared<i32> owner = success(Shared.new(7));
    if (*Shared.as_ref(&owner) != 7 || Shared.strong_count(&owner) != 1 || Shared.weak_count(&owner) != 0) return 1;
    mut Weak<i32> weak = Weak.new<i32>();
    if (!Weak.is_empty(&weak) || !Weak.is_expired(&weak) || Weak.strong_count(&weak) != 0 || Weak.weak_count(&weak) != 0) return 2;
    {
        Shared<i32> clone = success(Shared.clone(&owner));
        if (!Shared.ptr_equal(&owner, &clone) || Shared.strong_count(&clone) != 2) return 3;
        Optional<&mut i32> unavailable = Shared.get_mut(&mut owner);
        if (!Optional.is_none(&unavailable)) return 4;
        weak = success(Shared.downgrade(&clone));
        if (Weak.is_empty(&weak) || Weak.is_expired(&weak) || Weak.weak_count(&weak) != 1) return 5;
        {
            Shared<i32> upgraded = present(success(Weak.upgrade(&weak)));
            if (!Shared.ptr_equal(&clone, &upgraded) || Shared.strong_count(&upgraded) != 3) return 6;
        }
    }
    if (Shared.strong_count(&owner) != 1 || Shared.weak_count(&owner) != 1) return 7;
    { Optional<&mut i32> unavailable = Shared.get_mut(&mut owner); if (!Optional.is_none(&unavailable)) return 8; }
    Weak.clear(&mut weak);
    if (!Weak.is_empty(&weak)) return 9;
    { &mut i32 value = present(Shared.get_mut(&mut owner)); *value = 9; }
    if (*Shared.as_ref(&owner) != 9) return 10;
    Result<i32, Shared<i32>> taken = Shared.try_unwrap(owner);
    return success(taken) == 9 ? 0 : 11;
}
]=])

shared_accept(expired_and_empty [=[
i32 main() {
    mut Weak<i32> weak = Weak.new<i32>();
    Weak<i32> empty_clone = success(Weak.clone(&weak));
    Optional<Shared<i32>> empty_upgrade = success(Weak.upgrade(&weak));
    if (!Weak.ptr_equal(&weak, &empty_clone) || !Optional.is_none(&empty_upgrade)) return 1;
    {
        Shared<i32> owner = success(Shared.new(17));
        weak = success(Shared.downgrade(&owner));
        if (Weak.ptr_equal(&weak, &empty_clone) || Weak.is_expired(&weak)) return 2;
        Weak<i32> cloned = success(Weak.clone(&weak));
        if (!Weak.ptr_equal(&weak, &cloned) || Weak.weak_count(&cloned) != 2) return 3;
    }
    if (!Weak.is_expired(&weak) || Weak.is_empty(&weak) || Weak.strong_count(&weak) != 0 || Weak.weak_count(&weak) != 1) return 4;
    Optional<Shared<i32>> unavailable = success(Weak.upgrade(&weak));
    if (!Optional.is_none(&unavailable)) return 5;
    Weak<i32> expired_clone = success(Weak.clone(&weak));
    if (Weak.weak_count(&weak) != 2 || !Weak.ptr_equal(&weak, &expired_clone)) return 6;
    Weak.clear(&mut weak);
    return Weak.is_empty(&weak) && Weak.is_expired(&expired_clone) ? 0 : 7;
}
]=])

shared_accept(unwrap_and_drop [=[
mut i32 drops = 0;
struct Item implements Drop { public i32 value; public void drop(&mut Self self) { drops += self.value; return; } };
i32 main() {
    mut Weak<Item> weak = Weak.new<Item>();
    {
        Shared<Item> first = success(Shared.new(Item { value = 3 }));
        weak = success(Shared.downgrade(&first));
        {
            Shared<Item> second = success(Shared.clone(&first));
            Result<Item, Shared<Item>> blocked = Shared.try_unwrap(second);
            switch (move blocked) {
                case Result<Item, Shared<Item>>.Err { Shared<Item> recovered = error }: if (Shared.strong_count(&recovered) != 2) return 7; break;
                case Result<Item, Shared<Item>>.Ok: return 1; break;
            }
        }
        if (drops != 0) return 2;
        Item value = success(Shared.try_unwrap(first));
        if (value.value != 3 || !Weak.is_expired(&weak) || drops != 0) return 3;
    }
    if (drops != 3) return 4;
    Weak.clear(&mut weak);
    {
        Shared<Item> first = success(Shared.new(Item { value = 5 }));
        Shared<Item> second = success(Shared.clone(&first));
        weak = success(Shared.downgrade(&second));
    }
    if (drops != 8 || !Weak.is_expired(&weak)) return 5;
    Weak.clear(&mut weak);
    return drops == 8 ? 0 : 6;
}
]=])

shared_accept(contained_references [=[
Shared<&i32> retained(&i32 input) { return success(Shared.new(input)); }
i32 main() {
    i32 value = 31;
    mut Optional<Shared<&i32>> saved = Optional.none();
    {
        Shared<&i32> first = retained(&value);
        saved = Optional.some(success(Shared.clone(&first)));
    }
    Shared<&i32> clone = present(saved);
    return **Shared.as_ref(&clone) == 31 ? 0 : 1;
}
]=])

shared_accept(exclusive_reference_paths [=[
struct Holder { public &mut i32 value; };
i32 main() {
    mut i32 first = 1;
    &mut i32 direct = &mut first;
    *direct = 2;
    mut &mut i32 nested = direct;
    &mut &mut i32 exclusive = &mut nested;
    **exclusive = 3;
    mut i32 second = 4;
    Holder owned = Holder { value = &mut second };
    *owned.value = 5;
    return **exclusive == 3 && *owned.value == 5 ? 0 : 1;
}
]=])

shared_reject(shared_mutable_reference [=[i32 main() { mut i32 value = 7; Shared<&mut i32> owner = success(Shared.new(&mut value)); & &mut i32 view = Shared.as_ref(&owner); **view = 9; return 0; }]=] "read-only|immutable")
shared_reject(shared_mutable_field [=[struct Holder { public &mut i32 value; }; i32 main() { mut i32 value = 7; Shared<Holder> owner = success(Shared.new(Holder { value = &mut value })); &Holder view = Shared.as_ref(&owner); *view.value = 9; return 0; }]=] "read-only|immutable")
shared_reject(shared_nested_reborrow [=[i32 main() { mut i32 value = 7; Shared<&mut i32> owner = success(Shared.new(&mut value)); & &mut i32 view = Shared.as_ref(&owner); &mut i32 alias = &mut **view; *alias = 9; return 0; }]=] "read-only|immutable")
shared_reject(shared_nested_move [=[void change(&mut i32 target) { *target = 9; return; } i32 main() { mut i32 value = 7; Shared<&mut i32> owner = success(Shared.new(&mut value)); & &mut i32 view = Shared.as_ref(&owner); change(*view); return 0; }]=] "borrow|loan|move|read-only")
shared_reject(shared_field_reborrow [=[struct Holder { public &mut i32 value; }; void change(&mut i32 target) { *target = 9; return; } i32 main() { mut i32 value = 7; Shared<Holder> owner = success(Shared.new(Holder { value = &mut value })); &Holder view = Shared.as_ref(&owner); change(view.value); return 0; }]=] "read-only|borrow|move")
shared_reject(shared_field_move [=[struct Holder { public &mut i32 value; }; i32 main() { mut i32 value = 7; Shared<Holder> owner = success(Shared.new(Holder { value = &mut value })); &Holder view = Shared.as_ref(&owner); &mut i32 escaped = view.value; *escaped = 9; return 0; }]=] "read-only|borrow|move")
shared_reject(readonly_nested_reference [=[i32 main() { mut i32 value = 7; &mut i32 direct = &mut value; & &mut i32 view = &direct; **view = 9; return 0; }]=] "read-only|immutable")

shared_reject(exclusive_payload_move [=[i32 main() { mut Shared<string> owner = success(Shared.new(success(string.from_view("owned")))); &mut string view = present(Shared.get_mut(&mut owner)); string stolen = *view; return 0; }]=] "borrowed place|ownership-preserving|move")
shared_reject(local_borrow_move [=[i32 main() { mut string owner = success(string.from_view("owned")); &mut string view = &mut owner; string stolen = *view; return 0; }]=] "borrowed place|ownership-preserving|move")
shared_reject(no_implicit_copy [=[i32 main() { Shared<i32> owner = success(Shared.new(1)); Shared<i32> moved = owner; return *Shared.as_ref(&owner); }]=] "move|unavailable")
shared_reject(no_private_construction [=[i32 main() { Shared<i32> forged = Shared<i32> { value = 7 }; return 0; }]=] "private|initializer|construct|nominal")
shared_reject(no_namespace_mismatch [=[i32 main() { Shared<f64> owner = success(Shared.new(1.0)); Shared<i32>.clone(&owner); return 0; }]=] "payload|match|type")
shared_reject(active_payload_view [=[i32 main() { Shared<i32> owner = success(Shared.new(1)); &i32 view = Shared.as_ref(&owner); Shared.try_unwrap(owner); return *view; }]=] "borrow|loan")
shared_reject(exclusive_requires_mut [=[i32 main() { Shared<i32> owner = success(Shared.new(1)); Shared.get_mut(&owner); return 0; }]=] "borrow|exclusive|mut")
shared_reject(escaped_owner [=[Shared<&i32> bad() { i32 local = 7; return success(Shared.new(&local)); } i32 main() { return 0; }]=] "local|outlive|reference")
shared_reject(escaped_clone [=[Shared<&i32> bad() { i32 local = 7; Shared<&i32> owner = success(Shared.new(&local)); return success(Shared.clone(&owner)); } i32 main() { return 0; }]=] "local|outlive|reference")
shared_reject(escaped_weak [=[Weak<&i32> bad() { i32 local = 7; Shared<&i32> owner = success(Shared.new(&local)); return success(Shared.downgrade(&owner)); } i32 main() { return 0; }]=] "local|outlive|reference")
message(STATUS "Shared/Weak ownership, lifetime, uniqueness and cleanup checks passed")
