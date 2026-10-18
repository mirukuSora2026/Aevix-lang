# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_memory_cli_tests.cmake
#
# Description :
#     봉인된 메모리 교체·교환의 소유권, 소멸, 참조 수명과 별칭 거부를 검사한다.
#     Check ownership, destruction, reference lifetimes, and alias rejection for sealed memory replacement and swap.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
function(memory_run name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(memory_accept name source)
    set(path "${TEST_DIR}/${name}.avx")
    file(WRITE "${path}" "import memory;\n${source}\n")
    memory_run("${name} check" 0 "" --check "${path}")
    memory_run("${name} AOT" 0 "" --run "${path}")
    memory_run("${name} optimized AOT" 0 "" --run -O2 "${path}")
    if(version MATCHES "ORC JIT enabled")
        memory_run("${name} JIT" 0 "" --jit "${path}")
    endif()
endfunction()
function(memory_reject name source pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "import memory;\n${source}\n")
    memory_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()

memory_accept(scalars [=[
T exchange<T>(&mut T target, T replacement) { return memory.replace(target, replacement); }
i32 main() {
    mut i32 first = 3;
    i32 old = memory.replace(&mut first, 5);
    if (old != 3 || first != 5) return 1;
    mut i32 second = 7;
    memory.swap<i32>(&mut first, &mut second);
    if (first != 7 || second != 5) return 2;
    if (exchange(&mut first, 11) != 7 || first != 11) return 3;
    return 0;
}
]=])
memory_accept(returning_switch [=[
T choose<T>(T value, bool early) {
    switch (early) { case true: return value; break; case false: break; }
    return value;
}
i32 main() { return choose(7, true) == 7 && choose(9, false) == 9 ? 0 : 1; }
]=])
memory_accept(owning_drop [=[
struct Counter implements Drop {
    public &mut i32 count;
    public void drop(&mut Self self) { *self.count += 1; return; }
};
struct Holder implements Drop {
    public Counter value;
    public void drop(&mut Self self) { return; }
};
i32 main() {
    mut i32 first = 0;
    mut i32 second = 0;
    {
        mut Holder holder = Holder { value = Counter { count = &mut first } };
        Counter prior = memory.replace(&mut holder.value, Counter { count = &mut second });
    }
    if (first != 1 || second != 1) return 1;
    {
        mut Counter left = Counter { count = &mut first };
        mut Counter right = Counter { count = &mut second };
        memory.swap(&mut left, &mut right);
    }
    return first == 2 && second == 2 ? 0 : 2;
}
]=])
memory_accept(enum_container [=[
enum Item { Empty; Value { public i32 value; }; };
Optional<T> take<T>(&mut Optional<T> source) { return memory.replace(source, Optional.none()); }
i32 main() {
    mut Optional<Item> source = Optional.some(Item.Value { value = 13 });
    Optional<Item> old = take(&mut source);
    switch (move old) {
        case Optional<Item>.Some { Item item = value }:
            switch (move item) { case Item.Value { i32 number = value }: if (number != 13) return 1; break; case Item.Empty: return 2; break; }
            break;
        case Optional<Item>.None: return 3; break;
    }
    return Optional.is_none(&source) ? 0 : 4;
}
]=])
memory_accept(generic_case_binding [=[
enum Slot<T> { Value { public T payload; }; };
struct Owner<T> {
    public Slot<T> slot;
    public T exchange<U>(&mut Self self, T replacement, U ignored) {
        switch (&mut self.slot) {
            case Slot<T>.Value { &mut T borrowed = &mut payload }:
                return memory.replace(borrowed, replacement);
                break;
        }
    }
};
i32 main() {
    mut Owner<i32> owner = Owner<i32> { slot = Slot<i32>.Value { payload = 7 } };
    return owner.exchange(9, true) == 7 ? 0 : 1;
}
]=])
memory_accept(conditional_owner_method [=[
struct Owner<T> {
    public T value;
    public T copy_value(&Self self) where T implements Copy { return self.value; }
    public i32 inspect(&Self self) { return 7; }
};
i32 main() {
    Owner<i32> numeric = Owner<i32> { value = 3 };
    Owner<Optional<i32>> owning = Owner<Optional<i32>> { value = Optional.some(2) };
    return numeric.copy_value() == 3 && owning.inspect() == 7 ? 0 : 1;
}
]=])
memory_accept(concrete_generic_overload [=[
struct Owner<T> { public T value; };
i32 read(&Owner<i32> owner) { return owner.value; }
i32 read<T>(&T owner) where T implements Copy { return 99; }
i32 main() { Owner<i32> owner = Owner<i32> { value = 7 }; return read(&owner) == 7 ? 0 : 1; }
]=])
memory_accept(generic_method_owner_bound [=[
struct Owner<T> {
    public usize count<const usize N>(Array<T, N> values) where T implements Copy { return N; }
};
i32 main() { i32 values[2] = [1, 2]; return Owner<i32>.count(values) == 2 ? 0 : 1; }
]=])
memory_accept(defaulted_nested_nominal [=[
struct Boxed<T, U = i32> where T implements Copy {
    public T value;
    public Result<Self, i32> new(T value) { return Result.ok(Self { value = value }); }
};
i32 main() {
    Result<Boxed<i8>, i32> first = Boxed<i8>.new(1i8);
    Result<Boxed<i8, i32>, i32> second = Boxed<i8>.new(2i8);
    switch (move first) { case Result<Boxed<i8>, i32>.Ok { Boxed<i8> value = value }: if (value.value != 1i8) return 1; break; case Result<Boxed<i8>, i32>.Err: return 2; break; }
    switch (move second) { case Result<Boxed<i8, i32>, i32>.Ok { Boxed<i8, i32> value = value }: return value.value == 2i8 ? 0 : 3; break; case Result<Boxed<i8, i32>, i32>.Err: return 4; break; }
}
]=])
file(WRITE "${TEST_DIR}/collision_types.avx" [=[
public enum Problem { Holder; };
Result<T, i32> relay_result<T>(Result<T, i32> value) { return value; }
public struct Holder<T> {
    public T value;
    public Result<Self, i32> new(T input) { return Result.ok(Self { value = input }); }
    public Result<Self, i32> forwarded(T input) { return relay_result(Self.new(input)); }
    public usize count<const usize N>(Array<T, N> input) where T implements Copy { return N; }
};
]=])
memory_accept(lexical_member_collision [=[
import collision_types;
i32 main() {
    i32 input[2] = [1, 2];
    if (collision_types.Holder<i32>.count(input) != 2) return 1;
    Result<collision_types.Holder<i32>, i32> result = collision_types.Holder<i32>.forwarded(7);
    switch (move result) {
        case Result<collision_types.Holder<i32>, i32>.Ok { collision_types.Holder<i32> owner = value }: return owner.value == 7 ? 0 : 2; break;
        case Result<collision_types.Holder<i32>, i32>.Err: return 3; break;
    }
}
]=])
memory_accept(array_copy_inference [=[
T identity<T>(T value) { return value; }
i32 main() {
    i32 values[2] = [3, 7];
    i32 copied[2] = identity(Array.from_copy(&values));
    Tuple<i32, bool> source = Tuple.make(9, true);
    Tuple<i32, bool> copied_tuple = identity(Tuple.copy(&source));
    return copied[0] == 3 && copied[1] == 7 && *Tuple.get<0>(&copied_tuple) == 9 ? 0 : 1;
}
]=])
memory_accept(interface_result_signature [=[
type Count = Result<usize, i32>;
interface Shape { public Count count(&Self self); public Count echo(&Self self, Count input); };
struct Static implements Shape {
    public Result<usize, i32> count(&Self self) { return Result.ok(3); }
    public Result<usize, i32> echo(&Self self, Result<usize, i32> input) { return input; }
};
i32 main() {
    Static shape = Static {};
    &dyn Shape view = &shape;
    Count counted = view.count();
    Count echoed = view.echo(counted);
    switch (move echoed) { case Count.Ok { usize value = value }: return value == 3 ? 0 : 1; break; case Count.Err: return 2; break; }
}
]=])
memory_accept(reference_payload [=[
&i32 previous(&i32 first, &i32 second) {
    mut &i32 current = first;
    return memory.replace(&mut current, second);
}
i32 main() {
    mut i32 first = 7;
    mut i32 second = 9;
    {
        mut &mut i32 current = &mut first;
        &mut i32 old = memory.replace(&mut current, &mut second);
        *old = 8;
        *current = 10;
    }
    if (first != 8 || second != 10) return 1;
    return *previous(&first, &second) == 8 ? 0 : 2;
}
]=])
memory_accept(tuple_projection [=[
i32 main() {
    mut Tuple<i32, i32> pair = Tuple.make(3, 7);
    i32 old = memory.replace(Tuple.get_mut<1>(&mut pair), 11);
    return old == 7 && *Tuple.get<1>(&pair) == 11 ? 0 : 1;
}
]=])
memory_accept(contained_reference_transfer [=[
struct Counted implements Drop {
    public &mut i32 count;
    public void drop(&mut Self self) { *self.count += 1; return; }
};
Optional<T> take<T>(&mut Optional<T> owner) { return memory.replace(owner, Optional.none()); }
Optional<T> relay<T>(&mut Optional<T> owner) { return take(owner); }
&i32 exchange(&mut &i32 target, &i32 input) { return memory.replace(target, input); }
i32 main() {
    mut i32 count = 0;
    {
        mut Optional<Counted> owner = Optional.some(Counted { count = &mut count });
        Optional<Counted> extracted = relay(&mut owner);
        if (!Optional.is_none(&owner) || !Optional.is_some(&extracted)) return 1;
    }
    if (count != 1) return 2;
    i32 first = 7;
    i32 second = 9;
    mut &i32 stored = &first;
    &i32 old = exchange(&mut stored, &second);
    return *old == 7 && *stored == 9 ? 0 : 3;
}
]=])
memory_accept(array_and_named [=[
mut i32 sequence = 0;
i32 replacement() { sequence = sequence * 10 + 1; return 9; }
&mut i32 target(&mut i32 value) { sequence = sequence * 10 + 2; return value; }
i32 main() {
    mut i32 values[2] = [3, 5];
    i32 old = memory.replace(replacement = replacement(), target = target(&mut values[0]));
    if (old != 3 || values[0] != 9 || sequence != 12) return 1;
    mut i32 other = values[1];
    memory.swap(&mut values[0], &mut other);
    values[1] = other;
    return values[0] == 5 && values[1] == 9 ? 0 : 2;
}
]=])

memory_reject(readonly_target [=[i32 main() { i32 value = 1; return memory.replace(&value, 2); }]=] "memory|exclusive|type")
memory_reject(wrong_value [=[i32 main() { mut i32 value = 1; return memory.replace(&mut value, true); }]=] "memory|type")
memory_reject(alias_swap [=[i32 main() { mut i32 value = 1; memory.swap(&mut value, &mut value); return 0; }]=] "borrow|loan")
memory_reject(existing_loan [=[i32 main() { mut i32 value = 1; &i32 view = &value; memory.replace(&mut value, 2); return *view; }]=] "borrow|loan")
memory_reject(uninitialized [=[i32 main() { mut i32 value; return memory.replace(&mut value, 2); }]=] "unavailable|uninitialized|initializ")
memory_reject(duplicate_argument [=[i32 main() { mut i32 value = 1; return memory.replace(target = &mut value, target = &mut value); }]=] "duplicate")
memory_reject(escaped_reference [=[
&i32 bad() { i32 local = 1; mut &i32 reference = &local; return memory.replace(&mut reference, &local); }
i32 main() { return 0; }
]=] "local|outlive|reference")
memory_reject(short_replacement [=[
i32 main() { i32 outer = 1; mut &i32 stored = &outer; { i32 inner = 2; memory.replace(&mut stored, &inner); } return *stored; }
]=] "outlive|local|reference")
memory_reject(projected_move_still_rejected [=[
struct Item { public i32 value; };
struct Holder implements Drop { public Item item; public void drop(&mut Self self) { return; } };
i32 main() { Holder holder = Holder { item = Item { value = 1 } }; Item moved = holder.item; return moved.value; }
]=] "Drop|projected|move")
memory_reject(switch_move_rejoins [=[
void consume<T>(T value) { return; }
T invalid<T>(T value, bool consumed) {
    switch (consumed) { case true: consume(value); break; case false: break; }
    return value;
}
i32 main() { return invalid(1, true); }
]=] "possible move|after.*move")
memory_reject(unsatisfied_owner_method [=[
struct Owner<T> { public T value; public T copy_value(&Self self) where T implements Copy { return self.value; } };
i32 main() { Owner<Optional<i32>> owner = Owner<Optional<i32>> { value = Optional.some(1) }; rsn invalid = owner.copy_value(); return 0; }
]=] "requirements|Copy|constraint")
memory_reject(conditional_interface_implementation [=[
interface Readable { public i32 read(&Self self); };
struct Owner<T> implements Readable { public T value; public i32 read(&Self self) where T implements Copy { return 7; } };
i32 main() { Owner<Optional<i32>> owner = Owner<Optional<i32>> { value = Optional.some(1) }; &dyn Readable view = &owner; return view.read(); }
]=] "requirements|contract|interface")
memory_reject(interface_result_wrong_error [=[
interface Shape { public Result<usize, i32> count(&Self self); };
struct Static implements Shape { public Result<usize, bool> count(&Self self) { return Result.ok(3); } };
i32 main() { return 0; }
]=] "return type|contract")
memory_reject(wrapper_short_replacement [=[
void install(&mut &i32 target, &i32 input) { memory.replace(target, input); return; }
void relay(&mut &i32 target, &i32 input) { install(target, input); return; }
i32 main() { i32 outer = 1; mut &i32 stored = &outer; { i32 inner = 2; relay(&mut stored, &inner); } return *stored; }
]=] "outlive|local|reference")
memory_reject(wrapper_retained_owner [=[
struct Owner { public &i32 value; };
void install(&mut Optional<Owner> target, Owner input) { memory.replace(target, Optional.some(input)); return; }
i32 main() { mut Optional<Owner> stored = Optional.none(); { i32 local = 2; install(&mut stored, Owner { value = &local }); } return Optional.is_some(&stored) ? 0 : 1; }
]=] "outlive|local|reference")
memory_reject(extracted_reference_stays_live [=[
Optional<T> take<T>(&mut Optional<T> target) { return memory.replace(target, Optional.none()); }
i32 main() { mut i32 value = 2; mut Optional<&mut i32> stored = Optional.some(&mut value); Optional<&mut i32> extracted = take(&mut stored); value = 3; return Optional.is_some(&extracted) ? 0 : 1; }
]=] "borrow|loan")
memory_reject(array_implicit_copy_bound [=[
T copy_value<T>(&T input) where T implements Copy { return *input; }
i32 main() { i32 values[2] = [1, 2]; i32 copied[2] = copy_value(&values); return copied[0]; }
]=] "Copy|requirements|implements")
memory_reject(tuple_implicit_copy_bound [=[
T copy_value<T>(&T input) where T implements Copy { return *input; }
i32 main() { Tuple<i32, bool> value = Tuple.make(1, true); rsn copied = copy_value(&value); return 0; }
]=] "Copy|requirements|implements")
message(STATUS "Sealed memory replacement and swap ownership checks passed")
