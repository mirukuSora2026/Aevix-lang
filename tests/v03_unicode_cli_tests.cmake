# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_unicode_cli_tests.cmake
#
# Description :
#     유니코드 스칼라 라이브러리의 독립 UTF 기준값, 오류 위치, 출력 원자성과 소유권을 검사한다.
#     Check independent UTF goldens, error offsets, transactional output, and owned Unicode library values.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
find_program(PYTHON_EXECUTABLE NAMES python3 REQUIRED)
file(MAKE_DIRECTORY "${TEST_DIR}")
file(SHA256 "${AEVIXC}" original_compiler)
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version TIMEOUT 10)
function(unicode_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 180)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(unicode_modes name)
    unicode_run("${name} check" 0 "ownership checked" --check "${TEST_DIR}/${name}.avx")
    foreach(optimization -O0 -O2)
        unicode_run("${name} AOT ${optimization}" 0 "" --run "${optimization}" "${TEST_DIR}/${name}.avx")
        if(version MATCHES "ORC JIT enabled")
            unicode_run("${name} JIT ${optimization}" 0 "" --jit "${optimization}" "${TEST_DIR}/${name}.avx")
        endif()
    endforeach()
endfunction()
execute_process(COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_unicode_golden.py" --output-dir "${TEST_DIR}" RESULT_VARIABLE generated OUTPUT_VARIABLE summary ERROR_VARIABLE error TIMEOUT 60)
if(NOT generated EQUAL 0)
    message(FATAL_ERROR "Independent Unicode codec oracle failed: ${error}")
endif()
message(STATUS "${summary}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/v03_library_unicode.avx" "${TEST_DIR}/usage.avx" COPYONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/v03_unicode_malformed.avx" "${TEST_DIR}/malformed.avx" COPYONLY)
file(WRITE "${TEST_DIR}/worker.avx" [=[
import unicode;
import task {Task,TaskScope,Executor,ScopeOptions};
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok {T value=value}:return value;break;default:Never.unreachable();break;}}
i32 main(){
    Executor executor=Executor.native_threads();TaskScope scope=success(Task.open_scope(&executor,ScopeOptions.defaults()));
    byte input[4]=[Byte.from_u8(240u8),Byte.from_u8(159u8),Byte.from_u8(152u8),Byte.from_u8(128u8)];
    Task<Result<char,unicode.UnicodeError>> work=success(Task.spawn_owned(&scope,[input]()->Result<char,unicode.UnicodeError>{return unicode.from_utf8(Slice.from_array(&input));}));
    char output=success(success(Task.join(work)));success(Task.close_scope(scope));return output=='😀'?0:1;
}
]=])
file(WRITE "${TEST_DIR}/no_allocate.avx" [=[
import unicode;
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok {T value=value}:return value;break;default:Never.unreachable();break;}}
i32 main(){
    Tuple<Array<byte,4>,u32> encoded=unicode.to_utf8('😀');
    &Array<byte,4> bytes=Tuple.get<0>(&encoded);
    Tuple<char,usize> decoded=success(unicode.decode_utf8_at(Slice.from_array(bytes),0usize));
    return *Tuple.get<0>(&decoded)=='😀'&&*Tuple.get<1>(&decoded)==4usize?0:1;
}
]=])
foreach(name exhaustive usage malformed worker no_allocate)
    unicode_modes("${name}")
endforeach()
# 미사용 공개 함수도 IR에 존재할 수 있어 main에서 도달하는 함수들의 본문만 검사한다.
# Unused public bodies can remain in IR; inspect only the transitive bodies reachable from main.
execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/no_allocate.avx" RESULT_VARIABLE emitted OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 180)
if(NOT emitted EQUAL 0)
    message(FATAL_ERROR "Cannot emit allocation-free fixed codec fixture: ${error}")
endif()
file(WRITE "${TEST_DIR}/no_allocate.ll" "${ir}")
execute_process(COMMAND "${PYTHON_EXECUTABLE}" -c [=[
import re,sys
text=open(sys.argv[1]).read()
functions={m.group(1):m.group(2) for m in re.finditer(r'^define\s+[^\n]*@([^ (]+)\([^\n]*\)[^\n]*\{\n(.*?)^}',text,re.M|re.S)}
pending=['main'];seen=set()
while pending:
 name=pending.pop()
 if name in seen:continue
 seen.add(name)
 body=functions.get(name,'')
 for target in re.findall(r'\bcall\b[^\n]*?@([^ (]+)\(',body):
  if target in {'malloc','calloc','realloc'} or target.startswith('aevix_text_'):raise SystemExit('unexpected allocation/text runtime reachable: '+target)
  if target in functions:pending.append(target)
if len(seen)<3:raise SystemExit('could not inspect closed codec call graph')
print('Fixed UTF codec reachable call graph is allocation-free')
]=] "${TEST_DIR}/no_allocate.ll" RESULT_VARIABLE allocation_check OUTPUT_VARIABLE allocation_summary ERROR_VARIABLE error TIMEOUT 10)
if(NOT allocation_check EQUAL 0)
    message(FATAL_ERROR "Allocation-free fixed codec check failed: ${allocation_summary}${error}")
