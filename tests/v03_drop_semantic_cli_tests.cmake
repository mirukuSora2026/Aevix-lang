# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_drop_semantic_cli_tests.cmake
#
# Description :
#     사용자 정리 훅의 정확한 서명, 제네릭 계약, 복사 금지와 명시적 호출 및 병렬 사용 거부를 검사한다.
#     Check exact user cleanup signatures, generic contracts, Copy exclusion, and rejection of explicit calls and parallel use.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
function(check_drop name expected pattern source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    execute_process(COMMAND "${AEVIXC}" --check "${TEST_DIR}/${name}.avx"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
    if(NOT "${result}" STREQUAL "${expected}"
        OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${result}\n${output}\n${error}")
    endif()
endfunction()

check_drop(struct_self 0 "" [=[
struct Resource implements Drop {
    public i32 value;
    public void drop(&mut Self self) { return; }
};
i32 main() { Resource resource = Resource { value = 7 }; return resource.value == 7 ? 0 : 1; }
]=])
check_drop(struct_concrete 0 "" [=[
struct Resource implements Drop { public void drop(&mut Resource self) { return; } };
i32 main() { Resource resource = Resource {}; return 0; }
]=])
check_drop(enum_self 0 "" [=[
enum Resource implements Drop {
    Empty;
    Value { public i32 value; };
    public void drop(&mut Self self) { return; }
};
i32 main() { Resource resource = Resource.Value { value = 7 }; return 0; }
]=])
check_drop(generic_resource 0 "" [=[
struct Resource<T> implements Drop {
    public T value;
    public void drop(&mut Self self) { return; }
};
void consume<T>(T value) where T implements Drop { return; }
i32 main() { Resource<i32> resource = Resource<i32> { value = 7 }; consume(resource); return 0; }
]=])
check_drop(generic_explicit_self 0 "" [=[
struct Resource<T> implements Drop {
    public T value;
    public void drop(&mut Resource<T> self) { return; }
};
i32 main() { Resource<i32> resource = Resource<i32> { value = 7 }; return 0; }
]=])
check_drop(ordinary_drop_method 0 "" [=[
struct Item { public void drop(&mut Self self) { return; } };
i32 main() { mut Item item = Item {}; item.drop(); return 0; }
]=])
check_drop(ordinary_dynamic_drop_method 0 "" [=[
interface Cleaner { public void drop(&mut Self self); };
struct Item implements Cleaner { public void drop(&mut Self self) { return; } };
i32 main() { mut Item item = Item {}; &mut dyn Cleaner view = &mut item; view.drop(); return 0; }
]=])
check_drop(self_interface 0 "" [=[
interface Readable { public i32 read(&Self self); };
interface Scored { public i32 score(&Self self) { return 1; } };
struct Item implements Readable, Scored, Drop {
    public i32 read(&Self self) { return 7; }
    public override i32 score(&Self self) { return 2; }
    public void drop(&mut Self self) { return; }
};
i32 main() { Item item = Item {}; &dyn Scored view = &item; return item.read() == 7 && view.score() == 2 ? 0 : 1; }
]=])
check_drop(self_override_result 1 "return type does not match" [=[
interface Scored { public i32 score(&Self self) { return 1; } };
struct Item implements Scored { public override bool score(&Self self) { return true; } };
i32 main() { return 0; }
]=])
check_drop(self_override_receiver 1 "override.*default" [=[
interface Scored { public i32 score(&Self self) { return 1; } };
struct Item implements Scored { public override i32 score(&mut Self self) { return 2; } };
i32 main() { return 0; }
]=])

check_drop(missing_method 1 "Drop requires exactly one" [=[struct Resource implements Drop {}; i32 main() { return 0; }]=])
check_drop(private_method 1 "Drop requires the exact" [=[struct Resource implements Drop { private void drop(&mut Self self) { return; } }; i32 main() { return 0; }]=])
check_drop(wrong_return 1 "Drop requires the exact" [=[struct Resource implements Drop { public i32 drop(&mut Self self) { return 1; } }; i32 main() { return 0; }]=])
check_drop(read_receiver 1 "Drop requires the exact" [=[struct Resource implements Drop { public void drop(&Self self) { return; } }; i32 main() { return 0; }]=])
check_drop(value_receiver 1 "Drop requires the exact" [=[struct Resource implements Drop { public void drop(Self self) { return; } }; i32 main() { return 0; }]=])
check_drop(wrong_receiver 1 "Drop requires the exact" [=[struct Resource implements Drop { public void drop(&mut i32 self) { return; } }; i32 main() { return 0; }]=])
check_drop(no_receiver 1 "Drop requires the exact" [=[struct Resource implements Drop { public void drop() { return; } }; i32 main() { return 0; }]=])
check_drop(extra_parameter 1 "Drop requires the exact" [=[struct Resource implements Drop { public void drop(&mut Self self, i32 extra) { return; } }; i32 main() { return 0; }]=])
check_drop(default_parameter 1 "Drop requires the exact" [=[struct Resource implements Drop { public void drop(&mut Self self, i32 extra = 1) { return; } }; i32 main() { return 0; }]=])
check_drop(variadic_parameter 1 "Drop requires the exact" [=[struct Resource implements Drop { public void drop(&mut Self self, i32 values[]) { return; } }; i32 main() { return 0; }]=])
check_drop(generic_method 1 "Drop requires the exact" [=[struct Resource implements Drop { public void drop<T>(&mut Self self) { return; } }; i32 main() { return 0; }]=])
check_drop(overloaded_method 1 "Drop requires exactly one" [=[struct Resource implements Drop { public void drop(&mut Self self) { return; } public void drop(&mut Self self, i32 extra) { return; } }; i32 main() { return 0; }]=])
check_drop(duplicate_marker 1 "Drop lifecycle marker exactly once" [=[struct Resource implements Drop, Drop { public void drop(&mut Self self) { return; } }; i32 main() { return 0; }]=])
check_drop(marker_arguments 1 "Drop is a lifecycle marker without type arguments" [=[struct Resource implements Drop<i32> { public void drop(&mut Self self) { return; } }; i32 main() { return 0; }]=])
check_drop(copy_drop 1 "both Copy and Drop" [=[struct Resource implements Copy, Drop { public void drop(&mut Self self) { return; } }; i32 main() { return 0; }]=])
check_drop(nested_copy_drop 1 "Copy-compatible" [=[
struct Resource implements Drop { public void drop(&mut Self self) { return; } };
struct Wrapper implements Copy { public Resource resource; };
i32 main() { return 0; }
]=])
check_drop(enum_copy_drop 1 "Copy-compatible" [=[
struct Resource implements Drop { public void drop(&mut Self self) { return; } };
enum Wrapper implements Copy { Value { public Resource resource; }; Empty; };
i32 main() { return 0; }
]=])
check_drop(unsatisfied_bound 1 "does not satisfy.*implements" [=[void consume<T>(T value) where T implements Drop { return; } i32 main() { consume(1); return 0; }]=])
check_drop(incompatible_bounds 1 "both Copy and Drop" [=[void consume<T>(T value) where T implements Copy, T implements Drop { return; } i32 main() { return 0; }]=])
check_drop(unused_struct_marker_arguments 1 "Drop is a lifecycle marker without type arguments" [=[struct Invalid<T> where T implements Drop<i32> {}; i32 main() { return 0; }]=])
check_drop(unused_interface_marker_arguments 1 "Drop is a lifecycle marker without type arguments" [=[interface Invalid<T> where T implements Drop<i32> {}; i32 main() { return 0; }]=])
check_drop(unused_struct_incompatible_bounds 1 "both Copy and Drop" [=[struct Invalid<T> where T implements Copy, T implements Drop {}; i32 main() { return 0; }]=])
check_drop(unused_bad_generic 1 "Drop requires the exact" [=[struct Resource<T> implements Drop { public T drop(&mut Self self) { return 1; } }; i32 main() { return 0; }]=])
check_drop(unused_generic_receiver 1 "Drop requires the exact" [=[struct Resource<T> implements Drop { public void drop(&mut Resource<i32> self) { return; } }; i32 main() { return 0; }]=])
check_drop(marker_redeclaration 1 "prelude name 'Drop'" [=[interface Drop {}; i32 main() { return 0; }]=])
check_drop(marker_nested_redeclaration 1 "prelude name 'Drop'" [=[struct Outer { struct Drop {}; }; i32 main() { return 0; }]=])
check_drop(marker_alias_rebinding 1 "prelude name 'Drop'" [=[type Drop = i32; i32 main() { return 0; }]=])
check_drop(marker_local_shadow 1 "shadow lifecycle marker 'Drop'" [=[i32 main() { i32 Drop = 1; return 0; }]=])
check_drop(marker_generic_shadow 1 "shadow lifecycle marker 'Drop'" [=[void value<Drop>(Drop input) { return; } i32 main() { return 0; }]=])
check_drop(dynamic_drop 1 "cannot form dyn Drop" [=[void bad(&dyn Drop value) { return; } i32 main() { return 0; }]=])
check_drop(boxed_drop 1 "cannot form dyn Drop" [=[import memory { Box }; void bad(Box<dyn Drop> value) { return; } i32 main() { return 0; }]=])
check_drop(unused_dynamic_drop 1 "cannot form dyn Drop" [=[interface Invalid { public void inspect(&dyn Drop value); }; i32 main() { return 0; }]=])
check_drop(aliased_dynamic_drop 1 "cannot form dyn Drop" [=[type Cleanup = Drop; void bad(&dyn Cleanup value) { return; } i32 main() { return 0; }]=])

set(resource [=[struct Resource implements Drop { public void drop(&mut Self self) { return; } };]=])
check_drop(explicit_receiver 1 "cannot be called explicitly" "${resource} i32 main() { mut Resource resource = Resource {}; resource.drop(); return 0; }")
check_drop(explicit_type 1 "cannot be called explicitly" "${resource} i32 main() { mut Resource resource = Resource {}; Resource.drop(&mut resource); return 0; }")
check_drop(explicit_alias 1 "cannot be called explicitly" "${resource} type Owner = Resource; i32 main() { mut Owner resource = Owner {}; Owner.drop(&mut resource); return 0; }")
check_drop(explicit_inside_drop 1 "cannot be called explicitly" [=[struct Resource implements Drop { public void drop(&mut Self self) { self.drop(); return; } }; i32 main() { return 0; }]=])
check_drop(method_reference 1 "cannot be referenced or called explicitly" "${resource} i32 main() { rsn callback = Resource.drop; return 0; }")
check_drop(unused_generic_call 1 "cannot be called explicitly" "${resource} void bad<T>(&mut Resource resource) { resource.drop(); return; } i32 main() { return 0; }")
check_drop(generic_bound_manual_call 1 "Generic contract|cannot be called explicitly" [=[void bad<T>(&mut T resource) where T implements Drop { resource.drop(); return; } i32 main() { return 0; }]=])
check_drop(dynamic_callable_drop 1 "cannot implement a callable interface" [=[
interface Cleaner { public void drop(&mut Self self); };
struct Resource implements Drop, Cleaner { public void drop(&mut Self self) { return; } };
i32 main() { mut Resource resource = Resource {}; &mut dyn Cleaner view = &mut resource; view.drop(); return 0; }
]=])
check_drop(boxed_callable_drop 1 "cannot implement a callable interface" [=[
import memory { Box };
interface Cleaner { public void drop(&mut Self self); };
struct Resource implements Drop, Cleaner { public void drop(&mut Self self) { return; } };
Result<i32, InputError<Resource>> run() {
    mut Box<Resource> concrete = Box.new(Resource {})?;
    mut Box<dyn Cleaner> owner = Box.into_dyn(concrete);
    owner.drop();
    return Result.ok(0);
}
i32 main() { return 0; }
]=])
check_drop(generic_callable_drop 1 "cannot implement a callable interface" [=[
interface Cleaner { public void drop(&mut Self self); };
struct Resource implements Drop, Cleaner { public void drop(&mut Self self) { return; } };
void invoke<T>(&mut T resource) where T implements Cleaner { resource.drop(); return; }
i32 main() { mut Resource resource = Resource {}; invoke(&mut resource); return 0; }
]=])
check_drop(unused_generic_callable_drop 1 "cannot implement a callable interface" [=[
interface Cleaner { public void drop(&mut Self self); };
struct Resource<T> implements Drop, Cleaner { public void drop(&mut Self self) { return; } };
i32 main() { return 0; }
]=])
check_drop(parallel_drop_temporary 1 "custom Drop cleanup" [=[
import parallel;
struct Resource implements Drop { public void drop(&mut Self self) { return; } };
void worker(&mut i32 item) { Resource {}; *item += 1; return; }
i32 main() { mut i32 values[2] = [0, 0]; parallel.for_each(Slice.from_array_mut(&mut values), worker); return 0; }
]=])
check_drop(parallel_nested_drop_temporary 1 "custom Drop cleanup" [=[
import parallel;
struct Resource implements Drop { public void drop(&mut Self self) { return; } };
struct Wrapper { public Resource resource; };
void worker(&mut i32 item) { Wrapper { resource = Resource {} }; *item += 1; return; }
i32 main() { mut i32 values[2] = [0, 0]; parallel.for_each(Slice.from_array_mut(&mut values), worker); return 0; }
]=])
message(STATUS "0.3 Drop semantic contracts and rejection checks passed")
