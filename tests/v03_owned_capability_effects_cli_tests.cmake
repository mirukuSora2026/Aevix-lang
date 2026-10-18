# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_owned_capability_effects_cli_tests.cmake
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
function(capability_run name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(capability_accept name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    capability_run("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    capability_run("${name} AOT" 0 "" --run "${TEST_DIR}/${name}.avx")
    capability_run("${name} AOT O2" 0 "" --run -O2 "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        capability_run("${name} JIT" 0 "" --jit "${TEST_DIR}/${name}.avx")
        capability_run("${name} JIT O2" 0 "" --jit -O2 "${TEST_DIR}/${name}.avx")
    endif()
endfunction()
function(capability_reject name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    capability_run("reject ${name}" 1 "borrow|Borrow|local|lifetime|escape" --check "${TEST_DIR}/${name}.avx")
endfunction()

capability_accept(box_consumed_storage_valid [=[
import memory {Box};
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok{T value=value}:return value;break;case Result<T,E>.Err:Never.unreachable();break;}}
struct View{public &mut Vector<&i32> owner;};
void put(mut Box<View> handle,&i32 input){&mut View target=Box.as_mut(&mut handle);rsn outcome=Vector.push(&mut *target.owner,input);return;}
i32 main(){i32 retained_local=7;Result<Vector<&i32>,CollectionError> made=[];mut Vector<&i32> owner=success(made);{Box<View> handle=success(Box.new(View{owner=&mut owner}));put(handle,&retained_local);}return *owner[0] == 7 ? 0 : 1;}

]=])

capability_reject(box_consumed_storage [=[
import memory {Box};
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok{T value=value}:return value;break;case Result<T,E>.Err:Never.unreachable();break;}}
struct View{public &mut Vector<&i32> owner;};
void put(mut Box<View> handle,&i32 input){&mut View target=Box.as_mut(&mut handle);rsn outcome=Vector.push(&mut *target.owner,input);return;}
i32 main(){Result<Vector<&i32>,CollectionError> made=[];mut Vector<&i32> owner=success(made);{i32 local=7;Box<View> handle=success(Box.new(View{owner=&mut owner}));put(handle,&local);}return *owner[0];}

]=])

capability_accept(shared_consumed_storage_valid [=[
import memory {Shared};
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok{T value=value}:return value;break;case Result<T,E>.Err:Never.unreachable();break;}}
struct View{public &mut Vector<&i32> owner;};
void put(mut Shared<View> handle,&i32 input){Optional<&mut View> unique=Shared.get_mut(&mut handle);switch(move unique){case Optional<&mut View>.Some{&mut View target=value}:rsn outcome=Vector.push(&mut *target.owner,input);break;default:Never.unreachable();break;}return;}
i32 main(){i32 retained_local=7;Result<Vector<&i32>,CollectionError> made=[];mut Vector<&i32> owner=success(made);{Shared<View> handle=success(Shared.new(View{owner=&mut owner}));put(handle,&retained_local);}return *owner[0] == 7 ? 0 : 1;}

]=])

capability_reject(shared_consumed_storage [=[
import memory {Shared};
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok{T value=value}:return value;break;case Result<T,E>.Err:Never.unreachable();break;}}
struct View{public &mut Vector<&i32> owner;};
void put(mut Shared<View> handle,&i32 input){Optional<&mut View> unique=Shared.get_mut(&mut handle);switch(move unique){case Optional<&mut View>.Some{&mut View target=value}:rsn outcome=Vector.push(&mut *target.owner,input);break;default:Never.unreachable();break;}return;}
i32 main(){Result<Vector<&i32>,CollectionError> made=[];mut Vector<&i32> owner=success(made);{i32 local=7;Shared<View> handle=success(Shared.new(View{owner=&mut owner}));put(handle,&local);}return *owner[0];}

]=])

