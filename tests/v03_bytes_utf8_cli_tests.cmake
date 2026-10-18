# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_bytes_utf8_cli_tests.cmake
#
# Description :
#     엄격한 UTF-8 바이트 API의 정확한 위치·소유권·할당 실패를 검사한다.
#     Check exact offsets, independent ownership, and allocation failure for strict UTF-8 byte APIs.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
find_program(PYTHON_EXECUTABLE NAMES python3 REQUIRED)
file(MAKE_DIRECTORY "${TEST_DIR}")
file(SHA256 "${AEVIXC}" compiler_sha)
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version TIMEOUT 10)
message(STATUS "Bytes UTF-8 gate uses compiler SHA256 ${compiler_sha}")
function(utf8_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 120)
    if(NOT "${status}" STREQUAL "${expected}" OR
        (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
configure_file("${CMAKE_CURRENT_LIST_DIR}/v03_library_bytes_utf8.avx" "${TEST_DIR}/utf8_usage.avx" COPYONLY)
execute_process(COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_bytes_golden.py"
    --output "${TEST_DIR}/base_goldens.avx" --metadata "${TEST_DIR}/utf8_goldens.json" --utf8-output "${TEST_DIR}/utf8_goldens.avx"
    RESULT_VARIABLE generated ERROR_VARIABLE error OUTPUT_QUIET TIMEOUT 30)
if(NOT "${generated}" STREQUAL "0")
    message(FATAL_ERROR "UTF-8 independent golden generation failed: ${error}")
endif()
foreach(fixture utf8_usage utf8_goldens)
    utf8_run("${fixture} check" 0 "ownership checked" --check "${TEST_DIR}/${fixture}.avx")
    foreach(optimization -O0 -O2)
        utf8_run("${fixture} AOT ${optimization}" 0 "" --run "${optimization}" "${TEST_DIR}/${fixture}.avx")
        if(version MATCHES "ORC JIT enabled")
            utf8_run("${fixture} JIT ${optimization}" 0 "" --jit "${optimization}" "${TEST_DIR}/${fixture}.avx")
        endif()
    endforeach()
endforeach()
file(WRITE "${TEST_DIR}/distinct_u8.avx" "import bytes;i32 main(){u8 input[1]=[65u8];bytes.utf8_validate(Slice.from_array(&input));return 0;}\n")
utf8_run("UTF-8 byte distinct from u8" 1 "match|requires|received|compatible" --check "${TEST_DIR}/distinct_u8.avx")
file(WRITE "${TEST_DIR}/unsupported_lossy.avx" "import bytes;i32 main(){byte input[0]=[];bytes.decode_utf8_lossy(Slice.from_array(&input));return 0;}\n")
utf8_run("lossy decoding is not silently supplied" 1 "not imported" --check "${TEST_DIR}/unsupported_lossy.avx")

if(DEFINED CLANG AND EXISTS "${CLANG}" AND DEFINED RUNTIME AND EXISTS "${RUNTIME}")
    file(WRITE "${TEST_DIR}/utf8_failures.avx" [=[
import bytes;
extern "C" void utf8_fault_point(u32 attempt);
extern "C" u32 utf8_attempts();
extern "C" u32 utf8_outstanding();
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok{T value=value}:return value;break;default:Never.unreachable();break;}}
bool allocation<T>(Result<T,bytes.ByteError> input){switch(move input){case Result<T,bytes.ByteError>.Err{bytes.ByteError reason=error}:switch(reason){case bytes.ByteError.AllocationFailure:return true;break;default:return false;break;}break;default:return false;break;}}
bool invalid<T>(Result<T,bytes.ByteError> input){switch(move input){case Result<T,bytes.ByteError>.Err{bytes.ByteError reason=error}:switch(reason){case bytes.ByteError.InvalidUtf8{usize offset=offset}:return offset==2 && string.equals(reason.code(),"bytes.invalid_utf8");break;default:return false;break;}break;default:return false;break;}}
i32 retained_text() {
    utf8_fault_point(0);
    string owner=success(string.from_view("A\0한\u{1f600}"));
    u32 before=utf8_outstanding();
    utf8_fault_point(1);
    if(!allocation(bytes.encode_utf8(string.view(&owner))) || utf8_attempts()!=1 || utf8_outstanding()!=before || !string.equals(string.view(&owner),"A\0한\u{1f600}"))return 1;
    return 0;
}
i32 main() {
    byte malformed[3]=[Byte.from_u8(65u8),Byte.from_u8(194u8),Byte.from_u8(32u8)];
    utf8_fault_point(1);
    if(!invalid(bytes.utf8_validate(Slice.from_array(&malformed))) || !invalid(bytes.decode_utf8(Slice.from_array(&malformed))) || utf8_attempts()!=0 || utf8_outstanding()!=0)return 1;
    if(Byte.to_u8(malformed[0])!=65u8 || Byte.to_u8(malformed[1])!=194u8 || Byte.to_u8(malformed[2])!=32u8)return 2;
    byte valid[4]=[Byte.from_u8(240u8),Byte.from_u8(159u8),Byte.from_u8(152u8),Byte.from_u8(128u8)];
    utf8_fault_point(1);
    if(!allocation(bytes.decode_utf8(Slice.from_array(&valid))) || utf8_attempts()!=1 || utf8_outstanding()!=0)return 3;
    utf8_fault_point(1);
    if(!allocation(bytes.encode_utf8("A\0한\u{1f600}")) || utf8_attempts()!=1 || utf8_outstanding()!=0)return 4;
    if(retained_text()!=0 || utf8_outstanding()!=0)return 5;
    utf8_fault_point(0);
    {
        string decoded=success(bytes.decode_utf8(Slice.from_array(&valid)));
        Vector<byte> encoded=success(bytes.encode_utf8(string.view(&decoded)));
        if(!string.equals(string.view(&decoded),"\u{1f600}") || Vector.len(&encoded)!=4 || Byte.to_u8(encoded[0])!=240u8 || Byte.to_u8(encoded[3])!=128u8)return 6;
    }
    if(utf8_attempts()!=2 || utf8_outstanding()!=0)return 7;
    utf8_fault_point(1);
    {
        byte empty[0]=[];
        success(bytes.utf8_validate(Slice.from_array(&empty)));
        string decoded=success(bytes.decode_utf8(Slice.from_array(&empty)));
        Vector<byte> encoded=success(bytes.encode_utf8(""));
        if(!string.is_empty(string.view(&decoded)) || Vector.len(&encoded)!=0)return 8;
    }
    if(utf8_attempts()!=0 || utf8_outstanding()!=0)return 9;
    return 0;
}
]=])
    utf8_run("UTF-8 fault source check" 0 "" --check "${TEST_DIR}/utf8_failures.avx")
    execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/utf8_failures.avx" RESULT_VARIABLE emitted OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 120)
    if(NOT "${emitted}" STREQUAL "0")
        message(FATAL_ERROR "UTF-8 fault IR emission failed: ${error}")
    endif()
    # 실제 검사된 모듈의 할당 지점만 바꾸며 UTF-8 검사를 대신하지 않는다.
    # Instrument real checked-module allocations without replacing UTF-8 validation.
    string(REPLACE "@malloc(" "@utf8_test_allocate(" ir "${ir}")
    string(REPLACE "@free(" "@utf8_test_free(" ir "${ir}")
    string(REPLACE "@aevix_text_copy(" "@utf8_test_text_copy(" ir "${ir}")
    file(WRITE "${TEST_DIR}/utf8_failures.ll" "${ir}")
    file(WRITE "${TEST_DIR}/utf8_failures.c" [=[
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
static uint32_t fail_at,calls,outstanding;
int32_t aevix_text_copy(void *,const void *,uint64_t,int32_t);
void utf8_fault_point(uint32_t point){fail_at=point;calls=0;}
uint32_t utf8_attempts(void){return calls;}
uint32_t utf8_outstanding(void){return outstanding;}
void *utf8_test_allocate(size_t size){if(++calls==fail_at)return NULL;void *value=malloc(size);if(value)outstanding++;return value;}
void utf8_test_free(void *value){if(value)outstanding--;free(value);}
int32_t utf8_test_text_copy(void *output,const void *input,uint64_t length,int32_t validate){
    if(length && ++calls==fail_at)return 1;
    int32_t status=aevix_text_copy(output,input,length,validate);if(!status && length)outstanding++;return status;
}
]=])
    set(extra_libraries -pthread)
    if(APPLE)
        list(APPEND extra_libraries -lc++)
    elseif(UNIX)
        list(APPEND extra_libraries -lstdc++ -lm)
    endif()
    foreach(optimization -O0 -O2)
        set(binary "${TEST_DIR}/utf8_failures${optimization}")
        if(WIN32)
            string(APPEND binary ".exe")
        endif()
        execute_process(COMMAND "${CLANG}" "${optimization}" "${TEST_DIR}/utf8_failures.ll" "${TEST_DIR}/utf8_failures.c" "${RUNTIME}" ${extra_libraries} -o "${binary}" RESULT_VARIABLE linked OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
        if(NOT "${linked}" STREQUAL "0")
            message(FATAL_ERROR "UTF-8 fault fixture link failed: ${output}${error}")
        endif()
        execute_process(COMMAND "${binary}" RESULT_VARIABLE ran OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
        if(NOT "${ran}" STREQUAL "0")
            message(FATAL_ERROR "UTF-8 allocation cleanup failed (${ran}): ${output}${error}")
        endif()
    endforeach()
else()
    message(STATUS "UTF-8 deterministic allocation gate requires CLANG and RUNTIME")
endif()
message(STATUS "Bytes strict UTF-8 gates passed; lossy and streaming policies remain separate.")
