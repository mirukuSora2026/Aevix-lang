# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_initializer_cli_tests.cmake
#
# Description :
#     필드 초기화 축약의 실행 순서·복사·이동·대여와 인터페이스 재정의 규칙을 전체 컴파일 경로에서 검사한다.
#     Check shorthand initialization order, copying, moves, borrows, and interface override rules through the full compiler pipeline.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
set(clang_arguments)
if(DEFINED CLANG)
    list(APPEND clang_arguments --clang "${CLANG}")
endif()

function(run_case name expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${result}" STREQUAL "${expected}"
        OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${result}\n${output}\n${error}")
    endif()
endfunction()

function(fixture name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
endfunction()

fixture(struct_enum_order [=[
struct Ordered {
    public i32 last;
    public i32 held;
    public i32 first;
};
enum Event {
    Move { public i32 x; public i32 y; };
    Quit;
};
i32 step(&mut i32 counter) {
    *counter += 1;
    return *counter;
}
i32 main() {
    mut i32 calls = 0;
    i32 held = 50;
    Ordered ordered = Ordered {
        first = step(&mut calls),
        held,
        last = step(&mut calls),
    };
    if (calls != 2 || ordered.first != 1 || ordered.last != 2 || ordered.held != 50) return 1;
    i32 x = 10;
    i32 y = 20;
    Event event = Event.Move { y, x, };
    switch (event) {
        case Event.Move { i32 horizontal = x, i32 vertical = y }:
            if (horizontal != 10 || vertical != 20) return 2;
            break;
        case Event.Quit: return 3; break;
    }
    return 0;
}
]=])

fixture(generic_copy [=[
struct Pair<T> { public T first; public T second; };
struct Point implements Copy { public i32 x; public i32 y; };
struct Holder { public Point point; };
i32 main() {
    i32 first = 10;
    i32 second = 20;
    Pair<i32> pair = Pair<i32> { second, first };
    if (pair.first != 10 || pair.second != 20) return 1;
    i32 x = 30;
    i32 y = 40;
    Point point = Point { x, y };
    Holder holder = Holder { point };
    Point again = point;
    if (point.x != 30 || again.y != 40 || holder.point.x != 30) return 2;
    return 0;
}
]=])

fixture(owned_fields [=[
struct TextHolder { public string text; };
enum Message { Text { public string text; }; Empty; };
Result<usize, TextError> struct_length() {
    string text = string.from_view("Aevix")?;
    TextHolder holder = TextHolder { text };
    return Result.ok(string.byte_len(&holder.text));
}
Result<usize, TextError> enum_length() {
    string text = string.from_view("payload")?;
    Message message = Message.Text { text };
    switch (move message) {
        case Message.Text { string content = text }:
            return Result.ok(string.byte_len(&content));
            break;
        case Message.Empty: return Result.ok(0); break;
    }
    return Result.ok(99);
}
i32 main() {
    Result<usize, TextError> first = struct_length();
    switch (move first) {
        case Result<usize, TextError>.Ok { usize count = value }:
            if (count != 5) return 1;
            break;
        case Result<usize, TextError>.Err: return 2; break;
    }
    Result<usize, TextError> second = enum_length();
    switch (move second) {
        case Result<usize, TextError>.Ok { usize count = value }:
            if (count != 7) return 3;
            break;
        case Result<usize, TextError>.Err: return 4; break;
    }
    return 0;
}
]=])

fixture(override_dispatch [=[
interface Value {
    public i32 value(&Self self);
    public i32 scaled(&Self self) { return self.value() * 2; }
};
struct Number implements Value {
    public i32 amount;
    public i32 value(&Number self) { return self.amount; }
    public override i32 scaled(&Number self) { return self.value() * 3; }
};
struct Inherited implements Value {
    public i32 value(&Inherited self) { return 5; }
};
enum Signal implements Value {
    Ready;
    public i32 value(&Signal self) { return 7; }
    public override i32 scaled(&Signal self) { return self.value() * 4; }
};
i32 main() {
    i32 amount = 10;
    Number number = Number { amount };
    Inherited inherited = Inherited {};
    Signal signal = Signal.Ready;
    &dyn Value number_view = &number;
    &dyn Value inherited_view = &inherited;
    &dyn Value signal_view = &signal;
    if (number.scaled() != 30 || number_view.scaled() != 30) return 1;
    if (inherited.scaled() != 10 || inherited_view.scaled() != 10) return 2;
    if (signal.scaled() != 28 || signal_view.scaled() != 28) return 3;
    return 0;
}
]=])

fixture(nominal_vector [=[
struct Item { public i32 value; };
Result<i32, CollectionError> run() {
    Item values[] = [Item { value = 7 }, Item { value = 9 }]?;
    return Result.ok(values[0].value + values[1].value);
}
i32 main() {
    Result<i32, CollectionError> result = run();
    switch (move result) {
        case Result<i32, CollectionError>.Ok { i32 value = value }: return value == 16 ? 0 : 1; break;
        case Result<i32, CollectionError>.Err: return 2; break;
    }
}
]=])
fixture(nested_nominal_array [=[
const usize COUNT = 2;
struct Item { public i32 value; };
Result<i32, CollectionError> run() {
    Array<Item, COUNT> row = [Item { value = 7 }, Item { value = 9 }];
    Array<Item, COUNT> rows[] = [row]?;
    return Result.ok(rows[0][1].value);
}
i32 main() {
    Result<i32, CollectionError> result = run();
    switch (move result) {
        case Result<i32, CollectionError>.Ok { i32 value = value }: return value == 9 ? 0 : 1; break;
        case Result<i32, CollectionError>.Err: return 2; break;
    }
}
]=])
file(WRITE "${TEST_DIR}/shapes.avx" [=[
public struct Item {
    private i32 stored;
    public Item make(i32 value) { return Item { stored = value }; }
    public i32 get(&Self self) { return self.stored; }
};
public i32 secret() { return 99; }
]=])
fixture(imported_nominal_vector [=[
import shapes { Item };
Result<i32, CollectionError> run() {
    Item values[] = [Item.make(7)]?;
    return Result.ok(values[0].get());
}
i32 main() {
    Result<i32, CollectionError> result = run();
    switch (move result) {
        case Result<i32, CollectionError>.Ok { i32 value = value }: return value == 7 ? 0 : 1; break;
        case Result<i32, CollectionError>.Err: return 2; break;
    }
}
]=])

execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version ERROR_VARIABLE version_error
    RESULT_VARIABLE version_result TIMEOUT 30)
if(NOT version_result EQUAL 0)
    message(FATAL_ERROR "Compiler version query failed: ${version_error}")
endif()
foreach(name struct_enum_order generic_copy owned_fields override_dispatch
    nominal_vector nested_nominal_array imported_nominal_vector)
    run_case("${name} check" 0 "" --check "${TEST_DIR}/${name}.avx")
    run_case("${name} AOT" 0 "" --run ${clang_arguments} "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        run_case("${name} JIT" 0 "" --jit "${TEST_DIR}/${name}.avx")
    endif()
endforeach()

function(reject name pattern source)
    fixture("${name}" "${source}")
    run_case("${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()

reject(container_import_does_not_leak "not imported" [=[
import shapes { Item };
Result<i32, CollectionError> run() {
    Item values[] = [Item.make(7)]?;
    return Result.ok(shapes.secret());
}
i32 main() { return 0; }
]=])
reject(container_private_field "private" [=[
import shapes { Item };
Result<i32, CollectionError> run() {
    Item values[] = [Item.make(7)]?;
    return Result.ok(values[0].stored);
}
i32 main() { return 0; }
]=])

reject(moved_string "[Mm]oved|[Mm]ove|unavailable" [=[
struct Holder { public string text; };
Result<usize, TextError> bad() {
    string text = string.from_view("owned")?;
    Holder holder = Holder { text };
    return Result.ok(string.byte_len(&text));
}
i32 main() { return 0; }
]=])
reject(borrowed_string "[Bb]orrow|loan|[Mm]ove" [=[
struct Holder { public string text; };
Result<usize, TextError> bad() {
    string text = string.from_view("owned")?;
    &string view = &text;
    Holder holder = Holder { text };
    return Result.ok(string.byte_len(view));
}
i32 main() { return 0; }
]=])
reject(private_field "private field" [=[
struct Point { private i32 x; };
i32 main() { i32 x = 1; Point point = Point { x }; return 0; }
]=])
reject(missing_field "[Mm]issing.*field" [=[
struct Point { public i32 x; public i32 y; };
i32 main() { i32 x = 1; Point point = Point { x }; return 0; }
]=])
reject(duplicate_field "Repeated field initializer" [=[
struct Point { public i32 x; };
i32 main() { i32 x = 1; Point point = Point { x, x = 2 }; return 0; }
]=])
reject(unknown_field "Unknown initializer field" [=[
struct Point { public i32 x; };
i32 main() { i32 y = 1; Point point = Point { y }; return 0; }
]=])
reject(unbound_name "Unknown name 'x'" [=[
struct Point { public i32 x; };
i32 main() { Point point = Point { x }; return 0; }
]=])
reject(no_implicit_self "Unknown name 'x'" [=[
struct Point {
    public i32 x;
    public Point copy(&Point self) { return Point { x }; }
};
i32 main() { return 0; }
]=])
reject(wrong_field_type "[Ff]ield.*requires|[Ii]nitializer.*requires" [=[
struct Point { public i32 x; };
i32 main() { bool x = true; Point point = Point { x }; return 0; }
]=])
reject(case_shorthand "Expected|identifier" [=[
enum Event { Move { public i32 x; }; Quit; };
i32 main() {
    i32 x = 1;
    Event event = Event.Move { x };
    switch (event) {
        case Event.Move { x }: break;
        case Event.Quit: break;
    }
    return 0;
}
]=])
reject(missing_override "requires override" [=[
interface Value { public i32 get(&Self self) { return 1; } };
struct Item implements Value { public i32 get(&Item self) { return 2; } };
i32 main() { return 0; }
]=])
reject(no_default "override.*default" [=[
struct Item { public override i32 get(&Item self) { return 1; } };
i32 main() { return 0; }
]=])
reject(required_method_override "override.*default" [=[
interface Value { public i32 get(&Self self); };
struct Item implements Value { public override i32 get(&Item self) { return 2; } };
i32 main() { return 0; }
]=])
reject(override_result_mismatch "return type does not match" [=[
interface Value { public i32 get(&Self self) { return 1; } };
struct Item implements Value { public override bool get(&Item self) { return true; } };
i32 main() { return 0; }
]=])
reject(override_parameter_mismatch "override.*default" [=[
interface Value { public i32 get(&Self self, i32 input) { return input; } };
struct Item implements Value { public override i32 get(&Item self, bool input) { return 2; } };
i32 main() { return 0; }
]=])
reject(override_receiver_mismatch "override.*default" [=[
interface Value { public i32 get(&Self self) { return 1; } };
struct Item implements Value { public override i32 get(&mut Item self) { return 2; } };
i32 main() { return 0; }
]=])
reject(free_override "override.*concrete" [=[
public override i32 get() { return 1; }
i32 main() { return 0; }
]=])
reject(field_override "override.*field" [=[
struct Item { public override i32 field; };
i32 main() { return 0; }
]=])
reject(interface_override "override.*concrete" [=[
interface Value { public override i32 get(&Self self) { return 1; } };
i32 main() { return 0; }
]=])
message(STATUS "0.3 initializer shorthand, ownership, and override dispatch checks passed")
