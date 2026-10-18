# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_box_extract_cli_tests.cmake
#
# Description :
#     전체 Box 추출의 CPU 실행, 정확한 정리, 보존 입력, 대여·import 거부를 검사한다.
#     Check whole-Box CPU execution, exact cleanup, retained input and borrow/import rejections.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
function(box_run name expected pattern)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 120)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(box_accept name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    box_run("${name} check" 0 "ownership checked" "${AEVIXC}" --check "${TEST_DIR}/${name}.avx")
    foreach(opt 0 2)
        box_run("${name} AOT O${opt}" 0 "" "${AEVIXC}" --run "-O${opt}" "${TEST_DIR}/${name}.avx")
        if(version MATCHES "ORC JIT enabled")
            box_run("${name} JIT O${opt}" 0 "" "${AEVIXC}" --jit "-O${opt}" "${TEST_DIR}/${name}.avx")
        endif()
    endforeach()
endfunction()
function(box_reject name pattern source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    box_run("reject ${name}" 1 "${pattern}" "${AEVIXC}" --check "${TEST_DIR}/${name}.avx")
endfunction()
file(WRITE "${TEST_DIR}/payload.avx" [==[
public struct Record {public i32 value;};
]==])
file(WRITE "${TEST_DIR}/factory.avx" [==[
import payload { Record };
import memory { Box };
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok {T value=value}:return value;break;default:Never.unreachable();break;}}
public Box<Record> make_box(){return success(Box.new(Record {value=23}));}
]==])
box_accept(scalars_zero_simd [==[
import simd { Simd };
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main() {
 Box<i32> number = success(Box.new(42)); if (Box.into_inner(owner = number) != 42) return 1;
 i128 wide = 170141183460469231731687303715884105727i128;
 Box<i128> big = success(Box.new(wide)); if (extract(big) != wide) return 2;
 Box<unit> unit_owner = success(Box.new(Unit.value())); unit empty = extract(unit_owner);
 i32 zero[0] = []; Box<Array<i32,0>> zero_owner = success(Box.new(zero)); i32 result[0] = extract(zero_owner);
 Simd<i32,4> lanes = Simd.splat<i32,4>(7); Box<Simd<i32,4>> packed = success(Box.new(lanes));
 Simd<i32,4> unpacked = extract(packed); return Simd.sum(unpacked) == 28 ? 0 : 3;
}
]==])
box_accept(nested_owners [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main() {
 Box<i32> first = success(Box.new(41)); Box<Box<i32>> outer = success(Box.new(first));
 Box<i32> inner = extract(outer); if (extract(inner) != 41) return 1;
 string text = success(string.from_view("retained ownership")); Box<string> text_owner = success(Box.new(text));
 string output = extract(text_owner); if (!string.equals(&output,"retained ownership")) return 2;
 Result<Vector<i32>,CollectionError> values = [3,4]; Box<Vector<i32>> vector_owner = success(Box.new(success(values)));
 Vector<i32> vector = extract(vector_owner); return vector[0] == 3 && vector[1] == 4 ? 0 : 3;
}
]==])
box_accept(external_loans [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
struct Views { public &i32 read; public &mut i32 write; };
&i32 take_read(Box<&i32> owner) { return Box.into_inner(owner); }
&mut i32 take_write(Box<&mut i32> owner) { return Box.into_inner(owner); }
i32 main() {
 i32 source = 41; &i32 borrowed = take_read(success(Box.new(&source))); if (*borrowed != 41) return 1;
 mut i32 number = 7; &mut i32 writable = take_write(success(Box.new(&mut number))); *writable = 9;
 if (number != 9) return 2;
 mut i32 another = 2; Box<Views> owner = success(Box.new(Views { read = &source, write = &mut another }));
 Views views = extract(owner); *views.write = *views.read; return *views.write == 41 ? 0 : 3;
}
]==])
box_accept(own_drop_and_early_failure [==[
import atomic { Atomic, MemoryOrder };
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
persist Atomic<i32> dropped = Atomic.new(0);
i32 count() { return success(Atomic.load(&dropped,MemoryOrder.Relaxed)); }
struct Owned implements Drop { public i32 value; public void drop(&mut Self self) { success(Atomic.fetch_add(&dropped,1,MemoryOrder.Relaxed)); return; } };
Result<i32,i32> failed() { return Result.err(7); }
i32 consume(Owned input, i32 next) { return input.value + next; }
Result<i32,i32> early() { Box<Owned> owner = success(Box.new(Owned { value = 11 })); return Result.ok(consume(Box.into_inner(owner),failed()?)); }
i32 main() {
 { Box<Owned> owner = success(Box.new(Owned { value = 42 })); Owned output = extract(owner); if (count() != 0 || output.value != 42) return 1; }
 if (count() != 1) return 2;
 Result<i32,i32> result = early(); return Result.is_err(&result) && count() == 2 ? 0 : 3;
}
]==])
box_accept(drop_with_external_loan [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
struct Owned implements Drop { public &mut i32 count; public void drop(&mut Self self) { *self.count += 1; return; } };
i32 main() { mut i32 count = 0; { Box<Owned> owner = success(Box.new(Owned { count = &mut count })); Owned output = extract(owner); } return count == 1 ? 0 : 1; }
]==])
box_accept(borrow_replace_composition [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main() {
 mut Box<Box<i32>> parent = success(Box.new(success(Box.new(7))));
 Box<i32> replacement = success(Box.new(9));
 Box<i32> removed = memory.replace(Box.as_mut(&mut parent),replacement);
 if (Box.into_inner(removed) != 7) return 1;
 return Box.into_inner(Box.into_inner(parent)) == 9 ? 0 : 2;
}
]==])
box_accept(source_shadow_identity [==[
import memory;
import memory;
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
struct Box { public i32 value; public i32 into_inner(Self owner) { return owner.value + 1; } };
i32 main() { Box local = Box { value = 6 }; i32 own = Box.into_inner(local); memory.Box<i32> actual = success(memory.Box.new(40)); return own == 7 && memory.Box.into_inner(actual) == 40 ? 0 : 1; }
]==])
box_accept(closure_payload [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main() { string value = success(string.from_view("capture")); rsn call = [value]() -> string { return value; }; rsn boxed = success(Box.new(call)); rsn unboxed = Box.into_inner(boxed); string output = unboxed(); return string.equals(&output,"capture") ? 0 : 1; }
]==])
box_accept(task_roots_and_worker [==[
import task { Task, TaskScope, TaskId, Executor, ScopeOptions, ScopeReport };
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
TaskScope open() { Executor executor = Executor.native_threads(); return success(Task.open_scope(&executor,ScopeOptions.defaults())); }
Task<i32> start(&TaskScope scope) { return success(Task.spawn_owned(scope,[]() -> i32 { return 42; })); }
i32 main() {
 TaskScope scope = open(); Task<i32> original = start(&scope); TaskId expected = Task.id(&original);
 Box<Task<i32>> owner = success(Box.new(original)); Task<i32> restored = extract(owner); if (!expected.equals(Task.id(&restored))) return 1;
 if (success(Task.join(restored)) != 42) return 2;
 Box<i32> captured = success(Box.new(13)); Task<i32> second = success(Task.spawn_owned(&scope,[captured]() -> i32 { return Box.into_inner(captured); }));
 if (success(Task.join(second)) != 13) return 3;
 ScopeReport report = success(Task.close_scope(scope));
 TaskScope empty = open(); Box<TaskScope> boxed_scope = success(Box.new(empty)); TaskScope restored_scope = extract(boxed_scope); success(Task.close_scope(restored_scope));
 return report.observed == 2u64 ? 0 : 4;
}
]==])
box_accept(type_alias_owner [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
type Owner = Box<i32>;i32 main(){Owner owner=success(Box.new(7));return Box.into_inner(owner)==7?0:1;}
]==])
box_accept(transferred_source_type [==[
import factory { make_box };import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main(){rsn owner=make_box();rsn value=Box.into_inner(owner);return value.value==23?0:1;}
]==])
box_accept(existing_views_and_dyn [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
interface Read {public i32 read(&Self self);};struct Item implements Read {public i32 value;public i32 read(&Self self){return self.value;}};i32 main(){mut Box<i32> value=success(Box.new(1));&mut i32 writable=Box.as_mut(&mut value);*writable=7;if(*Box.as_ref(&value)!=7)return 1;Box<Item> concrete=success(Box.new(Item {value=42}));Box<dyn Read> erased=Box.into_dyn(concrete);&dyn Read borrowed=Box.as_ref(&erased);return borrowed.read()==42?0:2;}
]==])
box_reject(double_move "moved|unavailable" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main() { Box<i32> owner = success(Box.new(1)); i32 value = Box.into_inner(owner); return Box.into_inner(owner); }
]==])
box_reject(borrowed_owner "owned Box" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main() { Box<i32> owner = success(Box.new(1)); return Box.into_inner(&owner); }
]==])
box_reject(live_read "borrow" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main() { Box<i32> owner = success(Box.new(1)); &i32 view = Box.as_ref(&owner); i32 value = extract(owner); return *view + value; }
]==])
box_reject(live_write "borrow" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main() { mut Box<i32> owner = success(Box.new(1)); &mut i32 view = Box.as_mut(&mut owner); i32 value = extract(owner); return *view + value; }
]==])
box_reject(wrong_named_argument "argument name" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main() { Box<i32> owner = success(Box.new(1)); return Box.into_inner(value = owner); }
]==])
box_reject(extra_argument "exactly one" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main() { Box<i32> owner = success(Box.new(1)); return Box.into_inner(owner,2); }
]==])
box_reject(explicit_payload_type "Only Box.new" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main() { Box<i32> owner = success(Box.new(1)); return Box.into_inner<i32>(owner); }
]==])
box_reject(wrong_result "initializer" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main() { Box<i32> owner = success(Box.new(1)); bool value = Box.into_inner(owner); return 0; }
]==])
box_reject(unsized_dyn "sized|dyn" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
interface Read {public i32 read(&Self self);}; struct Item implements Read { public i32 read(&Self self) {return 1;} }; i32 main() { Box<Item> concrete = success(Box.new(Item {})); Box<dyn Read> owner = Box.into_dyn(concrete); rsn payload = Box.into_inner(owner); return 0; }
]==])
box_reject(read_escape "local|reference" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
&i32 escape() { i32 value = 7; Box<&i32> owner = success(Box.new(&value)); return extract(owner); } i32 main() {return 0;}
]==])
box_reject(mut_escape "local|reference" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
&mut i32 escape() { mut i32 value = 7; Box<&mut i32> owner = success(Box.new(&mut value)); return extract(owner); } i32 main() {return 0;}
]==])
box_reject(held_mutation "borrow" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main() { mut i32 value = 7; Box<&i32> owner = success(Box.new(&value)); &i32 output = extract(owner); value = 9; return *output; }
]==])
box_reject(held_exclusive_alias "borrow" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main() { mut i32 value = 7; Box<&mut i32> owner = success(Box.new(&mut value)); &mut i32 output = extract(owner); &mut i32 conflicting = &mut value; *conflicting = 9; return *output; }
]==])
box_reject(owned_drop_loan_escape "local|reference" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
struct Owned implements Drop {public &mut i32 value;public void drop(&mut Self self){*self.value+=1;return;}}; Owned escape(){mut i32 local=0;Box<Owned> owner=success(Box.new(Owned {value=&mut local}));return extract(owner);}i32 main(){return 0;}
]==])
box_reject(ordinary_field_move "projected|partial|move" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
struct Parent {public Box<i32> field;}; i32 main(){Parent owner=Parent {field=success(Box.new(1))};return Box.into_inner(owner.field);}
]==])
box_reject(custom_drop_field_move "Drop|projected" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
struct Parent implements Drop {public Box<i32> field;public void drop(&mut Self self){return;}};i32 main(){Parent owner=Parent {field=success(Box.new(1))};return Box.into_inner(owner.field);}
]==])
box_reject(private_payload "payload" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main(){Box<i32> owner=success(Box.new(1));return owner.payload;}
]==])
box_reject(ffi_box "C FFI" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
extern "C" i32 foreign(Box<i32> owner);i32 main(){Box<i32> owner=success(Box.new(1));return foreign(owner);}
]==])
box_reject(task_escape "local|reference|Scope|scope" [==[
import task { Task, TaskScope, TaskId, Executor, ScopeOptions, ScopeReport };
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
TaskScope open() { Executor executor = Executor.native_threads(); return success(Task.open_scope(&executor,ScopeOptions.defaults())); }
Task<i32> start(&TaskScope scope) { return success(Task.spawn_owned(scope,[]() -> i32 { return 42; })); }
Task<i32> escape(){TaskScope scope=open();Box<Task<i32>> owner=success(Box.new(start(&scope)));return extract(owner);}i32 main(){return 0;}
]==])
box_reject(task_scope_early_close "borrow|Scope|scope" [==[
import task { Task, TaskScope, TaskId, Executor, ScopeOptions, ScopeReport };
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
TaskScope open() { Executor executor = Executor.native_threads(); return success(Task.open_scope(&executor,ScopeOptions.defaults())); }
Task<i32> start(&TaskScope scope) { return success(Task.spawn_owned(scope,[]() -> i32 { return 42; })); }
i32 main(){TaskScope scope=open();Box<Task<i32>> owner=success(Box.new(start(&scope)));Task<i32> work=extract(owner);Task.close_scope(scope);return success(Task.join(work));}
]==])
box_reject(task_in_record_escape "local|reference|Scope|scope" [==[
import task { Task, TaskScope, TaskId, Executor, ScopeOptions, ScopeReport };
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
TaskScope open() { Executor executor = Executor.native_threads(); return success(Task.open_scope(&executor,ScopeOptions.defaults())); }
Task<i32> start(&TaskScope scope) { return success(Task.spawn_owned(scope,[]() -> i32 { return 42; })); }
struct Held {public Task<i32> work;};Held escape(){TaskScope scope=open();Box<Held> owner=success(Box.new(Held {work=start(&scope)}));return extract(owner);}i32 main(){return 0;}
]==])
box_reject(generic_concrete_payload_mismatch "arbitrary type parameter" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
T bad<T>(Box<i32> owner){return Box.into_inner(owner);}i32 main(){return 0;}
]==])
box_reject(drop_retains_exclusive_loan "borrow" [==[
import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
struct Owned implements Drop {public &mut i32 value;public void drop(&mut Self self){*self.value+=1;return;}};i32 main(){mut i32 count=0;Box<Owned> owner=success(Box.new(Owned {value=&mut count}));Owned output=extract(owner);return count;}
]==])
box_reject(private_import_short_leak "Unknown|import|undeclared" [==[
import factory { make_box };import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main(){rsn owner=make_box();rsn value=Box.into_inner(owner);Record leaked;return 0;}
]==])
box_reject(private_import_qualified_leak "Unknown|import|undeclared|member" [==[
import factory { make_box };import memory { Box, InputError, replace };
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
T extract<T>(Box<T> owner) { return Box.into_inner(owner); }
i32 main(){rsn owner=make_box();rsn value=Box.into_inner(owner);factory.Record leaked;return 0;}
]==])
box_reject(no_import "Unknown|import" [==[
i32 main(){return Box.into_inner(1);}
]==])
box_reject(selective_not_imported "Unknown|import" [==[
import memory { Shared };i32 main(){return memory.Box.into_inner(1);}
]==])
file(MAKE_DIRECTORY "${TEST_DIR}/minimal")
file(WRITE "${TEST_DIR}/minimal/payload.avx" [==[
public struct Record { public i32 value; };
]==])
file(WRITE "${TEST_DIR}/minimal/factory.avx" [==[
import memory { Box, InputError };
import payload { Record };
public Box<Record> make_box() {
    Result<Box<Record>,InputError<Record>> constructed = Box.new(Record { value = 23 });
    switch (move constructed) {
        case Result<Box<Record>,InputError<Record>>.Ok { Box<Record> owner = value }:
            return owner;
            break;
        case Result<Box<Record>,InputError<Record>>.Err: Never.unreachable(); break;
    }
}
]==])
box_accept(minimal/direct [==[
import memory { Box, InputError };
i32 main() {
    Result<Box<i32>,InputError<i32>> constructed = Box.new(42);
    switch (move constructed) {
        case Result<Box<i32>,InputError<i32>>.Ok { Box<i32> owner = value }:
            return Box.into_inner(owner) == 42 ? 0 : 1;
            break;
        case Result<Box<i32>,InputError<i32>>.Err: return 2; break;
    }
}
]==])
box_accept(minimal/cross_module [==[
import memory { Box };
import factory { make_box };
i32 main() {
    rsn owner = make_box();
    rsn payload = Box.into_inner(owner);
    return payload.value == 23 ? 0 : 1;
}
]==])