capability_accept(guard_tuple_consumed_storage_valid [=[
import memory;
import sync {Mutex,MutexGuard};
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok{T value=value}:return value;break;case Result<T,E>.Err:Never.unreachable();break;}}
void put(mut Tuple<MutexGuard<Optional<&i32>>> tuple,&i32 input){&mut MutexGuard<Optional<&i32>> guard=Tuple.get_mut<0>(&mut tuple);&mut Optional<&i32> target=Mutex.data_mut(guard);Optional<&i32> previous=memory.replace<Optional<&i32>>(target,Optional.some(input));return;}
i32 main(){i32 retained_local=7;Mutex<Optional<&i32>> owner=Mutex.new(Optional.none());{MutexGuard<Optional<&i32>> guard=success(Mutex.lock(&owner));Tuple<MutexGuard<Optional<&i32>>> tuple=Tuple.make(guard);put(tuple,&retained_local);}MutexGuard<Optional<&i32>> reader=success(Mutex.lock(&owner));return read_value(Mutex.data(&reader)) == 7 ? 0 : 1;}

i32 read_value(&Optional<&i32> input) { switch (*input) { case Optional<&i32>.Some { &i32 value = value }: return *value; break; case Optional<&i32>.None: return -1; break; } }

]=])

capability_reject(guard_tuple_consumed_storage [=[
import memory;
import sync {Mutex,MutexGuard};
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok{T value=value}:return value;break;case Result<T,E>.Err:Never.unreachable();break;}}
void put(mut Tuple<MutexGuard<Optional<&i32>>> tuple,&i32 input){&mut MutexGuard<Optional<&i32>> guard=Tuple.get_mut<0>(&mut tuple);&mut Optional<&i32> target=Mutex.data_mut(guard);Optional<&i32> previous=memory.replace<Optional<&i32>>(target,Optional.some(input));return;}
i32 main(){Mutex<Optional<&i32>> owner=Mutex.new(Optional.none());{i32 local=7;MutexGuard<Optional<&i32>> guard=success(Mutex.lock(&owner));Tuple<MutexGuard<Optional<&i32>>> tuple=Tuple.make(guard);put(tuple,&local);}MutexGuard<Optional<&i32>> reader=success(Mutex.lock(&owner));switch(*Mutex.data(&reader)){case Optional<&i32>.Some{& &i32 found=&value}:return **found;break;default:return 0;break;}}

]=])

capability_accept(guard_optional_consumed_storage_valid [=[
import memory;
import sync {Mutex,MutexGuard};
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok{T value=value}:return value;break;case Result<T,E>.Err:Never.unreachable();break;}}
void put(Optional<MutexGuard<Optional<&i32>>> wrapped,&i32 input){switch(move wrapped){case Optional<MutexGuard<Optional<&i32>>>.Some{MutexGuard<Optional<&i32>> found=value}:mut MutexGuard<Optional<&i32>> guard=found;&mut Optional<&i32> target=Mutex.data_mut(&mut guard);Optional<&i32> previous=memory.replace<Optional<&i32>>(target,Optional.some(input));break;default:Never.unreachable();break;}return;}
i32 main(){i32 retained_local=7;Mutex<Optional<&i32>> owner=Mutex.new(Optional.none());{MutexGuard<Optional<&i32>> guard=success(Mutex.lock(&owner));put(Optional.some(guard),&retained_local);}MutexGuard<Optional<&i32>> reader=success(Mutex.lock(&owner));return read_value(Mutex.data(&reader)) == 7 ? 0 : 1;}

i32 read_value(&Optional<&i32> input) { switch (*input) { case Optional<&i32>.Some { &i32 value = value }: return *value; break; case Optional<&i32>.None: return -1; break; } }

]=])

capability_reject(guard_optional_consumed_storage [=[
import memory;
import sync {Mutex,MutexGuard};
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok{T value=value}:return value;break;case Result<T,E>.Err:Never.unreachable();break;}}
void put(Optional<MutexGuard<Optional<&i32>>> wrapped,&i32 input){switch(move wrapped){case Optional<MutexGuard<Optional<&i32>>>.Some{MutexGuard<Optional<&i32>> found=value}:mut MutexGuard<Optional<&i32>> guard=found;&mut Optional<&i32> target=Mutex.data_mut(&mut guard);Optional<&i32> previous=memory.replace<Optional<&i32>>(target,Optional.some(input));break;default:Never.unreachable();break;}return;}
i32 main(){Mutex<Optional<&i32>> owner=Mutex.new(Optional.none());{i32 local=7;MutexGuard<Optional<&i32>> guard=success(Mutex.lock(&owner));put(Optional.some(guard),&local);}MutexGuard<Optional<&i32>> reader=success(Mutex.lock(&owner));switch(*Mutex.data(&reader)){case Optional<&i32>.Some{& &i32 found=&value}:return **found;break;default:return 0;break;}}

]=])

