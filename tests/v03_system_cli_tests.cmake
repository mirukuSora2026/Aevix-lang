# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_system_cli_tests.cmake
#
# Description :
#     시스템 출력·서식·로그·입력과 실패 값 및 대여 검사를 AOT·JIT로 검증한다.
#     Verify system output, formats, logging, input, error values, and loans through AOT and JIT.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
set(modes --run)
if(version MATCHES "ORC JIT enabled")
    list(APPEND modes --jit)
endif()
function(system_fixture name source input expected_output expected_error)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    file(WRITE "${TEST_DIR}/${name}.in" "${input}")
    foreach(mode IN LISTS modes)
        execute_process(COMMAND "${AEVIXC}" "${mode}" "${TEST_DIR}/${name}.avx"
            INPUT_FILE "${TEST_DIR}/${name}.in" RESULT_VARIABLE status
            OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
        if(NOT "${status}" STREQUAL "0" OR NOT "${output}" STREQUAL "${expected_output}" OR NOT "${error}" STREQUAL "${expected_error}")
            message(FATAL_ERROR "${name} ${mode}: status ${status}\nstdout=${output}\nstderr=${error}\nexpected stdout=${expected_output}\nexpected stderr=${expected_error}")
        endif()
    endforeach()
endfunction()
system_fixture(output [=[
import system;
i32 next(&mut i32 value) { *value += 1; return *value; }
string_view next_code(&mut i32 value) { *value = *value * 10 + 1; return "NC"; }
system.LogLevel next_level(&mut i32 value) { *value = *value * 10 + 2; return system.info; }
string_view next_message(&mut i32 value) { *value = *value * 10 + 3; return "named"; }
i32 main() {
    system.print("values:");
    system.println(1 + 2);
    system.println("mix=" + true + ", " + '한' + ", " + 10u64 + ", " + Byte.from_u8(255u8));
    system.println("math=" + (1 + 2));
    system.printf("%s:%d:%u:%f:%g:%c:%b:%%\n", "ok", -2, 3u32, 1.5, 2.0, 'A', true);
    system.printf(r#"raw:%s"#, "yes");
    system.println("");
    mut i32 count = 0;
    system.println("steps=" + next(&mut count) + "," + next(&mut count));
    if (count != 2) return 1;
    system.log(system.warn, "hi", "C1");
    system.log(system.info, "plain");
    system.log(system.error, "value=" + 5);
    mut i32 order = 0;
    system.log(code = next_code(&mut order), level = next_level(&mut order), message = next_message(&mut order));
    if (order != 123) return 2;
    system.flush();
    return 0;
}
]=] "" "values:3\nmix=true, 한, 10, 0xff\nmath=3\nok:-2:3:1.5:2:A:true:%\nraw:yes\nsteps=1,2\n" "[WARN C1] hi\n[INFO] plain\n[ERROR] value=5\n[INFO NC] named\n")

system_fixture(owned [=[
import system;
i32 main() {
    Result<string, TextError> result = string.from_view("owned");
    switch (move result) {
        case Result<string, TextError>.Ok { string text = value }:
            system.println(text);
            system.printf("%s\n", text);
            system.println(&text);
            return string.byte_len(&text) == 5 ? 0 : 1;
            break;
        case Result<string, TextError>.Err: return 2; break;
    }
}
]=] "" "owned\nowned\nowned\n" "")

system_fixture(input [=[
import system;
Result<i32, system.ReadError> read_all() {
    Optional<i32> number = system.input<i32>()?;
    switch (number) { case Optional<i32>.Some { i32 value = value }: if (value != 42) return Result.ok(1); break; case Optional<i32>.None: return Result.ok(2); break; }
    Optional<string> delimiter = system.input()?;
    switch (move delimiter) { case Optional<string>.Some { string text = value }: if (!string.is_empty(&text)) return Result.ok(3); break; case Optional<string>.None: return Result.ok(4); break; }
    Optional<string> line = system.input<string>()?;
    switch (move line) { case Optional<string>.Some { string text = value }: if (!string.equals(&text, "hello world")) return Result.ok(5); break; case Optional<string>.None: return Result.ok(6); break; }
    Optional<string> final_line = system.input()?;
    switch (move final_line) { case Optional<string>.Some { string text = value }: if (!string.equals(&text, "last")) return Result.ok(7); break; case Optional<string>.None: return Result.ok(8); break; }
    Optional<string> ended = system.input()?;
    switch (ended) { case Optional<string>.None: return Result.ok(0); break; case Optional<string>.Some: return Result.ok(9); break; }
}
i32 main() {
    Result<i32, system.ReadError> result = read_all();
    switch (result) { case Result<i32, system.ReadError>.Ok { i32 code = value }: return code; break; case Result<i32, system.ReadError>.Err: return 99; break; }
}
]=] "42\nhello world\r\nlast" "" "")

system_fixture(scalar_input [=[
import system;
Result<i32, system.ReadError> read_scalars() {
    Optional<bool> flag = system.input<bool>()?;
    switch (flag) { case Optional<bool>.Some { bool value = value }: if (!value) return Result.ok(1); break; case Optional<bool>.None: return Result.ok(2); break; }
    Optional<char> character = system.input<char>()?;
    switch (character) { case Optional<char>.Some { char value = value }: if (value != '한') return Result.ok(3); break; case Optional<char>.None: return Result.ok(4); break; }
    Optional<f64> real = system.input<f64>()?;
    switch (real) { case Optional<f64>.Some { f64 value = value }: if (value != 1.5) return Result.ok(5); break; case Optional<f64>.None: return Result.ok(6); break; }
    Optional<byte> octet = system.input<byte>()?;
    switch (octet) { case Optional<byte>.Some { byte value = value }: if (Byte.to_u8(value) != 255u8) return Result.ok(7); break; case Optional<byte>.None: return Result.ok(8); break; }
    Optional<i128> wide = system.input<i128>()?;
    switch (wide) { case Optional<i128>.Some { i128 value = value }: if (value != -170141183460469231731687303715884105728i128) return Result.ok(9); break; case Optional<i128>.None: return Result.ok(10); break; }
    Optional<i32> ended = system.input<i32>()?;
    switch (ended) { case Optional<i32>.None: return Result.ok(0); break; case Optional<i32>.Some: return Result.ok(11); break; }
}
i32 main() {
    Result<i32, system.ReadError> result = read_scalars();
    switch (result) { case Result<i32, system.ReadError>.Ok { i32 code = value }: return code; break; case Result<i32, system.ReadError>.Err: return 99; break; }
}
]=] "true 한 1.5 255 -170141183460469231731687303715884105728" "" "")

system_fixture(parse_error [=[
import system;
i32 main() {
    Result<Optional<i32>, system.ReadError> result = system.input<i32>();
    switch (move result) {
        case Result<Optional<i32>, system.ReadError>.Ok: return 1; break;
        case Result<Optional<i32>, system.ReadError>.Err { system.ReadError cause = error }:
            switch (cause) {
                case system.ReadError.Parse { ParseError problem = error }:
                    switch (problem) { case ParseError.InvalidSyntax: return 0; break; default: return 2; break; }
                    break;
                case system.ReadError.Io: return 3; break;
            }
            break;
    }
}
]=] "wrong\n" "" "")

string(ASCII 255 invalid_utf8)
system_fixture(encoding_error [=[
import system;
i32 main() {
    Result<Optional<string>, system.ReadError> result = system.input();
    switch (move result) {
        case Result<Optional<string>, system.ReadError>.Ok: return 1; break;
        case Result<Optional<string>, system.ReadError>.Err { system.ReadError cause = error }:
            switch (cause) {
                case system.ReadError.Io { system.IoError problem = error }:
                    switch (problem) { case system.IoError.InvalidUtf8: return 0; break; default: return 2; break; }
                    break;
                case system.ReadError.Parse: return 3; break;
            }
            break;
    }
}
]=] "${invalid_utf8}\n" "" "")

function(invalid_system name source pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    execute_process(COMMAND "${AEVIXC}" --check "${TEST_DIR}/${name}.avx"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 45)
    if(NOT "${status}" STREQUAL "1" OR NOT "${output}${error}" MATCHES "${pattern}")
        message(FATAL_ERROR "${name}: expected rejection/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
invalid_system(no_import "i32 main() { system.println(1); return 0; }" "Unknown name")
invalid_system(bad_format "import system; i32 main() { system.printf(\"%q\", 1); return 0; }" "Unsupported printf")
invalid_system(width "import system; i32 main() { system.printf(\"%04d\", 1); return 0; }" "Unsupported printf")
invalid_system(precision "import system; i32 main() { system.printf(\"%.2f\", 1.0); return 0; }" "Unsupported printf")
invalid_system(missing "import system; i32 main() { system.printf(\"%d\"); return 0; }" "Missing system.printf")
invalid_system(late_missing "import system; i32 main() { system.printf(\"%d%d\", 1); return 0; }" "Missing system.printf")
invalid_system(late_invalid "import system; i32 main() { system.printf(\"%d%q\", 1, 2); return 0; }" "Unsupported printf")
invalid_system(late_incomplete "import system; i32 main() { system.printf(\"%d%\", 1); return 0; }" "Incomplete system.printf")
invalid_system(extra "import system; i32 main() { system.printf(\"hi\", 1); return 0; }" "Unexpected system.printf")
invalid_system(mismatch "import system; i32 main() { system.printf(\"%d\", 1u32); return 0; }" "does not match")
invalid_system(dynamic "import system; i32 bad(string_view format) { system.printf(format, 1); return 0; } i32 main() { return 0; }" "compile-time string")
invalid_system(half_input "import system; i32 main() { system.input<f16>(); return 0; }" "system.input supports")
invalid_system(view_input "import system; i32 main() { system.input<string_view>(); return 0; }" "system.input supports")
invalid_system(bad_level "import system; i32 main() { system.log(1, \"hi\"); return 0; }" "requires a system.LogLevel")
invalid_system(duplicate_log "import system; i32 main() { system.log(system.info, level = system.warn, message = \"hi\"); return 0; }" "duplicated system.log")
invalid_system(unknown_log "import system; i32 main() { system.log(level = system.info, text = \"hi\"); return 0; }" "Unknown or duplicated")
invalid_system(non_output_concat "i32 main() { string_view text = \"value=\" + 1; return 0; }" "Binary operands|requires matching|Operator")
invalid_system(conflicting_loan [=[
import system;
i32 clear(&mut string value) { string.clear(value); return 0; }
i32 bad(mut string text) { system.println(text + clear(&mut text)); return 0; }
i32 main() { return 0; }
]=] "borrow|loan|Borrow")
message(STATUS "System output, checked formats, logging, input, and source restrictions passed")
