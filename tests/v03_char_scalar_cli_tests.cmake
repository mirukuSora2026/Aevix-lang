# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_char_scalar_cli_tests.cmake
#
# Description :
#     모든 유니코드 코드 포인트의 스칼라 유효성과 정확한 오류·내장 정체성을 검사한다.
#     Check scalar validity for every Unicode code point, exact rejection values, and builtin identity.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version TIMEOUT 10)
file(SHA256 "${AEVIXC}" original_compiler)
function(char_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 120)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(char_modes name path)
    char_run("${name} check" 0 "ownership checked" --check "${path}")
    foreach(optimization -O0 -O2)
        char_run("${name} AOT ${optimization}" 0 "" --run "${optimization}" "${path}")
        if(version MATCHES "ORC JIT enabled")
            char_run("${name} JIT ${optimization}" 0 "" --jit "${optimization}" "${path}")
        endif()
    endforeach()
endfunction()
set(header [=[
T success<T,E>(Result<T,E> input) {switch(move input){case Result<T,E>.Ok {T value=value}:return value;break;default:Never.unreachable();break;}}
]=])
file(WRITE "${TEST_DIR}/exhaustive.avx" "${header}\n" [=[
i32 main() {
    mut u32 raw = 0u32;
    mut u32 valid = 0u32;
    mut u32 rejected = 0u32;
    while (raw <= 1114111u32) {
        bool scalar = raw < 55296u32 || raw > 57343u32;
        Result<char,CharError> result = Char.from_u32(raw);
        switch(move result) {
            case Result<char,CharError>.Ok {char value=value}:
                if (!scalar || Char.to_u32(value) != raw) return 1;
                valid++;
                break;
            case Result<char,CharError>.Err {CharError reason=error}:
                if (scalar) return 2;
                switch(reason) {
                    case CharError.InvalidScalar {u32 original=value}:
                        if (original != raw) return 3;
                        break;
                }
                rejected++;
                break;
        }
        raw++;
    }
    return valid == 1112064u32 && rejected == 2048u32 ? 0 : 4;
}
]=])
file(WRITE "${TEST_DIR}/boundaries.avx" "${header}\n" [=[
T identity<T>(T input) { return input; }
bool fails(u32 input) {
    Result<char,CharError> result = Char.from_u32(input);
    switch(move result) {
        case Result<char,CharError>.Err {CharError reason=error}:
            switch(reason) {case CharError.InvalidScalar {u32 original=value}: return original == input; break;}
            break;
        default: return false; break;
    }
}
i32 main() {
    if (Char.to_u32('\0') != 0u32 || Char.to_u32('A') != 65u32 || Char.to_u32('한') != 54620u32 || Char.to_u32('\u{1F600}') != 128512u32) return 1;
    if (identity(Char.to_u32(success(Char.from_u32(value=65)))) != 65u32) return 2;
    if (Char.to_u32(success(Char.from_u32(0))) != 0u32 || Char.to_u32(success(Char.from_u32(1114111))) != 1114111u32) return 3;
    if (Char.to_u32(success(Char.from_u32(55295u32))) != 55295u32 || Char.to_u32(success(Char.from_u32(57344u32))) != 57344u32) return 4;
    if (Char.to_u32(success(Char.from_u32(64976u32))) != 64976u32 || Char.to_u32(success(Char.from_u32(65534u32))) != 65534u32 || Char.to_u32(success(Char.from_u32(65535u32))) != 65535u32) return 5;
    if (Char.to_u32(success(Char.from_u32(57344u32))) != 57344u32 || Char.to_u32(success(Char.from_u32(888u32))) != 888u32) return 6;
    return fails(55296u32) && fails(57343u32) && fails(1114112u32) && fails(4294967295u32) ? 0 : 7;
}
]=])
file(WRITE "${TEST_DIR}/worker.avx" "import task {Task,TaskScope,Executor,ScopeOptions};\n${header}\n" [=[
i32 main() {
    Executor executor = Executor.native_threads();
    TaskScope scope = success(Task.open_scope(&executor,ScopeOptions.defaults()));
    u32 input = 1114111u32;
    Task<Result<char,CharError>> work = success(Task.spawn_owned(&scope,[input]() -> Result<char,CharError>{return Char.from_u32(input);}));
    char scalar = success(success(Task.join(work)));
    success(Task.close_scope(scope));
    return Char.to_u32(scalar) == input ? 0 : 1;
}
]=])
foreach(name exhaustive boundaries worker)
    char_modes("${name}" "${TEST_DIR}/${name}.avx")