# 원본 LLVM은 그대로 보존하고 추적 allocator 이름만 별도 복사본에 연결한다.
# Keep original LLVM and change only allocator symbol names in a separate tracking copy.
file(WRITE "${TEST_DIR}/box_extract_fault.avx" [==[
import memory { Box, InputError };
struct Owned implements Drop {
    public &mut i32 count;
    public i32 value;
    public void drop(&mut Self self) { *self.count += 1; return; }
};
i32 main() {
    mut i32 count = 0;
    mut i32 value = 0;
    {
        Result<Box<Owned>,InputError<Owned>> first = Box.new(Owned { count = &mut count, value = 42 });
        switch (move first) {
            case Result<Box<Owned>,InputError<Owned>>.Ok { Box<Owned> inner = value }:
                Result<Box<Box<Owned>>,InputError<Box<Owned>>> second = Box.new(inner);
                switch (move second) {
                    case Result<Box<Box<Owned>>,InputError<Box<Owned>>>.Ok { Box<Box<Owned>> outer = value }:
                        Owned output = Box.into_inner(Box.into_inner(outer));
                        value = output.value;
                        break;
                    case Result<Box<Box<Owned>>,InputError<Box<Owned>>>.Err { InputError<Box<Owned>> failure = error }:
                        switch (move failure) {
                            case InputError<Box<Owned>>.AllocationFailure { Box<Owned> recovered = input }:
                                Owned output = Box.into_inner(recovered);
                                value = output.value;
                                break;
                            default: Never.unreachable(); break;
                        }
                        break;
                }
                break;
            case Result<Box<Owned>,InputError<Owned>>.Err { InputError<Owned> failure = error }:
                switch (move failure) {
                    case InputError<Owned>.AllocationFailure { Owned recovered = input }:
                        value = recovered.value;
                        break;
                    default: Never.unreachable(); break;
                }
                break;
        }
    }
    return count == 1 && value == 42 ? 0 : 1;
}
]==])
box_run("fault fixture check" 0 "ownership checked" "${AEVIXC}" --check "${TEST_DIR}/box_extract_fault.avx")
execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/box_extract_fault.avx" RESULT_VARIABLE status OUTPUT_VARIABLE fault_ir ERROR_VARIABLE error TIMEOUT 90)
if(NOT "${status}" STREQUAL "0")
    message(FATAL_ERROR "fault LLVM: ${error}")
