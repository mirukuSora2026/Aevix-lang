# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_closure_consume_cli_tests.cmake
#
# Description :
#     FnOnce 소유 캡처의 이동·정리와 외부 읽기·수정 대여 보존을 검사한다.
#     Check FnOnce capture moves, exact cleanup, and preservation of external shared/exclusive loans.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(prelude [=[
import atomic { Atomic, MemoryOrder };
import task { Task, TaskScope, Executor, ScopeOptions };
T success<T,E>(Result<T,E> input) {
    switch (move input) {
        case Result<T,E>.Ok { T value = value }: return value; break;
        case Result<T,E>.Err: Never.unreachable(); break;
    }
}
persist Atomic<i32> destroyed = Atomic.new(0);
persist Atomic<i64> sequence = Atomic.new(0i64);
i32 count() { return success(Atomic.load(&destroyed,MemoryOrder.Relaxed)); }
i64 order() { return success(Atomic.load(&sequence,MemoryOrder.Relaxed)); }
void reset() {
    success(Atomic.store(&destroyed,0,MemoryOrder.Relaxed));
    success(Atomic.store(&sequence,0i64,MemoryOrder.Relaxed));
    return;
}
struct Owned implements Drop {
    public i32 id;
    public void drop(&mut Self self) {
        success(Atomic.fetch_add(&destroyed,1,MemoryOrder.Relaxed));
        success(Atomic.store(&sequence,order() * 32i64 + success(Integer.convert<i64>(self.id)),MemoryOrder.Relaxed));
        return;
    }
};
TaskScope open() {
    Executor executor = Executor.native_threads();
    return success(Task.open_scope(&executor,ScopeOptions.defaults()));
}
]=])
function(run name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(accept name body)
    file(WRITE "${TEST_DIR}/${name}.avx" "${prelude}\n${body}\n")
    run("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    foreach(opt 0 2)
        run("${name} AOT O${opt}" 0 "" --run "-O${opt}" "${TEST_DIR}/${name}.avx")
        if(version MATCHES "ORC JIT enabled")
            run("${name} JIT O${opt}" 0 "" --jit "-O${opt}" "${TEST_DIR}/${name}.avx")
        endif()
    endforeach()
endfunction()
function(reject name body pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${prelude}\n${body}\n")
    run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()

accept(return_owned_and_cleanup_order [=[
i32 main() {
    {
        Owned first = Owned { id = 1 };
        Owned second = Owned { id = 2 };
        rsn choose = [first,second](Owned argument) -> Owned { return first; };
        Owned selected = choose(Owned { id = 3 });
        if (selected.id != 1 || count() != 2 || order() != 98i64) return 1;
    }
    if (count() != 3 || order() != 3137i64) return 2;
    reset();
    {
        Owned first = Owned { id = 1 };
        Owned second = Owned { id = 2 };
        rsn uncalled = [first,second]() -> Owned { return first; };
    }
    return count() == 2 && order() == 65i64 ? 0 : 3;
}
]=])

accept(owned_containers [=[
Result<i32,CollectionError> containers() {
    {
        string text = success(string.from_view("owned text"));
        rsn take = [text]() -> string { return text; };
        string moved = take();
        if (!string.equals(string.view(&moved),"owned text")) return Result.ok(1);
    }
    {
        Owned items[2] = [Owned { id = 1 },Owned { id = 2 }];
        rsn take = [items]() -> Array<Owned,2> { return items; };
        Owned moved[2] = take();
        if (moved[0].id != 1 || moved[1].id != 2 || count() != 0) return Result.ok(2);
    }
    if (count() != 2 || order() != 65i64) return Result.ok(3);
    reset();
    {
        Vector<Owned> items = [Owned { id = 1 },Owned { id = 2 }]?;
        rsn take = [items]() -> Vector<Owned> { return items; };
        Vector<Owned> moved = take();
        if (Vector.len(&moved) != 2usize || count() != 0) return Result.ok(4);
    }
    if (count() != 2) return Result.ok(5);
    reset();
    {
        Tuple<Owned,Owned> pair = Tuple.make(Owned { id = 1 },Owned { id = 2 });
        rsn take = [pair]() -> Tuple<Owned,Owned> { return pair; };
        Tuple<Owned,Owned> moved = take();
        if (count() != 0) return Result.ok(6);
    }
    return Result.ok(count() == 2 && order() == 65i64 ? 0 : 7);
}
i32 main() { return success(containers()); }
]=])

accept(branches_and_propagation [=[
Result<Owned,i32> branch(bool fail) {
    Owned input = Owned { id = 1 };
    rsn take = [input](bool failed) -> Result<Owned,i32> {
        Result<unit,i32> status = failed ? Result.err(7) : Result.ok(Unit.value());
        status?;
        return Result.ok(input);
    };
    return take(fail);
}
i32 main() {
    {
        Result<Owned,i32> failure = branch(true);
        if (!Result.is_err(&failure) || count() != 1) return 1;
    }
    reset();
    {
        Owned output = success(branch(false));
        if (output.id != 1 || count() != 0) return 2;
    }
    if (count() != 1) return 3;
    reset();
    for (i32 selector in Range.new(0,2)) {
        Owned left = Owned { id = 1 };
        Owned right = Owned { id = 2 };
        rsn choose = [left,right](bool first) -> Owned { if (first) return left; return right; };
        Owned selected = choose(selector == 0);
        if (selected.id != (selector == 0 ? 1 : 2)) return 4;
    }
    return count() == 4 ? 0 : 5;
}
]=])

accept(external_reference_provenance [=[
i32 main() {
    i32 shared = 41;
    mut i32 writable = 7;
    i32 copied = 2;
    {
        Owned input = Owned { id = 1 };
        rsn take = [input,&shared,&mut writable,copied]() -> Owned {
            writable = writable + shared + copied;
            return input;
        };
        Owned selected = take();
        if (selected.id != 1 || writable != 50 || count() != 0) return 1;
    }
    if (count() != 1) return 2;
    reset();
    Owned first = Owned { id = 1 };
    rsn read = [first,&shared]() -> &i32 { return &shared; };
    &i32 reading = read();
    if (*reading != 41 || count() != 1) return 3;
    Owned second = Owned { id = 2 };
    rsn write = [second,&mut writable]() -> &mut i32 { return &mut writable; };
    &mut i32 writing = write();
    *writing = *writing + 1;
    return writable == 51 && count() == 2 ? 0 : 4;
}
]=])

accept(nested_closure_and_repeated_receivers [=[
i32 invoke_once<F>(F callable) where F implements FnOnce<Tuple<>,i32> { return callable(); }
i32 main() {
    if (invoke_once([]() -> i32 { return 42; }) != 42) return 6;
    {
        Owned input = Owned { id = 1 };
        rsn outer = [input]() -> Owned {
            rsn inner = [input]() -> Owned { return input; };
            return inner();
        };
        Owned selected = outer();
        if (selected.id != 1 || count() != 0) return 1;
    }
    if (count() != 1) return 2;
    i32 constant = 5;
    rsn shared = [constant](i32 value) -> i32 { return constant + value; };
    if (shared(1) != 6 || shared(2) != 7) return 3;
    mut i32 total = 0;
    mut rsn mutable = [&mut total](i32 value) -> i32 { total = total + value; return total; };
    if (mutable(2) != 2 || mutable(3) != 5) return 4;
    return total == 5 ? 0 : 5;
}
]=])

accept(task_capture_and_result_roots [=[
i32 main() {
    TaskScope scope = open();
    Task<i32> original = success(Task.spawn_owned(&scope,[]() -> i32 { return 42; }));
    rsn forward = [original]() -> Task<i32> { return original; };
    Task<i32> moved = forward();
    if (success(Task.join(moved)) != 42) return 1;
    success(Task.close_scope(scope));
    {
        TaskScope second = open();
        Owned captured = Owned { id = 1 };
        Task<Owned> work = success(Task.spawn_owned(&second,[captured]() -> Owned { return captured; }));
        Owned result = success(Task.join(work));
        if (result.id != 1 || count() != 0) return 2;
    }
    return count() == 1 ? 0 : 3;
}
]=])

set(moved "moved|unavailable|move")
set(borrowed "borrow|Borrow|reference|Reference|read-only|lifetime|local|escaping")
reject(double_capture_move [=[i32 main() { Owned input = Owned { id = 1 }; rsn take = [input]() -> Owned { Owned first = input; return input; }; take(); return 0; }]=] "${moved}")
reject(conditional_capture_move [=[i32 main() { Owned input = Owned { id = 1 }; rsn take = [input](bool consume) -> Owned { if (consume) { Owned first = input; } return input; }; take(true); return 0; }]=] "${moved}")
reject(borrowed_capture_move [=[i32 main() { Owned input = Owned { id = 1 }; rsn take = [&input]() -> Owned { return input; }; take(); return 0; }]=] "${borrowed}")
reject(readonly_capture_write [=[i32 main() { i32 number = 1; Owned input = Owned { id = 1 }; rsn take = [input,&number]() -> Owned { number = 2; return input; }; take(); return 0; }]=] "${borrowed}|mutable")
reject(owned_capture_reference_escape [=[i32 main() { Owned input = Owned { id = 1 }; rsn take = [input]() -> &i32 { return &input.id; }; &i32 escaped = take(); return *escaped; }]=] "${borrowed}")
reject(owned_capture_nested_reference_escape [=[struct Holder { public &i32 view; }; i32 main() { Owned input = Owned { id = 1 }; rsn take = [input]() -> Holder { return Holder { view = &input.id }; }; Holder escaped = take(); return *escaped.view; }]=] "${borrowed}")
reject(copy_capture_reference_escape [=[struct Plain implements Copy { public i32 id; }; i32 main() { Plain copied = Plain { id = 4 }; Owned anchor = Owned { id = 1 }; rsn take = [anchor,copied]() -> &i32 { return &copied.id; }; &i32 escaped = take(); return *escaped; }]=] "${borrowed}")
reject(duplicate_capture_name [=[i32 main() { mut i32 number = 1; Owned input = Owned { id = 1 }; rsn take = [input,&number,&mut number]() -> Owned { return input; }; take(); return 0; }]=] "more than once")
reject(aliasing_captures [=[i32 main() { mut i32 number = 1; &i32 reading = &number; &mut i32 writing = &mut number; Owned input = Owned { id = 1 }; rsn take = [input,reading,writing]() -> Owned { *writing = *reading; return input; }; take(); return 0; }]=] "${borrowed}")
reject(ordinary_struct_field_move [=[struct Holder { public Owned item; }; i32 main() { Holder input = Holder { item = Owned { id = 1 } }; Owned output = input.item; return 0; }]=] "projected|partial|Move|move")
reject(drop_capture_nested_field_move [=[struct Parent implements Drop { public Owned item; public void drop(&mut Self self) { return; } }; i32 main() { Parent input = Parent { item = Owned { id = 1 } }; rsn take = [input]() -> Owned { return input.item; }; take(); return 0; }]=] "Drop|projected")
reject(task_capture_keeps_scope [=[i32 main() { TaskScope scope = open(); Task<i32> original = success(Task.spawn_owned(&scope,[]() -> i32 { return 1; })); rsn forward = [original]() -> Task<i32> { return original; }; Task<i32> held = forward(); Task.close_scope(scope); return 0; }]=] "${borrowed}|scope|Scope")
reject(unused_task_capture_keeps_scope [=[i32 main() { TaskScope scope = open(); Task<i32> original = success(Task.spawn_owned(&scope,[]() -> i32 { return 1; })); rsn unused = [original]() -> i32 { return 7; }; Task.close_scope(scope); return 0; }]=] "${borrowed}|scope|Scope")
reject(closure_is_once [=[i32 main() { Owned input = Owned { id = 1 }; rsn take = [input]() -> Owned { return input; }; Owned first = take(); Owned second = take(); return 0; }]=] "${moved}")

# sanitizer는 소유 캡처 반환과 작업 결과 경로의 실제 생성 코드를 함께 검사한다.
# The sanitizer checks real generated owned-capture return and task-result paths together.
if(DEFINED CLANG AND EXISTS "${CLANG}" AND DEFINED RUNTIME AND EXISTS "${RUNTIME}" AND NOT WIN32)
    set(extra_libraries -pthread)
    if(APPLE)
        list(APPEND extra_libraries -lc++)
    else()
        list(APPEND extra_libraries -lstdc++)
    endif()
    foreach(name return_owned_and_cleanup_order owned_containers external_reference_provenance task_capture_and_result_roots)
        execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/${name}.avx" OUTPUT_FILE "${TEST_DIR}/${name}.ll" RESULT_VARIABLE emitted ERROR_VARIABLE error TIMEOUT 90)
        if(NOT "${emitted}" STREQUAL "0")
            message(FATAL_ERROR "sanitizer emit ${name}: ${error}")
        endif()
        # LLVM 입력에는 C 프런트엔드가 없으므로 ASan 함수 속성을 직접 붙여 실제 계측을 보장한다.
        # LLVM input bypasses the C frontend; add ASan function attributes so generated functions are actually instrumented.
        file(READ "${TEST_DIR}/${name}.ll" sanitizer_ir)
        string(REPLACE ") {" ") sanitize_address {" sanitizer_ir "${sanitizer_ir}")
        string(REPLACE ") noreturn {" ") noreturn sanitize_address {" sanitizer_ir "${sanitizer_ir}")
        file(WRITE "${TEST_DIR}/${name}-sanitized.ll" "${sanitizer_ir}")
        execute_process(COMMAND "${CLANG}" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "${TEST_DIR}/${name}-sanitized.ll" "${RUNTIME}" ${extra_libraries} -o "${TEST_DIR}/${name}-sanitized" RESULT_VARIABLE linked OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
        if(NOT "${linked}" STREQUAL "0")
            message(FATAL_ERROR "sanitizer link ${name}: ${output}${error}")
        endif()
        execute_process(COMMAND "${TEST_DIR}/${name}-sanitized" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
        if(NOT "${status}" STREQUAL "0")
            message(FATAL_ERROR "sanitizer run ${name}: ${status}\n${output}${error}")
        endif()
    endforeach()
endif()
message(STATUS "Complete FnOnce capture ownership, cleanup ordering, and reference provenance passed")