capability_accept(guard_enum_consumed_storage_valid [=[
import memory;
import sync {Mutex,MutexGuard};
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok{T value=value}:return value;break;case Result<T,E>.Err:Never.unreachable();break;}}
enum Cap{Owned{public MutexGuard<Optional<&i32>> guard;};};
void put(Cap wrapped,&i32 input){switch(move wrapped){case Cap.Owned{MutexGuard<Optional<&i32>> found=guard}:mut MutexGuard<Optional<&i32>> guard=found;&mut Optional<&i32> target=Mutex.data_mut(&mut guard);Optional<&i32> previous=memory.replace<Optional<&i32>>(target,Optional.some(input));break;}return;}
i32 main(){i32 retained_local=7;Mutex<Optional<&i32>> owner=Mutex.new(Optional.none());{MutexGuard<Optional<&i32>> guard=success(Mutex.lock(&owner));put(Cap.Owned{guard=guard},&retained_local);}MutexGuard<Optional<&i32>> reader=success(Mutex.lock(&owner));return read_value(Mutex.data(&reader)) == 7 ? 0 : 1;}

i32 read_value(&Optional<&i32> input) { switch (*input) { case Optional<&i32>.Some { &i32 value = value }: return *value; break; case Optional<&i32>.None: return -1; break; } }

]=])

capability_reject(guard_enum_consumed_storage [=[
import memory;
import sync {Mutex,MutexGuard};
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok{T value=value}:return value;break;case Result<T,E>.Err:Never.unreachable();break;}}
enum Cap{Owned{public MutexGuard<Optional<&i32>> guard;};};
void put(Cap wrapped,&i32 input){switch(move wrapped){case Cap.Owned{MutexGuard<Optional<&i32>> found=guard}:mut MutexGuard<Optional<&i32>> guard=found;&mut Optional<&i32> target=Mutex.data_mut(&mut guard);Optional<&i32> previous=memory.replace<Optional<&i32>>(target,Optional.some(input));break;}return;}
i32 main(){Mutex<Optional<&i32>> owner=Mutex.new(Optional.none());{i32 local=7;MutexGuard<Optional<&i32>> guard=success(Mutex.lock(&owner));put(Cap.Owned{guard=guard},&local);}MutexGuard<Optional<&i32>> reader=success(Mutex.lock(&owner));switch(*Mutex.data(&reader)){case Optional<&i32>.Some{& &i32 found=&value}:return **found;break;default:return 0;break;}}

]=])

capability_reject(nested_read_escape [=[
struct Middle { public &i32 value; };
struct Handle { public &Middle owner; };
&i32 inspect(&Handle handle) { return handle.owner.value; }
&i32 escape() { i32 local=7; Middle middle=Middle{value=&local}; Handle handle=Handle{owner=&middle}; return inspect(&handle); }
i32 main() { return *escape(); }

]=])

capability_reject(nested_slice_escape [=[
struct Holder { public Slice<&i32> values; };
&i32 first(&Holder holder){return holder.values[0];}
&i32 escape(){i32 local=7;&i32 items[1]=[&local];Holder holder=Holder{values=Slice.from_array(&items)};return first(&holder);}
i32 main(){return *escape();}

]=])

file(WRITE "${TEST_DIR}/recursive_reference_capacity.avx" [=[
struct Link { public &Link next; };
&Link walk(&Link value, i32 count) { if (count == 0) return value; return walk(value.next, count - 1); }
i32 main() { return 0; }

]=])
capability_run("recursive_reference_capacity" 1 "supported capacity" --check "${TEST_DIR}/recursive_reference_capacity.avx")