endif()
string(REPLACE "@malloc(" "@box_fault_allocate(" fault_ir "${fault_ir}")
string(REPLACE "@free(" "@box_fault_release(" fault_ir "${fault_ir}")
file(WRITE "${TEST_DIR}/fault-jit.ll" "${fault_ir}")
if(version MATCHES "ORC JIT enabled" AND DEFINED JIT_PROBE AND EXISTS "${JIT_PROBE}")
    foreach(opt 0 2)
        box_run("fault JIT O${opt} three stages" 0 "3 exact" "${JIT_PROBE}" "${TEST_DIR}/fault-jit.ll" "${opt}")
    endforeach()
endif()
if(DEFINED CLANG AND EXISTS "${CLANG}" AND DEFINED RUNTIME AND EXISTS "${RUNTIME}" AND NOT WIN32)
    string(REPLACE "@main(" "@box_fault_main(" fault_ir "${fault_ir}")
    file(WRITE "${TEST_DIR}/fault-aot.ll" "${fault_ir}")
    foreach(opt 0 2)
        box_run("build fault AOT O${opt}" 0 "" "${CLANG}" --driver-mode=g++ -std=c++20 "-O${opt}" "${TEST_DIR}/fault-aot.ll" "${CMAKE_CURRENT_LIST_DIR}/v03_box_extract_fault_harness.cpp" "${RUNTIME}" -pthread -o "${TEST_DIR}/fault-O${opt}")
        foreach(stage 0 1 2)
            box_run("fault AOT O${opt} stage${stage}" 0 "" "${TEST_DIR}/fault-O${opt}" "${stage}")
        endforeach()
        string(REPLACE ") {" ") sanitize_address {" instrumented "${fault_ir}")
        string(REPLACE ") noreturn {" ") noreturn sanitize_address {" instrumented "${instrumented}")
        file(WRITE "${TEST_DIR}/fault-sanitized.ll" "${instrumented}")
        box_run("instrument fault LLVM O${opt}" 0 "" "${CLANG}" "-O${opt}" -g -fsanitize=address,undefined -fno-omit-frame-pointer -c "${TEST_DIR}/fault-sanitized.ll" -o "${TEST_DIR}/fault-sanitized-O${opt}.o")
        find_program(NM_EXECUTABLE nm)
        if(NM_EXECUTABLE)
            box_run("actual ASan object O${opt}" 0 "__asan_report_" "${NM_EXECUTABLE}" -u "${TEST_DIR}/fault-sanitized-O${opt}.o")
        endif()
        box_run("link fault ASan O${opt}" 0 "" "${CLANG}" --driver-mode=g++ -std=c++20 "-O${opt}" -g -fsanitize=address,undefined -fno-omit-frame-pointer "${TEST_DIR}/fault-sanitized-O${opt}.o" "${CMAKE_CURRENT_LIST_DIR}/v03_box_extract_fault_harness.cpp" "${RUNTIME}" -pthread -o "${TEST_DIR}/fault-sanitized-O${opt}")
        foreach(stage 0 1 2)
            box_run("fault ASan O${opt} stage${stage}" 0 "" "${CMAKE_COMMAND}" -E env ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "${TEST_DIR}/fault-sanitized-O${opt}" "${stage}")
        endforeach()
    endforeach()
    set(extra_libraries -pthread)
    if(APPLE)
        list(APPEND extra_libraries -lc++)
    else()
        list(APPEND extra_libraries -lstdc++)
    endif()
    foreach(name scalars_zero_simd nested_owners external_loans own_drop_and_early_failure task_roots_and_worker)
        execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/${name}.avx" RESULT_VARIABLE status OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 90)
        if(NOT "${status}" STREQUAL "0")
            message(FATAL_ERROR "sanitizer LLVM ${name}: ${error}")
        endif()
        string(REPLACE ") {" ") sanitize_address {" ir "${ir}")
        string(REPLACE ") noreturn {" ") noreturn sanitize_address {" ir "${ir}")
        file(WRITE "${TEST_DIR}/${name}-sanitized.ll" "${ir}")
        set(levels 0 2)
        # LLVM22의 O0 raw-IR ASan은 빈 load/store를 0-byte report 경로로 계측한다.
        # LLVM22 O0 raw-IR ASan instruments empty loads/stores through a zero-byte report path.
        # 이 두 fixture에는 unit/빈 배열/빈 클로저가 있다. O0 AOT/JIT는 위에서 그대로 실행한다.
        # These fixtures contain unit/empty arrays/empty closures; ordinary O0 AOT/JIT remains exercised above.
        if(name STREQUAL "scalars_zero_simd" OR name STREQUAL "task_roots_and_worker")
            set(levels 1 2)
        endif()
        foreach(opt IN LISTS levels)
            box_run("link generated ASan ${name} O${opt}" 0 "" "${CLANG}" "-O${opt}" -g -fsanitize=address,undefined -fno-omit-frame-pointer "${TEST_DIR}/${name}-sanitized.ll" "${RUNTIME}" ${extra_libraries} -o "${TEST_DIR}/${name}-sanitized-O${opt}")
            box_run("generated ASan ${name} O${opt}" 0 "" "${CMAKE_COMMAND}" -E env ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "${TEST_DIR}/${name}-sanitized-O${opt}")
        endforeach()
    endforeach()
endif()
message(STATUS "Box.into_inner: exact whole-owner consumption, retained loans, accepted source modes and27 rejections and applicable allocation/ASan gates passed")
