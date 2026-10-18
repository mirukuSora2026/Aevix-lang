# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_enum_cleanup_cli_tests.cmake
#
# Description :
#     enum payload 일부 이동 뒤 남은 소유자와 중첩 운반자의 정확한 정리 횟수·순서를 검사한다.
#     Check exact cleanup counts and order after partial enum payload moves and nested carrier extraction.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR OR NOT DEFINED CLANG)
    message(FATAL_ERROR "AEVIXC, TEST_DIR, and CLANG are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")

function(run_enum_cleanup name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${result}" STREQUAL "${expected}" OR
        (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${result}: ${output}${error}")
    endif()
endfunction()

# Aevix의 가변 전역 상태에 의존하지 않고 C ABI로 정리의 관찰 가능한 순서를 기록한다.
# Record observable cleanup order through the C ABI without relying on mutable Aevix globals.
file(WRITE "${TEST_DIR}/enum_cleanup_audit.c" [=[
#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
static int events[1024];
static int count;
EXPORT void enum_drop_record(int value) {
    if (count < 1024) events[count] = value;
    count++;
}
EXPORT int enum_drop_count(void) { return count; }
EXPORT int enum_drop_at(int index) {
    return index >= 0 && index < count && index < 1024 ? events[index] : -99999;
}
EXPORT void enum_drop_reset(void) { count = 0; }
]=])
if(APPLE)
    set(native "${TEST_DIR}/enum cleanup audit.dylib")
    set(native_link "${native}")
    set(shared_options -dynamiclib -fPIC)
elseif(WIN32)
    set(native "${TEST_DIR}/enum cleanup audit.dll")
    execute_process(COMMAND "${CLANG}" -dumpmachine RESULT_VARIABLE target_result
        OUTPUT_VARIABLE native_target OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_VARIABLE target_error TIMEOUT 10)
    if(NOT target_result EQUAL 0)
        message(FATAL_ERROR "Cannot determine Windows audit ABI: ${target_error}")
    endif()
    if(native_target MATCHES "windows-msvc")
        set(native_link "${TEST_DIR}/enum cleanup audit import.lib")
        set(shared_options -shared "-Wl,/implib:${native_link}")
    elseif(native_target MATCHES "windows-gnu|mingw")
        set(native_link "${TEST_DIR}/enum cleanup audit import.dll.a")
        set(shared_options -shared "-Wl,--out-implib,${native_link}")
    else()
        message(FATAL_ERROR "Unsupported Windows audit ABI: ${native_target}")
    endif()
    set(ENV{PATH} "${TEST_DIR};$ENV{PATH}")
else()
    set(native "${TEST_DIR}/enum cleanup audit.so")
    set(native_link "${native}")
    set(shared_options -shared -fPIC)
endif()
execute_process(COMMAND "${CLANG}" ${shared_options} "${TEST_DIR}/enum_cleanup_audit.c" -o "${native}"
    RESULT_VARIABLE native_result ERROR_VARIABLE native_error TIMEOUT 30)
if(NOT native_result EQUAL 0)
    message(FATAL_ERROR "Cannot build cleanup audit library: ${native_error}")
endif()
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)

set(prefix [=[
extern "C" void enum_drop_record(i32 value);
extern "C" i32 enum_drop_count();
extern "C" i32 enum_drop_at(i32 index);
extern "C" void enum_drop_reset();
struct Resource implements Drop {
    public i32 id;
    public void drop(&mut Resource self) { enum_drop_record(self.id); return; }
};
enum Payload {
    Empty;
    Pair { public Resource first; public Resource second; public i32 marker; };
    Single { public Resource item; };
};
]=])

function(enum_cleanup_case name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${prefix}\n${source}\n")
    run_enum_cleanup("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    run_enum_cleanup("${name} AOT" 0 "" --run --link "${native_link}" "${TEST_DIR}/${name}.avx")
    run_enum_cleanup("${name} AOT O2" 0 "" --run -O2 --link "${native_link}" "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        run_enum_cleanup("${name} JIT" 0 "" --jit --load "${native}" "${TEST_DIR}/${name}.avx")
        run_enum_cleanup("${name} JIT O2" 0 "" --jit -O2 --load "${native}" "${TEST_DIR}/${name}.avx")
    endif()
endfunction()

enum_cleanup_case(partial_first [=[
i32 main() {
    Payload source = Payload.Pair { first = Resource { id = 1 }, second = Resource { id = 2 }, marker = 7 };
    switch (move source) {
        case Payload.Pair { Resource moved = first, i32 copied = marker }:
            if (moved.id != 1 || copied != 7 || enum_drop_count() != 0) return 1;
            break;
        case Payload.Single: return 2; break;
        case Payload.Empty: return 3; break;
    }
    return enum_drop_count() == 2 && enum_drop_at(0) == 1 && enum_drop_at(1) == 2 ? 0 : 4;
}
]=])

enum_cleanup_case(partial_second [=[
i32 main() {
    Payload source = Payload.Pair { first = Resource { id = 1 }, second = Resource { id = 2 }, marker = 7 };
    switch (move source) {
        case Payload.Pair { Resource moved = second }:
            if (moved.id != 2 || enum_drop_count() != 0) return 1;
            break;
        case Payload.Single: break;
        case Payload.Empty: break;
    }
    return enum_drop_count() == 2 && enum_drop_at(0) == 2 && enum_drop_at(1) == 1 ? 0 : 2;
}
]=])

enum_cleanup_case(complete_payload [=[
i32 main() {
    Payload source = Payload.Pair { first = Resource { id = 1 }, second = Resource { id = 2 }, marker = 7 };
    switch (move source) {
        case Payload.Pair { Resource first_owner = first, Resource second_owner = second }:
            if (first_owner.id != 1 || second_owner.id != 2 || enum_drop_count() != 0) return 1;
            break;
        default: return 2; break;
    }
    return enum_drop_count() == 2 && enum_drop_at(0) == 2 && enum_drop_at(1) == 1 ? 0 : 3;
}
]=])

enum_cleanup_case(no_payload_bindings [=[
i32 main() {
    Payload source = Payload.Pair { first = Resource { id = 1 }, second = Resource { id = 2 }, marker = 7 };
    switch (move source) {
        case Payload.Pair:
            if (enum_drop_count() != 0) return 1;
            break;
        default: return 2; break;
    }
    return enum_drop_count() == 2 && enum_drop_at(0) == 2 && enum_drop_at(1) == 1 ? 0 : 3;
}
]=])

enum_cleanup_case(copy_only_bindings [=[
i32 main() {
    Payload source = Payload.Pair { first = Resource { id = 1 }, second = Resource { id = 2 }, marker = 7 };
    switch (move source) {
        case Payload.Pair { i32 copied = marker }:
            if (copied != 7 || enum_drop_count() != 0) return 1;
            break;
        default: return 2; break;
    }
    return enum_drop_count() == 2 && enum_drop_at(0) == 2 && enum_drop_at(1) == 1 ? 0 : 3;
}
]=])

enum_cleanup_case(branch_and_active_variant [=[
void selected(bool pair) {
    Payload source = pair
        ? Payload.Pair { first = Resource { id = 1 }, second = Resource { id = 2 }, marker = 7 }
        : Payload.Single { item = Resource { id = 3 } };
    switch (move source) {
        case Payload.Pair { Resource moved = first }:
            if (moved.id != 1) enum_drop_record(99);
            break;
        case Payload.Single: break;
        case Payload.Empty: break;
    }
    return;
}
i32 main() {
    selected(false);
    if (enum_drop_count() != 1 || enum_drop_at(0) != 3) return 1;
    enum_drop_reset();
    selected(true);
    return enum_drop_count() == 2 && enum_drop_at(0) == 1 && enum_drop_at(1) == 2 ? 0 : 2;
}
]=])

enum_cleanup_case(loop_reinitialization [=[
i32 main() {
    mut i32 iteration = 0;
    while (iteration < 4) {
        Payload source = Payload.Pair {
            first = Resource { id = iteration * 10 + 1 },
            second = Resource { id = iteration * 10 + 2 }, marker = iteration
        };
        switch (move source) {
            case Payload.Pair { Resource moved = second, i32 copied = marker }:
                if (moved.id != iteration * 10 + 2 || copied != iteration) return 1;
                break;
            default: return 2; break;
        }
        iteration++;
    }
    if (enum_drop_count() != 8) return 3;
    for (mut i32 index = 0; index < 4; index++) {
        if (enum_drop_at(index * 2) != index * 10 + 2 || enum_drop_at(index * 2 + 1) != index * 10 + 1) return 4;
    }
    return 0;
}
]=])

enum_cleanup_case(early_return [=[
i32 selected() {
    Payload source = Payload.Pair { first = Resource { id = 1 }, second = Resource { id = 2 }, marker = 7 };
    switch (move source) {
        case Payload.Pair { Resource moved = first }: return moved.id; break;
        default: return -1; break;
    }
}
i32 main() {
    if (selected() != 1) return 1;
    return enum_drop_count() == 2 && enum_drop_at(0) == 1 && enum_drop_at(1) == 2 ? 0 : 2;
}
]=])

enum_cleanup_case(nested_result_optional [=[
i32 main() {
    Result<Optional<Payload>, Resource> result = Result<Optional<Payload>, Resource>.Ok {
        value = Optional<Payload>.Some {
            value = Payload.Pair { first = Resource { id = 1 }, second = Resource { id = 2 }, marker = 7 }
        }
    };
    switch (move result) {
        case Result<Optional<Payload>, Resource>.Ok { Optional<Payload> optional = value }:
            switch (move optional) {
                case Optional<Payload>.Some { Payload payload = value }:
                    switch (move payload) {
                        case Payload.Pair { Resource moved = first }:
                            if (moved.id != 1 || enum_drop_count() != 0) return 1;
                            break;
                        default: return 2; break;
                    }
                    break;
                case Optional<Payload>.None: return 3; break;
            }
            break;
        case Result<Optional<Payload>, Resource>.Err: return 4; break;
    }
    return enum_drop_count() == 2 && enum_drop_at(0) == 1 && enum_drop_at(1) == 2 ? 0 : 5;
}
]=])

enum_cleanup_case(generic_payload [=[
enum Generic<T> {
    Owned { public T first; public T second; };
    Empty;
};
i32 main() {
    Generic<Resource> source = Generic<Resource>.Owned { first = Resource { id = 1 }, second = Resource { id = 2 } };
    switch (move source) {
        case Generic<Resource>.Owned { Resource moved = first }: if (moved.id != 1) return 1; break;
        case Generic<Resource>.Empty: return 2; break;
    }
    return enum_drop_count() == 2 && enum_drop_at(0) == 1 && enum_drop_at(1) == 2 ? 0 : 3;
}
]=])

enum_cleanup_case(case_continue_and_break [=[
i32 main() {
    mut i32 iteration = 0;
    while (iteration < 3) {
        Payload source = Payload.Pair {
            first = Resource { id = iteration * 10 + 1 },
            second = Resource { id = iteration * 10 + 2 }, marker = iteration
        };
        iteration++;
        switch (move source) {
            case Payload.Pair { Resource moved = first }:
                if (moved.id != (iteration - 1) * 10 + 1) return 1;
                if (iteration == 1) continue;
                break;
            default: return 2; break;
        }
        if (iteration == 2) break;
    }
    return iteration == 2 && enum_drop_count() == 4
        && enum_drop_at(0) == 1 && enum_drop_at(1) == 2
        && enum_drop_at(2) == 11 && enum_drop_at(3) == 12 ? 0 : 3;
}
]=])

file(WRITE "${TEST_DIR}/invalid_drop_owner.avx" "${prefix}\n" [=[
enum Owned implements Drop {
    Pair { public Resource first; public Resource second; };
    public void drop(&mut Owned self) { enum_drop_record(90); return; }
};
i32 main() {
    Owned source = Owned.Pair { first = Resource { id = 1 }, second = Resource { id = 2 } };
    switch (move source) {
        case Owned.Pair { Resource extracted = first }: return extracted.id; break;
    }
}
]=])
run_enum_cleanup("Drop owner partial extraction" 1 "Drop|partial" --check "${TEST_DIR}/invalid_drop_owner.avx")
message(STATUS "Partial enum payload cleanup: eleven source programs and Drop-owner rejection passed")
