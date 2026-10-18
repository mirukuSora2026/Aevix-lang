# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_float_bits_cli_tests.cmake
#
# Description :
#     모든 16비트 패턴·독립 숫자 기준값·표현 보존과 내장 정체성을 검사한다.
#     Check every 16-bit pattern, independent numeric goldens, representation preservation, and builtin identity.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
find_program(PYTHON_EXECUTABLE NAMES python3 REQUIRED)
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version TIMEOUT 10)
file(SHA256 "${AEVIXC}" original_compiler)
function(bits_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(bits_modes name)
    bits_run("${name} check" 0 "ownership checked" --check "${TEST_DIR}/${name}.avx")
    foreach(optimization -O0 -O2)
        bits_run("${name} AOT ${optimization}" 0 "" --run "${optimization}" "${TEST_DIR}/${name}.avx")
        if(version MATCHES "ORC JIT enabled")
            bits_run("${name} JIT ${optimization}" 0 "" --jit "${optimization}" "${TEST_DIR}/${name}.avx")
        endif()
    endforeach()
endfunction()
function(bits_reject name source pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    bits_run("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()
file(WRITE "${TEST_DIR}/exhaustive.avx" [=[
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
i32 main() {
    mut u32 raw = 0u32;
    while (raw < 65536u32) {
        u16 word = success(Integer.convert<u16>(raw));
        f16 half = Float.from_bits<f16>(word);
        bf16 brain = Float.from_bits<bf16>(word);
        if (Float.to_bits(half) != word || Float.to_bits(brain) != word) return 1;
        if (Float.to_bits(Float.from_bits<f16>(word)) != word || Float.to_bits(Float.from_bits<bf16>(word)) != word) return 2;
        raw++;
    }
    if (Float.to_bits(1.0f16) != 15360u16 || Float.to_bits(-2.5f16) != 49408u16) return 3;
    if (Float.to_bits(0.0f16) != 0u16 || Float.to_bits(-0.0f16) != 32768u16) return 4;
    if (Float.to_bits(65504.0f16) != 31743u16 || Float.to_bits(0.000000059604644775390625f16) != 1u16) return 5;
    if (Float.to_bits(1.0bf16) != 16256u16 || Float.to_bits(-2.5bf16) != 49184u16) return 6;
    if (Float.to_bits(0.0bf16) != 0u16 || Float.to_bits(-0.0bf16) != 32768u16) return 7;
    if (Float.to_bits(1.1754943508222875e-38bf16) != 128u16 || Float.to_bits(9.183549615799121e-41bf16) != 1u16) return 8;
    f16 half_nan = Float.from_bits<f16>(31745u16);
    bf16 brain_nan = Float.from_bits<bf16>(32641u16);
    if (half_nan == half_nan || brain_nan == brain_nan) return 9;
    if (!(Float.from_bits<f16>(31744u16) > 65504.0f16) || !(Float.from_bits<bf16>(32640u16) > 1.0bf16)) return 10;
    return 0;
}
]=])
file(WRITE "${TEST_DIR}/composition.avx" [=[
type Half = f16;
type Word = u16;
type Double = float;
T identity<T>(T input) { return input; }
Result<T,unit> wrap<T>(T input) { return Result.ok(input); }
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; case Result<T,E>.Err: Never.unreachable(); break; } }
i32 main() {
    Word input = 15360u16;
    Half half = identity(Float.from_bits<Half>(value = input));
    u16 half_word = identity(Float.to_bits(value = half));
    f32 single = success(wrap(Float.from_bits<f32>(1069547520u32)));
    u32 single_word = success(wrap(Float.to_bits(single)));
    Double double_value = identity(Float.from_bits<Double>(4609434218613702656u64));
    u64 double_word = identity(Float.to_bits(double_value));
    return half_word == input && single_word == 1069547520u32 && double_word == 4609434218613702656u64 ? 0 : 1;
}
]=])
execute_process(COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_float_bits_golden.py" --output "${TEST_DIR}/golden.avx" --metadata "${TEST_DIR}/golden.json"
    RESULT_VARIABLE generated OUTPUT_VARIABLE summary ERROR_VARIABLE error TIMEOUT 30)
if(NOT "${generated}" STREQUAL "0")
    message(FATAL_ERROR "Independent Float bit oracle failed: ${error}")
endif()
message(STATUS "${summary}")
foreach(name exhaustive composition golden)
    bits_modes("${name}")
endforeach()
execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/composition.avx" RESULT_VARIABLE emitted OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 90)
if(NOT "${emitted}" STREQUAL "0" OR "${ir}" MATCHES "@malloc|@calloc|@realloc|@aevix_numeric_|@aevix_parse_")
    message(FATAL_ERROR "Float bit conversion introduced numeric runtime conversion or allocation: ${error}")
endif()
foreach(case missing_destination extra_destination explicit_to_bits wrong_name extra_value no_value wrong_destination wrong_container signed_width wrong_width size_word float_word unsuffixed borrow integer bool byte text)
    if(case STREQUAL "missing_destination")
        set(expression "Float.from_bits(0u32)")
    elseif(case STREQUAL "extra_destination")
        set(expression "Float.from_bits<f32,f64>(0u32)")
    elseif(case STREQUAL "explicit_to_bits")
        set(expression "Float.to_bits<f32>(1.0f32)")
    elseif(case STREQUAL "wrong_name")
        set(expression "Float.from_bits<f32>(other = 0u32)")
    elseif(case STREQUAL "extra_value")
        set(expression "Float.to_bits(1.0f32,1.0f32)")
    elseif(case STREQUAL "no_value")
        set(expression "Float.from_bits<f32>()")
    elseif(case STREQUAL "wrong_destination")
        set(expression "Float.from_bits<i32>(0u32)")
    elseif(case STREQUAL "wrong_container")
        set(expression "Float.from_bits<Array<f32,2>>(0u32)")
    elseif(case STREQUAL "signed_width")
        set(expression "Float.from_bits<f32>(0i32)")
    elseif(case STREQUAL "wrong_width")
        set(expression "Float.from_bits<f64>(0u32)")
    elseif(case STREQUAL "size_word")
        set(expression "Float.from_bits<f64>(0usize)")
    elseif(case STREQUAL "float_word")
        set(expression "Float.from_bits<f32>(0.0f32)")
    elseif(case STREQUAL "unsuffixed")
        set(expression "Float.from_bits<f32>(0)")
    elseif(case STREQUAL "borrow")
        set(expression "Float.to_bits(&input)")
    elseif(case STREQUAL "integer")
        set(expression "Float.to_bits(7)")
    elseif(case STREQUAL "bool")
        set(expression "Float.to_bits(true)")
    elseif(case STREQUAL "byte")
        set(expression "Float.to_bits(Byte.from_u8(0u8))")
    else()
        set(expression "Float.to_bits(\"text\")")
    endif()
    bits_reject("bad_${case}" "i32 main(){f32 input=1.0f32;rsn invalid=${expression};return 0;}" "Float|float|unsigned|bit|format|argument")
endforeach()
file(MAKE_DIRECTORY "${TEST_DIR}/shadow")
file(WRITE "${TEST_DIR}/shadow/Float.avx" "public i32 to_bits(i32 value){return value+1;}public i32 from_bits<T>(i32 value){return value+2;}\n")
file(WRITE "${TEST_DIR}/shadow/relay.avx" "import Float;public i32 run<T>(T ignored){return Float.from_bits<i32>(40); }\n")
file(WRITE "${TEST_DIR}/shadow/module.avx" "import Float;import relay;T identity<T>(T value){return value;}i32 main(){return identity(Float.to_bits(41))==42 && identity(Float.from_bits<i32>(40))==42 && relay.run(0)==42?0:1;}\n")
file(WRITE "${TEST_DIR}/shadow/local.avx" "struct User {public i32 to_bits(&Self self,i32 value){return value+1;}public i32 from_bits(&Self self,i32 value){return value+2;}};T identity<T>(T value){return value;}i32 main(){User Float=User{};return identity(Float.to_bits(41))==42 && identity(Float.from_bits(40))==42?0:1;}\n")
foreach(name module local)
    bits_run("${name} shadow check" 0 "ownership checked" --check "${TEST_DIR}/shadow/${name}.avx")
    foreach(optimization -O0 -O2)
        bits_run("${name} shadow AOT ${optimization}" 0 "" --run "${optimization}" "${TEST_DIR}/shadow/${name}.avx")
        if(version MATCHES "ORC JIT enabled")
            bits_run("${name} shadow JIT ${optimization}" 0 "" --jit "${optimization}" "${TEST_DIR}/shadow/${name}.avx")
        endif()
    endforeach()
endforeach()
file(MAKE_DIRECTORY "${TEST_DIR}/transitive")
file(WRITE "${TEST_DIR}/transitive/Float.avx" "public i32 to_bits(i32 value){return value+1;}public i32 from_bits<T>(i32 value){return value+2;}\n")
file(WRITE "${TEST_DIR}/transitive/holder.avx" "import Float;public i32 value(){return 42;}\n")
file(WRITE "${TEST_DIR}/transitive/main.avx" "import holder;T identity<T>(T input){return input;}i32 main(){return holder.value()==42 && identity(Float.to_bits(Float.from_bits<f32>(1069547520u32)))==1069547520u32?0:1;}\n")
bits_run("transitive builtin check" 0 "ownership checked" --check "${TEST_DIR}/transitive/main.avx")
foreach(optimization -O0 -O2)
    bits_run("transitive builtin AOT ${optimization}" 0 "" --run "${optimization}" "${TEST_DIR}/transitive/main.avx")
    if(version MATCHES "ORC JIT enabled")
        bits_run("transitive builtin JIT ${optimization}" 0 "" --jit "${optimization}" "${TEST_DIR}/transitive/main.avx")
    endif()
endforeach()
file(SHA256 "${AEVIXC}" final_compiler)
if(NOT original_compiler STREQUAL final_compiler)
    message(FATAL_ERROR "Compiler binary changed during Float bits gate")
endif()
message(STATUS "Float bits exhaustive patterns, numeric goldens, composition, and namespace identity passed")