endforeach()
execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/boundaries.avx" RESULT_VARIABLE emitted OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 120)
if(NOT "${emitted}" STREQUAL "0" OR "${ir}" MATCHES "@(malloc|calloc|realloc|aevix_parse_scalar)[(]")
    message(FATAL_ERROR "Scalar Char conversion introduced an allocation or parsing runtime dependency: ${error}")
endif()
foreach(case signed narrow wide size floating byte borrowed wrong_to_input explicit_type extra_type wrong_name no_value extra_value namespace_type)
    set(prefix "")
    if(case STREQUAL "signed")
        set(prefix "i32 input=65;")
        set(expression "Char.from_u32(input)")
    elseif(case STREQUAL "narrow")
        set(expression "Char.from_u32(65u16)")
    elseif(case STREQUAL "wide")
        set(expression "Char.from_u32(65u64)")
    elseif(case STREQUAL "size")
        set(expression "Char.from_u32(65usize)")
    elseif(case STREQUAL "floating")
        set(expression "Char.from_u32(65.0f32)")
    elseif(case STREQUAL "byte")
        set(expression "Char.from_u32(Byte.from_u8(65u8))")
    elseif(case STREQUAL "borrowed")
        set(prefix "u32 input=65u32;")
        set(expression "Char.from_u32(&input)")
    elseif(case STREQUAL "wrong_to_input")
        set(expression "Char.to_u32(65u32)")
    elseif(case STREQUAL "explicit_type")
        set(expression "Char.from_u32<u32>(65u32)")
    elseif(case STREQUAL "extra_type")
        set(expression "Char.to_u32<char,u32>('A')")
    elseif(case STREQUAL "wrong_name")
        set(expression "Char.from_u32(other=65u32)")
    elseif(case STREQUAL "no_value")
        set(expression "Char.from_u32()")
    elseif(case STREQUAL "extra_value")
        set(expression "Char.to_u32('A','B')")
    else()
        set(expression "Char<u32>.from_u32(65u32)")
    endif()
    file(WRITE "${TEST_DIR}/invalid_${case}.avx" "i32 main(){${prefix}rsn invalid=${expression};return 0;}\n")
    char_run("reject ${case}" 1 "Char|char|u32|type|argument|scalar|overload|borrow" --check "${TEST_DIR}/invalid_${case}.avx")
endforeach()
set(shadow "${TEST_DIR}/shadow")
file(MAKE_DIRECTORY "${shadow}")
file(WRITE "${shadow}/Char.avx" "public i32 from_u32<T>(i32 value){return value+1;}public i32 to_u32(i32 value){return value+2;}\n")
file(WRITE "${shadow}/relay.avx" "import Char;public i32 run<T>(T ignored){return Char.from_u32<i32>(41);}\n")
file(WRITE "${shadow}/main.avx" "import Char;import relay;T identity<T>(T input){return input;}i32 main(){return identity(Char.from_u32<i32>(41))==42 && identity(Char.to_u32(40))==42 && relay.run(0)==42?0:1;}\n")
char_modes(user_module "${shadow}/main.avx")
file(WRITE "${shadow}/local.avx" "struct User{public i32 from_u32(&Self self,i32 value){return value+1;}public i32 to_u32(&Self self,i32 value){return value+2;}};T identity<T>(T input){return input;}i32 main(){User Char=User{};return identity(Char.from_u32(41))==42 && identity(Char.to_u32(40))==42?0:1;}\n")
char_modes(user_value "${shadow}/local.avx")
set(transitive "${TEST_DIR}/transitive")
file(MAKE_DIRECTORY "${transitive}")
file(WRITE "${transitive}/Char.avx" "public i32 from_u32<T>(i32 value){return value+1;}public i32 to_u32(i32 value){return value+2;}\n")
file(WRITE "${transitive}/holder.avx" "import Char;public i32 value(){return Char.from_u32<i32>(41);}\n")
file(WRITE "${transitive}/main.avx" "import holder;${header}\ni32 main(){return holder.value()==42 && Char.to_u32(success(Char.from_u32(65)))==65u32?0:1;}\n")
char_modes(transitive_only "${transitive}/main.avx")
file(SHA256 "${AEVIXC}" final_compiler)
if(NOT original_compiler STREQUAL final_compiler)
    message(FATAL_ERROR "Compiler binary changed during Char scalar gate")
endif()
message(STATUS "Char scalar validation, exact errors, worker transfer, and namespace identity passed")
