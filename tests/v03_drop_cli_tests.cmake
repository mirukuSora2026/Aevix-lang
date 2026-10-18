# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_drop_cli_tests.cmake
#
# Description :
#     사용자 정의 정리의 정확한 호출 횟수·순서와 소유권 전달을 AOT·JIT로 검사한다.
#     Check exact user-defined cleanup counts, ordering, and ownership transfers through AOT and JIT.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR OR NOT DEFINED CLANG)
    message(FATAL_ERROR "AEVIXC, TEST_DIR, and CLANG are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")

function(run_drop name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${result}" STREQUAL "${expected}" OR
        (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${result}: ${output}${error}")
    endif()
endfunction()

# Aevix의 가변 전역 상태에 의존하지 않고 C ABI로 정리의 관찰 가능한 순서를 기록한다.
# Record observable cleanup order through the C ABI without relying on mutable Aevix globals.
file(WRITE "${TEST_DIR}/drop_audit.c" [=[
#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
static int events[1024];
static int count;
EXPORT void drop_record(int value) {
    if (count < 1024) events[count] = value;
    count++;
}
EXPORT int drop_count(void) { return count; }
EXPORT int drop_at(int index) {
    return index >= 0 && index < count && index < 1024 ? events[index] : -99999;
}
EXPORT void drop_reset(void) { count = 0; }
]=])
if(APPLE)
    set(native "${TEST_DIR}/drop audit.dylib")
    set(native_link "${native}")
    set(shared_options -dynamiclib -fPIC)
elseif(WIN32)
    set(native "${TEST_DIR}/drop audit.dll")
    execute_process(COMMAND "${CLANG}" -dumpmachine RESULT_VARIABLE target_result
        OUTPUT_VARIABLE native_target OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_VARIABLE target_error TIMEOUT 10)
    if(NOT target_result EQUAL 0)
        message(FATAL_ERROR "Cannot determine Windows audit ABI: ${target_error}")
    endif()
    if(native_target MATCHES "windows-msvc")
        set(native_link "${TEST_DIR}/drop audit import.lib")
        set(shared_options -shared "-Wl,/implib:${native_link}")
    elseif(native_target MATCHES "windows-gnu|mingw")
        set(native_link "${TEST_DIR}/drop audit import.dll.a")
        set(shared_options -shared "-Wl,--out-implib,${native_link}")
    else()
        message(FATAL_ERROR "Unsupported Windows audit ABI: ${native_target}")
    endif()
    set(ENV{PATH} "${TEST_DIR};$ENV{PATH}")
else()
    set(native "${TEST_DIR}/drop audit.so")
    set(native_link "${native}")
    set(shared_options -shared -fPIC)
endif()
execute_process(COMMAND "${CLANG}" ${shared_options} "${TEST_DIR}/drop_audit.c" -o "${native}"
    RESULT_VARIABLE native_result ERROR_VARIABLE native_error TIMEOUT 30)
if(NOT native_result EQUAL 0)
    message(FATAL_ERROR "Cannot build cleanup audit library: ${native_error}")
endif()
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)

set(prefix [=[
import memory { Box, InputError };
extern "C" void drop_record(i32 value);
extern "C" i32 drop_count();
extern "C" i32 drop_at(i32 index);
extern "C" void drop_reset();
struct Resource implements Drop {
    public i32 id;
    public void drop(&mut Resource self) { drop_record(self.id); return; }
};
]=])

function(drop_case name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${prefix}\n${source}\n")
    run_drop("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    run_drop("${name} AOT" 0 "" --run --link "${native_link}" "${TEST_DIR}/${name}.avx")
    run_drop("${name} AOT O2" 0 "" --run -O2 --link "${native_link}" "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        run_drop("${name} JIT" 0 "" --jit --load "${native}" "${TEST_DIR}/${name}.avx")
        run_drop("${name} JIT O2" 0 "" --jit -O2 --load "${native}" "${TEST_DIR}/${name}.avx")
    endif()
endfunction()

drop_case(scope_reverse [=[
i32 main() {
    {
        Resource first = Resource { id = 1 };
        Resource second = Resource { id = 2 };
        if (drop_count() != 0) return 1;
        {
            Resource nested = Resource { id = 3 };
            if (drop_count() != 0) return 2;
        }
        if (drop_count() != 1 || drop_at(0) != 3) return 3;
    }
    return drop_count() == 3 && drop_at(1) == 2 && drop_at(2) == 1 ? 0 : 4;
}
]=])

drop_case(recursive_fields [=[
struct Parent implements Drop {
    public Resource left;
    public Resource right;
    public void drop(&mut Parent self) {
        drop_record(self.left.id * 10 + self.right.id);
        return;
    }
};
struct Envelope { public Parent parent; public Resource tail; };
i32 main() {
    {
        Envelope envelope = Envelope {
            parent = Parent { left = Resource { id = 1 }, right = Resource { id = 2 } },
            tail = Resource { id = 3 }
        };
        if (drop_count() != 0) return 1;
    }
    return drop_count() == 4 && drop_at(0) == 3 && drop_at(1) == 12
        && drop_at(2) == 2 && drop_at(3) == 1 ? 0 : 2;
}
]=])

drop_case(whole_move [=[
i32 main() {
    {
        Resource original = Resource { id = 7 };
        {
            Resource transferred = original;
            if (drop_count() != 0) return 1;
        }
        if (drop_count() != 1 || drop_at(0) != 7) return 2;
    }
    return drop_count() == 1 ? 0 : 3;
}
]=])

drop_case(return_transfer [=[
Resource make_resource() {
    Resource local = Resource { id = 8 };
    return local;
}
i32 early_exit() {
    Resource local = Resource { id = 9 };
    if (local.id == 9) return 4;
    return 0;
}
i32 main() {
    {
        Resource received = make_resource();
        if (drop_count() != 0 || received.id != 8) return 1;
        if (early_exit() != 4 || drop_count() != 1 || drop_at(0) != 9) return 2;
    }
    return drop_count() == 2 && drop_at(1) == 8 ? 0 : 3;
}
]=])

drop_case(parameters_and_temporaries [=[
void consume(Resource value) {
    if (drop_count() != 0) drop_record(99);
    return;
}
Resource temporary_owner() { return Resource { id = 3 }; }
i32 main() {
    consume(Resource { id = 1 });
    if (drop_count() != 1 || drop_at(0) != 1) return 1;
    Resource { id = 2 };
    if (drop_count() != 2 || drop_at(1) != 2) return 2;
    temporary_owner();
    return drop_count() == 3 && drop_at(2) == 3 ? 0 : 3;
}
]=])

drop_case(generic_cleanup [=[
struct Wrapper<T> implements Drop {
    public T value;
    public i32 id;
    public void drop(&mut Wrapper<T> self) { drop_record(self.id); return; }
};
i32 main() {
    {
        Wrapper<Resource> owned = Wrapper<Resource> { value = Resource { id = 1 }, id = 10 };
        Wrapper<i32> scalar = Wrapper<i32> { value = 2, id = 20 };
        if (drop_count() != 0) return 1;
    }
    return drop_count() == 3 && drop_at(0) == 20 && drop_at(1) == 10
        && drop_at(2) == 1 ? 0 : 2;
}
]=])

drop_case(result_propagation [=[
Result<i32, i32> fail() { return Result.err(42); }
Result<i32, i32> propagate() {
    Resource first = Resource { id = 1 };
    Resource second = Resource { id = 2 };
    i32 missing = fail()?;
    return Result.ok(missing);
}
i32 main() {
    Result<i32, i32> result = propagate();
    if (!Result.is_err(&result)) return 1;
    return drop_count() == 2 && drop_at(0) == 2 && drop_at(1) == 1 ? 0 : 2;
}
]=])

drop_case(loop_exits [=[
i32 main() {
    mut i32 iteration = 0;
    while (iteration < 3) {
        Resource local = Resource { id = iteration + 1 };
        iteration++;
        if (iteration == 1) continue;
        if (iteration == 2) break;
    }
    if (iteration != 2 || drop_count() != 2 || drop_at(0) != 1 || drop_at(1) != 2) return 1;
    while (false) { Resource uncreated = Resource { id = 99 }; }
    return drop_count() == 2 ? 0 : 2;
}
]=])

drop_case(replacement [=[
i32 main() {
    {
        mut Resource owner = Resource { id = 1 };
        owner = Resource { id = 2 };
        if (drop_count() != 1 || drop_at(0) != 1 || owner.id != 2) return 1;
        Resource replacement = Resource { id = 3 };
        owner = replacement;
        if (drop_count() != 2 || drop_at(1) != 2 || owner.id != 3) return 2;
    }
    return drop_count() == 3 && drop_at(2) == 3 ? 0 : 3;
}
]=])

drop_case(array_owners [=[
i32 main() {
    {
        Resource values[3] = [Resource { id = 1 }, Resource { id = 2 }, Resource { id = 3 }];
        if (drop_count() != 0 || values[0].id != 1) return 1;
    }
    if (drop_count() != 3 || drop_at(0) != 3 || drop_at(1) != 2 || drop_at(2) != 1) return 2;
    drop_reset();
    { Resource empty[0] = []; }
    return drop_count() == 0 ? 0 : 3;
}
]=])

drop_case(closure_owners [=[
i32 main() {
    {
        Resource captured = Resource { id = 1 };
        rsn consume = [captured]() -> i32 { return captured.id; };
        if (drop_count() != 0 || consume() != 1) return 1;
        if (drop_count() != 1 || drop_at(0) != 1) return 2;
    }
    {
        Resource captured = Resource { id = 2 };
        rsn uncalled = [captured]() -> i32 { return captured.id; };
        if (drop_count() != 1) return 3;
    }
    return drop_count() == 2 && drop_at(1) == 2 ? 0 : 4;
}
]=])

drop_case(vector_owners [=[
Result<i32, CollectionError> vectors() {
    {
        Resource values[] = [Resource { id = 1 }, Resource { id = 2 }, Resource { id = 3 }]?;
        if (drop_count() != 0 || Vector.len(&values) != 3) return Result.ok(1);
    }
    if (drop_count() != 3 || drop_at(0) != 3 || drop_at(1) != 2 || drop_at(2) != 1) return Result.ok(2);
    drop_reset();
    { Resource empty[] = []?; }
    return Result.ok(drop_count() == 0 ? 0 : 3);
}
i32 main() {
    Result<i32, CollectionError> result = vectors();
    switch (move result) {
        case Result<i32, CollectionError>.Ok { i32 status = value }: return status; break;
        case Result<i32, CollectionError>.Err: return 99; break;
    }
}
]=])

drop_case(box_owners [=[
interface Identity { public i32 value(&Self self); };
struct DynamicResource implements Drop, Identity {
    public i32 id;
    public i32 value(&DynamicResource self) { return self.id; }
    public void drop(&mut DynamicResource self) { drop_record(self.id); return; }
};
Result<i32, InputError<Resource>> concrete_box() {
    {
        Box<Resource> owner = Box.new(Resource { id = 4 })?;
        if (drop_count() != 0 || Box.as_ref(&owner).id != 4) return Result.ok(1);
    }
    return Result.ok(drop_count() == 1 && drop_at(0) == 4 ? 0 : 2);
}
Result<i32, InputError<DynamicResource>> dynamic_box() {
    {
        Box<DynamicResource> concrete = Box.new(DynamicResource { id = 5 })?;
        Box<dyn Identity> erased = Box.into_dyn(concrete);
        if (drop_count() != 0 || erased.value() != 5) return Result.ok(3);
    }
    return Result.ok(drop_count() == 1 && drop_at(0) == 5 ? 0 : 4);
}
i32 main() {
    Result<i32, InputError<Resource>> first = concrete_box();
    switch (move first) {
        case Result<i32, InputError<Resource>>.Ok { i32 status = value }: if (status != 0) return status; break;
        case Result<i32, InputError<Resource>>.Err: return 98; break;
    }
    drop_reset();
    Result<i32, InputError<DynamicResource>> second = dynamic_box();
    switch (move second) {
        case Result<i32, InputError<DynamicResource>>.Ok { i32 status = value }: return status; break;
        case Result<i32, InputError<DynamicResource>>.Err: return 99; break;
    }
}
]=])

drop_case(enum_cleanup [=[
enum Event implements Drop {
    Empty;
    Owned { public Resource item; };
    public void drop(&mut Event self) { drop_record(90); return; }
};
i32 main() {
    {
        Event payload = Event.Owned { item = Resource { id = 6 } };
        Event empty = Event.Empty;
        if (drop_count() != 0) return 1;
    }
    return drop_count() == 3 && drop_at(0) == 90 && drop_at(1) == 90
        && drop_at(2) == 6 ? 0 : 2;
}
]=])

function(reject_drop name source pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${prefix}\n${source}\n")
    run_drop("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()

reject_drop(copy_and_drop [=[
struct Invalid implements Copy, Drop {
    public i32 id;
    public void drop(&mut Invalid self) { drop_record(self.id); return; }
};
i32 main() { return 0; }
]=] "Copy|Drop")

reject_drop(direct_drop [=[
i32 main() {
    mut Resource owner = Resource { id = 1 };
    owner.drop();
    return 0;
}
]=] "Drop|drop|cleanup")

reject_drop(qualified_drop [=[
i32 main() {
    mut Resource owner = Resource { id = 1 };
    Resource.drop(&mut owner);
    return 0;
}
]=] "Drop|drop|cleanup")

reject_drop(partial_field_move [=[
struct Owner implements Drop {
    public Resource inner;
    public void drop(&mut Owner self) { drop_record(90); return; }
};
i32 main() {
    Owner whole = Owner { inner = Resource { id = 1 } };
    Resource extracted = whole.inner;
    return extracted.id;
}
]=] "Drop|drop|partial|field")

reject_drop(partial_enum_move [=[
enum Owner implements Drop {
    Empty;
    Owned { public Resource item; };
    public void drop(&mut Owner self) { drop_record(90); return; }
};
i32 main() {
    Owner whole = Owner.Owned { item = Resource { id = 1 } };
    switch (move whole) {
        case Owner.Owned { Resource extracted = item }: return extracted.id; break;
        case Owner.Empty: return 0; break;
    }
}
]=] "Drop|drop|partial|field")

reject_drop(repeat_owner [=[
i32 main() {
    Resource values[2] = [Resource { id = 1 }; 2];
    return 0;
}
]=] "Copy|copy")

message(STATUS "User-defined Drop cleanup counts, ordering, and transfers passed")
