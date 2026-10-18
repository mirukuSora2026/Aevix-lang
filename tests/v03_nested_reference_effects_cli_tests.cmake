# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_nested_reference_effects_cli_tests.cmake
#
# Description :
#     보관된 수정 참조를 통한 호출의 필드 출처와 실제 원본 소유자 수명을 검사한다.
#     Check field provenance and original-owner lifetimes through stored mutable-reference calls.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
function(nested_run name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(nested_accept name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    nested_run("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    nested_run("${name} AOT" 0 "" --run "${TEST_DIR}/${name}.avx")
    nested_run("${name} AOT O2" 0 "" --run -O2 "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        nested_run("${name} JIT" 0 "" --jit "${TEST_DIR}/${name}.avx")
        nested_run("${name} JIT O2" 0 "" --jit -O2 "${TEST_DIR}/${name}.avx")
    endif()
endfunction()
function(nested_reject name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    nested_run("reject ${name}" 1 "borrow|Borrow|local|lifetime|escape" --check "${TEST_DIR}/${name}.avx")
endfunction()

nested_accept(reserve [=[
Result<unit, CollectionError> reserve<T>(&mut Vector<T> owner) { Vector.reserve(owner, 1)?; return Result.ok(Unit.value()); }
enum View<T> {
    private Handle { &mut Vector<T> owner; };
    public Self new(&mut Vector<T> owner) { return Self.Handle { owner = owner }; }
    public Result<unit, CollectionError> prepare(&mut Self self) {
        switch (&mut *self) {
            case Self.Handle { &mut &mut Vector<T> owner = &mut owner }: return reserve<T>(&mut **owner); break;
        }
    }
};
Result<i32, CollectionError> exercise() {
    i32 local = 7;
    mut Vector<&i32> owner = [&local]?;
    mut View<&i32> view = View<&i32>.new(&mut owner);
    view.prepare()?;
    return Result.ok(0);
}
i32 main(){Result<i32,CollectionError> result=exercise();switch(move result){case Result<i32,CollectionError>.Ok{i32 n=value}:return n;break;case Result<i32,CollectionError>.Err:return 90;break;}}

]=])

nested_accept(append_long_lived [=[
Result<unit, InputError<T>> append<T>(&mut Vector<T> owner, T input) { return Vector.push(owner, input); }
enum View<T> {
    private Handle { &mut Vector<T> owner; };
    public Self new(&mut Vector<T> owner) { return Self.Handle { owner = owner }; }
    public Result<unit, InputError<T>> push(&mut Self self, T input) {
        switch (&mut *self) {
            case Self.Handle { &mut &mut Vector<T> owner = &mut owner }: return append<T>(&mut **owner, input); break;
        }
    }
};
Result<i32, CollectionError> exercise() {
    i32 local = 7;
    mut Vector<&i32> owner = []?;
    {
        mut View<&i32> view = View<&i32>.new(&mut owner);
        Result<unit, InputError<&i32>> added = view.push(&local);
        if (!Result.is_ok(&added)) return Result.ok(90);
    }
    return Result.ok(*owner[0]);
}
i32 main(){Result<i32,CollectionError> result=exercise();switch(move result){case Result<i32,CollectionError>.Ok{i32 n=value}:return n == 7 ? 0 : 1;break;case Result<i32,CollectionError>.Err:return 90;break;}}

]=])

nested_reject(append_short_lived [=[
Result<unit, InputError<T>> append<T>(&mut Vector<T> owner, T input) { return Vector.push(owner, input); }
enum View<T> {
    private Handle { &mut Vector<T> owner; };
    public Self new(&mut Vector<T> owner) { return Self.Handle { owner = owner }; }
    public Result<unit, InputError<T>> push(&mut Self self, T input) {
        switch (&mut *self) {
            case Self.Handle { &mut &mut Vector<T> owner = &mut owner }: return append<T>(&mut **owner, input); break;
        }
    }
};
Result<i32, CollectionError> exercise() {
    mut Vector<&i32> owner = []?;
    {
        i32 local = 7;
        mut View<&i32> view = View<&i32>.new(&mut owner);
        Result<unit, InputError<&i32>> added = view.push(&local);
        if (!Result.is_ok(&added)) return Result.ok(90);
    }
    return Result.ok(*owner[0]);
}
i32 main(){Result<i32,CollectionError> result=exercise();switch(move result){case Result<i32,CollectionError>.Ok{i32 n=value}:return n;break;case Result<i32,CollectionError>.Err:return 90;break;}}

]=])

nested_reject(append_then_mutate [=[
Result<unit, InputError<T>> append<T>(&mut Vector<T> owner, T input) { return Vector.push(owner, input); }
enum View<T> {
    private Handle { &mut Vector<T> owner; };
    public Self new(&mut Vector<T> owner) { return Self.Handle { owner = owner }; }
    public Result<unit, InputError<T>> push(&mut Self self, T input) {
        switch (&mut *self) {
            case Self.Handle { &mut &mut Vector<T> owner = &mut owner }: return append<T>(&mut **owner, input); break;
        }
    }
};
Result<i32, CollectionError> exercise() {
    mut i32 local = 7;
    mut Vector<&i32> owner = []?;
    {
        mut View<&i32> view = View<&i32>.new(&mut owner);
        Result<unit, InputError<&i32>> added = view.push(&local);
        if (!Result.is_ok(&added)) return Result.ok(90);
    }
    local = 9;
    return Result.ok(*owner[0]);
}
i32 main(){Result<i32,CollectionError> result=exercise();switch(move result){case Result<i32,CollectionError>.Ok{i32 n=value}:return n;break;case Result<i32,CollectionError>.Err:return 90;break;}}

]=])

nested_accept(helper_store_valid [=[
import sync { Mutex,MutexGuard }; T success<T,E>(Result<T,E> x) {switch(move x){case Result<T,E>.Ok{T a=value}:return a;break;case Result<T,E>.Err:Never.unreachable();break;}}
void store(&mut MutexGuard<&i32> guard,&i32 input) { *Mutex.data_mut(guard)=input; return; }
void relay(&mut MutexGuard<&i32> guard,&i32 input) { store(guard,input); return; }
i32 main(){i32 first=1;i32 second=2;Mutex<&i32> lock=Mutex.new(&first);{mut MutexGuard<&i32> guard=success(Mutex.lock(&lock));relay(&mut guard,&second);if(**Mutex.data(&guard)!=2)return 1;}MutexGuard<&i32> guard=success(Mutex.lock(&lock));return **Mutex.data(&guard)==2?0:2;}
]=])

nested_reject(helper_store_short [=[
import sync { Mutex,MutexGuard }; T success<T,E>(Result<T,E> x) {switch(move x){case Result<T,E>.Ok{T a=value}:return a;break;case Result<T,E>.Err:Never.unreachable();break;}}
void store(&mut MutexGuard<&i32> guard,&i32 input) { *Mutex.data_mut(guard)=input; return; }
void relay(&mut MutexGuard<&i32> guard,&i32 input) { store(guard,input); return; }
i32 main(){i32 first=1;Mutex<&i32> lock=Mutex.new(&first);{i32 second=2;mut MutexGuard<&i32> guard=success(Mutex.lock(&lock));relay(&mut guard,&second);}MutexGuard<&i32> guard=success(Mutex.lock(&lock));return **Mutex.data(&guard);}
]=])

nested_reject(helper_store_mutate [=[
import sync { Mutex,MutexGuard }; T success<T,E>(Result<T,E> x) {switch(move x){case Result<T,E>.Ok{T a=value}:return a;break;case Result<T,E>.Err:Never.unreachable();break;}}
void store(&mut MutexGuard<&i32> guard,&i32 input) { *Mutex.data_mut(guard)=input; return; }
void relay(&mut MutexGuard<&i32> guard,&i32 input) { store(guard,input); return; }
i32 main(){i32 first=1;mut i32 second=2;Mutex<&i32> lock=Mutex.new(&first);{mut MutexGuard<&i32> guard=success(Mutex.lock(&lock));relay(&mut guard,&second);}second=3;MutexGuard<&i32> guard=success(Mutex.lock(&lock));return **Mutex.data(&guard);}
]=])

nested_accept(helper_write_valid [=[
import sync { Mutex, MutexGuard };
T success<T,E>(Result<T,E> value) { switch(move value) { case Result<T,E>.Ok { T output = value }: return output; break; case Result<T,E>.Err: Never.unreachable(); break; } }
&mut &i32 view(&mut MutexGuard<&i32> guard) { return Mutex.data_mut(guard); }
&mut &i32 relay(&mut MutexGuard<&i32> guard) { return view(guard); }
i32 main() { i32 first=1; i32 second=2; Mutex<&i32> lock=Mutex.new(&first); { mut MutexGuard<&i32> guard=success(Mutex.lock(&lock)); *relay(&mut guard)=&second; if (**Mutex.data(&guard)!=2) return 1; } { MutexGuard<&i32> guard=success(Mutex.lock(&lock)); if (**Mutex.data(&guard)!=2) return 2; } return 0; }

]=])

nested_reject(helper_write_short [=[
import sync { Mutex, MutexGuard };
T success<T,E>(Result<T,E> value) { switch(move value) { case Result<T,E>.Ok { T output = value }: return output; break; case Result<T,E>.Err: Never.unreachable(); break; } }
&mut &i32 view(&mut MutexGuard<&i32> guard) { return Mutex.data_mut(guard); }
&mut &i32 relay(&mut MutexGuard<&i32> guard) { return view(guard); }
i32 main() { i32 first=1; Mutex<&i32> lock=Mutex.new(&first); { i32 second=2; mut MutexGuard<&i32> guard=success(Mutex.lock(&lock)); *relay(&mut guard)=&second; } MutexGuard<&i32> guard=success(Mutex.lock(&lock)); return **Mutex.data(&guard); }

]=])

nested_reject(helper_write_newreferent_mutated [=[
import sync { Mutex, MutexGuard };
T success<T,E>(Result<T,E> value) { switch(move value) { case Result<T,E>.Ok { T output = value }: return output; break; case Result<T,E>.Err: Never.unreachable(); break; } }
&mut &i32 view(&mut MutexGuard<&i32> guard) { return Mutex.data_mut(guard); }
&mut &i32 relay(&mut MutexGuard<&i32> guard) { return view(guard); }
i32 main() { i32 first=1; mut i32 second=2; Mutex<&i32> lock=Mutex.new(&first); { mut MutexGuard<&i32> guard=success(Mutex.lock(&lock)); *relay(&mut guard)=&second; } second=3; MutexGuard<&i32> guard=success(Mutex.lock(&lock)); return **Mutex.data(&guard); }

]=])

nested_reject(helper_view_guard_dropped [=[
import sync { Mutex, MutexGuard };
T success<T,E>(Result<T,E> value) { switch(move value) { case Result<T,E>.Ok { T output = value }: return output; break; case Result<T,E>.Err: Never.unreachable(); break; } }
&mut &i32 view(&mut MutexGuard<&i32> guard) { return Mutex.data_mut(guard); }
&mut &i32 relay(&mut MutexGuard<&i32> guard) { return view(guard); }
i32 main() { i32 first=1; Mutex<&i32> lock=Mutex.new(&first); mut MutexGuard<&i32> guard=success(Mutex.lock(&lock)); &mut &i32 held=relay(&mut guard); Mutex.unlock(guard); return **held; }

]=])

nested_accept(consumed_field_cleanup [=[
struct Tracked implements Drop {
    public &mut i32 counter;
    public void drop(&mut Self self) { *self.counter += 1; return; }
};
Result<Tracked, i32> forward(Tracked input) {
    Optional<Tracked> stored = Optional.some(input);
    switch (move stored) {
        case Optional<Tracked>.Some { Tracked owned = value }: return Result.ok(owned); break;
        case Optional<Tracked>.None: return Result.err(1); break;
    }
}
i32 main() {
    mut i32 drops = 0;
    {
        Result<Tracked, i32> returned = forward(Tracked { counter = &mut drops });
        switch (move returned) {
            case Result<Tracked, i32>.Ok { Tracked owned = value }: break;
            case Result<Tracked, i32>.Err: return 1; break;
        }
    }
    return drops == 1 ? 0 : 2;
}
]=])

nested_reject(copied_guard_view [=[
import sync { Mutex, MutexGuard };
T success<T,E>(Result<T,E> x){switch(move x){case Result<T,E>.Ok{T a=value}:return a;break;case Result<T,E>.Err:Never.unreachable();break;}}
&i32 view(&MutexGuard<i32> guard){return Mutex.data(guard);}
i32 main(){Mutex<i32> lock=Mutex.new(1);MutexGuard<i32> guard=success(Mutex.lock(&lock));&i32 first=view(&guard);&i32 second=first;Mutex.unlock(guard);return *second;}

]=])

nested_accept(redistributed_fields [=[
struct References { public &i32 first; public &i32 second; };
void redirect(&mut References owner) { owner.second = owner.first; return; }
i32 main() {
    i32 first = 11;
    i32 second = 23;
    mut References owner = References { first = &first, second = &second };
    redirect(&mut owner);
    return *owner.second == 11 ? 0 : 1;
}
]=])

nested_reject(redistributed_field_escape [=[
struct References { public &i32 first; public &i32 second; };
void redirect(&mut References owner) { owner.second = owner.first; return; }
i32 main(){mut i32 first=1;i32 second=2;i32 replacement=3;mut &i32 held=&replacement;{mut References owner=References{first=&first,second=&second};redirect(&mut owner);held=owner.second;}first=7;return *held;}

]=])