endif()
file(WRITE "${TEST_DIR}/bad_type.avx" "import unicode;i32 main(){unicode.to_utf8(65u32);return 0;}\n")
unicode_run("char domain remains explicit" 1 "type|matching|requires|overload" --check "${TEST_DIR}/bad_type.avx")
file(WRITE "${TEST_DIR}/bad_view.avx" [=[
import unicode;
import error {AnyError,Error};
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok {T value=value}:return value;break;default:Never.unreachable();break;}}
&dyn Error escape(){AnyError owner=success(AnyError.pack(unicode.UnicodeError.EmptyInput));return owner.as_error();}
i32 main(){return 0;}
]=])
unicode_run("erased error view cannot escape its owner" 1 "local|reference|outlive|borrow|escape" --check "${TEST_DIR}/bad_view.avx")
file(WRITE "${TEST_DIR}/no_import.avx" "i32 main(){unicode.replacement();return 0;}\n")
unicode_run("unicode requires import" 1 "Unknown|import|name" --check "${TEST_DIR}/no_import.avx")

# 알려진 소스 효과는 그대로 두고 생성된 IR의 할당 기호만 실패하도록 바꾼다.
# Preserve source effects and inject allocation failure at emitted malloc/text-copy ABI boundaries.
if(DEFINED CLANG AND EXISTS "${CLANG}" AND DEFINED RUNTIME AND EXISTS "${RUNTIME}")
    file(WRITE "${TEST_DIR}/allocation_failure.avx" [=[
import unicode;
import error {AnyError};
bool allocation(Result<string,unicode.UnicodeError> input){switch(move input){case Result<string,unicode.UnicodeError>.Err {unicode.UnicodeError reason=error}:switch(reason){case unicode.UnicodeError.AllocationFailure:return true;break;default:return false;break;}break;default:return false;break;}}
i32 main(){
    char scalar='한';
    if(!allocation(unicode.to_string(scalar))||!allocation(unicode.escape_unicode(scalar))||!allocation(unicode.escape_default(scalar))||!allocation(unicode.escape_debug(scalar))||!allocation(unicode.ascii_escape('A')))return 1;
    Result<AnyError,InputError<unicode.UnicodeError>> erased=AnyError.pack(unicode.UnicodeError.BufferTooSmall {required=4usize,available=1usize});
    switch(move erased){
        case Result<AnyError,InputError<unicode.UnicodeError>>.Err {InputError<unicode.UnicodeError> reason=error}:
            switch(move reason){case InputError<unicode.UnicodeError>.AllocationFailure {unicode.UnicodeError original=input}:switch(original){case unicode.UnicodeError.BufferTooSmall {usize needed=required,usize actual=available}:return scalar=='한'&&needed==4usize&&actual==1usize?0:2;break;default:return 3;break;}break;default:return 4;break;}
            break;
        default:return 5;break;
    }
}
]=])
    unicode_run("allocation failure source check" 0 "ownership checked" --check "${TEST_DIR}/allocation_failure.avx")
    execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/allocation_failure.avx" RESULT_VARIABLE emitted OUTPUT_VARIABLE failure_ir ERROR_VARIABLE error TIMEOUT 180)
    if(NOT emitted EQUAL 0)
        message(FATAL_ERROR "Cannot emit Unicode allocation failure fixture: ${error}")
    endif()
    string(REPLACE "@malloc(" "@unicode_test_allocate(" failure_ir "${failure_ir}")
    string(REPLACE "@free(" "@unicode_test_free(" failure_ir "${failure_ir}")
    string(REPLACE "@aevix_text_copy(" "@unicode_test_text_copy(" failure_ir "${failure_ir}")
    string(REPLACE "@main(" "@unicode_test_main(" failure_ir "${failure_ir}")
    file(WRITE "${TEST_DIR}/allocation_failure.ll" "${failure_ir}")
    file(WRITE "${TEST_DIR}/allocation_failure.c" [=[
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
static int attempts;
static int frees;
void *unicode_test_allocate(size_t size){(void)size;attempts++;return NULL;}
void unicode_test_free(void *pointer){if(pointer)frees++;free(pointer);}
/* TextRuntime.hpp: status1 means allocation failure, without publishing an output owner. */
int32_t unicode_test_text_copy(void *output,const void *data,uint64_t length,int32_t validate){
    if(!output||!data||!length||validate!=1)return 3;
    attempts++;return 1;
}
extern int unicode_test_main(void);
int main(void){int result=unicode_test_main();return result?result:(attempts==6&&frees==0?0:90);}
]=])
    set(extra_libraries -pthread)
    if(APPLE)
        list(APPEND extra_libraries -lc++)
    elseif(UNIX)
        list(APPEND extra_libraries -lstdc++ -lm)
    endif()
    set(failure_binary "${TEST_DIR}/allocation_failure")
    if(WIN32)
        string(APPEND failure_binary ".exe")
    endif()
    foreach(optimization -O0 -O2)
        execute_process(COMMAND "${CLANG}" "${optimization}" "${TEST_DIR}/allocation_failure.ll" "${TEST_DIR}/allocation_failure.c" "${RUNTIME}" ${extra_libraries} -o "${failure_binary}" RESULT_VARIABLE linked OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
        if(NOT linked EQUAL 0)
            message(FATAL_ERROR "Cannot link Unicode allocation failure fixture: ${output}${error}")
        endif()
        execute_process(COMMAND "${failure_binary}" RESULT_VARIABLE ran OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
        if(NOT ran EQUAL 0)
            message(FATAL_ERROR "Unicode allocation failure ownership check failed: ${ran}/${output}${error}")
        endif()
    endforeach()
else()
    message(STATUS "Unicode deterministic allocation failure gate requires CLANG and RUNTIME; no allocation-failure execution claim is made.")
endif()
file(SHA256 "${AEVIXC}" final_compiler)
if(NOT original_compiler STREQUAL final_compiler)
    message(FATAL_ERROR "Compiler binary changed during Unicode source gate")
endif()
message(STATUS "Unicode scalar source contracts passed exact codecs, ASCII, errors, transactional writes, owned values and applicable allocator gates")
