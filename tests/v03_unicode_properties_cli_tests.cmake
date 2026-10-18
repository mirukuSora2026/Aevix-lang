# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_unicode_properties_cli_tests.cmake
#
# Description :
#     고정 UCD 원본의 모든 스칼라와 이름·대소문자·할당 실패 및 소유권을 실행 검증한다.
#     Execute all scalar property, name, casing, allocation-failure, and ownership checks against pinned UCD originals.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
find_program(PYTHON_EXECUTABLE NAMES python3 REQUIRED)
get_filename_component(source_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(ucd_root "${source_root}/third_party/unicode/17.0.0")
file(MAKE_DIRECTORY "${TEST_DIR}")
file(SHA256 "${AEVIXC}" original_compiler)
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version TIMEOUT 10)
set(aot_options)
if(DEFINED RUNTIME AND EXISTS "${RUNTIME}")
    list(APPEND aot_options --runtime "${RUNTIME}")
endif()
if(DEFINED CLANG AND EXISTS "${CLANG}")
    list(APPEND aot_options --clang "${CLANG}")
endif()
function(ucd_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 240)
    if(NOT "${status}" STREQUAL "${expected}" OR (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
function(ucd_modes name)
    ucd_run("${name} check" 0 "ownership checked" --check "${TEST_DIR}/${name}.avx")
    foreach(optimization -O0 -O2)
        ucd_run("${name} AOT ${optimization}" 0 "" --run "${optimization}" ${aot_options} "${TEST_DIR}/${name}.avx")
        if(version MATCHES "ORC JIT enabled")
            ucd_run("${name} JIT ${optimization}" 0 "" --jit "${optimization}" "${TEST_DIR}/${name}.avx")
        endif()
    endforeach()
endfunction()
execute_process(COMMAND "${PYTHON_EXECUTABLE}" "${source_root}/tools/unicode/generate_ucd" --verify
    RESULT_VARIABLE verified OUTPUT_VARIABLE summary ERROR_VARIABLE error TIMEOUT 60)
if(NOT verified EQUAL 0)
    message(FATAL_ERROR "Unicode artifact reproduction failed: ${summary}${error}")
endif()
message(STATUS "${summary}")
execute_process(COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_unicode_properties_golden.py"
    --input-dir "${ucd_root}" --metadata "${source_root}/docs/Language/Library/unicode_properties.json" --output-dir "${TEST_DIR}"
    RESULT_VARIABLE generated OUTPUT_VARIABLE summary ERROR_VARIABLE error TIMEOUT 120)
if(NOT generated EQUAL 0)
    message(FATAL_ERROR "Independent UCD oracle failed: ${summary}${error}")
endif()
message(STATUS "${summary}")
foreach(pair "v03_library_unicode_properties|edges" "v03_unicode_properties_worker|worker" "v03_unicode_properties_no_allocate|no_allocate" "v03_unicode_properties_failures|faults")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 input)
    list(GET parts 1 output)
    configure_file("${CMAKE_CURRENT_LIST_DIR}/${input}.avx" "${TEST_DIR}/${output}.avx" COPYONLY)
endforeach()
file(READ "${ucd_root}/artifact-manifest.json" artifact_manifest)
string(JSON fingerprint GET "${artifact_manifest}" artifact_fingerprint)
file(WRITE "${TEST_DIR}/fingerprint.avx" "import unicode;i32 main(){return string.equals(unicode.ucd_fingerprint(),\"sha256:${fingerprint}\")?0:1;}\n")
foreach(name properties_full text_full edges worker no_allocate fingerprint)
    ucd_modes("${name}")
endforeach()
file(WRITE "${TEST_DIR}/bad_private.avx" "import unicode;i32 main(){unicode.ucd_name_pool_data();return 0;}\n")
ucd_run("packed Unicode tables stay private" 1 "private" --check "${TEST_DIR}/bad_private.avx")
file(WRITE "${TEST_DIR}/bad_domain.avx" "import unicode;i32 main(){unicode.general_category(65u32);return 0;}\n")
ucd_run("properties require valid char domain" 1 "matching|type|requires" --check "${TEST_DIR}/bad_domain.avx")
file(WRITE "${TEST_DIR}/bad_carrier.avx" "import unicode;Result<string,CharError> wrong(){return unicode.to_uppercase('A');}i32 main(){return 0;}\n")
ucd_run("allocation errors keep explicit UnicodeError carrier" 1 "requires|type" --check "${TEST_DIR}/bad_carrier.avx")
file(WRITE "${TEST_DIR}/no_import.avx" "i32 main(){unicode.is_alphabetic('A');return 0;}\n")
ucd_run("Unicode property module requires import" 1 "Unknown|import|name" --check "${TEST_DIR}/no_import.avx")
file(WRITE "${TEST_DIR}/bad_owner.avx" [=[
import unicode;
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok {T value=value}:return value;break;default:Never.unreachable();break;}}
string_view escape(){Optional<string> name=success(unicode.name('A'));switch(move name){case Optional<string>.Some {string owned=value}:return string.view(&owned);break;default:Never.unreachable();break;}}
i32 main(){return 0;}
]=])
ucd_run("owned name view cannot escape" 1 "local|borrow|outlive|escape" --check "${TEST_DIR}/bad_owner.avx")
file(WRITE "${TEST_DIR}/bad_scripts.avx" [=[
import unicode;
T success<T,E>(Result<T,E> input){switch(move input){case Result<T,E>.Ok {T value=value}:return value;break;default:Never.unreachable();break;}}
&unicode.Script escape(){Vector<unicode.Script> scripts=success(unicode.script_extensions('·'));return &scripts[0];}
i32 main(){return 0;}
]=])
ucd_run("script set element cannot outlive vector" 1 "local|borrow|outlive|escape" --check "${TEST_DIR}/bad_scripts.avx")
execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/no_allocate.avx" RESULT_VARIABLE emitted OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 180)
if(NOT emitted EQUAL 0)
    message(FATAL_ERROR "Cannot emit allocation-free property fixture: ${error}")
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
 for target in re.findall(r'\bcall\b[^\n]*?@([^ (]+)\(',functions.get(name,'')):
  if target in {'malloc','calloc','realloc'} or target.startswith(('aevix_text_copy','aevix_text_append','aevix_text_push_char')):raise SystemExit('unexpected allocator reachable: '+target)
  if target in functions:pending.append(target)
if len(seen)<50:raise SystemExit('closed property call graph was not inspected')
for name in ['aevix.module.initialize','aevix.module.shutdown']:
 if functions.get(name,'').strip()!='entry:\n  ret void':raise SystemExit('unexpected Unicode startup/shutdown work: '+name)
print(str(len(seen))+' reachable functions: no allocator; existing module initialization/shutdown wrappers are empty')
]=] "${TEST_DIR}/no_allocate.ll" RESULT_VARIABLE allocation_check OUTPUT_VARIABLE summary ERROR_VARIABLE error TIMEOUT 15)
if(NOT allocation_check EQUAL 0)
    message(FATAL_ERROR "Unicode immutable-table check failed: ${summary}${error}")
endif()
message(STATUS "${summary}")
if(DEFINED CLANG AND EXISTS "${CLANG}" AND DEFINED RUNTIME AND EXISTS "${RUNTIME}")
    ucd_run("allocation failure source" 0 "ownership checked" --check "${TEST_DIR}/faults.avx")
    execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/faults.avx" RESULT_VARIABLE emitted OUTPUT_VARIABLE failure_ir ERROR_VARIABLE error TIMEOUT 180)
    if(NOT emitted EQUAL 0)
        message(FATAL_ERROR "Cannot emit UCD allocation fixture: ${error}")
    endif()
    string(REPLACE "@malloc(" "@ucd_test_allocate(" failure_ir "${failure_ir}")
    string(REPLACE "@free(" "@ucd_test_free(" failure_ir "${failure_ir}")
    string(REPLACE "@aevix_text_copy(" "@ucd_test_text_copy(" failure_ir "${failure_ir}")
    string(REPLACE "@main(" "@ucd_test_main(" failure_ir "${failure_ir}")
    file(WRITE "${TEST_DIR}/faults.ll" "${failure_ir}")
    file(WRITE "${TEST_DIR}/faults.c" [=[
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static uint32_t fail_at,calls,outstanding,failures,baselines;
int32_t aevix_text_copy(void *,const void *,uint64_t,int32_t);
void ucd_fault_point(uint32_t point){fail_at=point;calls=0;if(point)failures++;else baselines++;}
uint32_t ucd_attempts(void){return calls;}
uint32_t ucd_outstanding(void){return outstanding;}
void *ucd_test_allocate(size_t size){if(++calls==fail_at)return NULL;void *value=malloc(size);if(value)outstanding++;return value;}
void ucd_test_free(void *value){if(value)outstanding--;free(value);}
int32_t ucd_test_text_copy(void *output,const void *input,uint64_t length,int32_t validate){if(length && ++calls==fail_at)return 1;int32_t status=aevix_text_copy(output,input,length,validate);if(!status && length)outstanding++;return status;}
extern int ucd_test_main(void);
int main(void){int result=ucd_test_main();if(result)return result;if(outstanding||baselines!=11||failures!=19)return 90;printf("11 operations, 19 failed allocation stages, zero live owners\n");return 0;}
]=])
    set(extra_libraries -pthread)
    if(APPLE)
        list(APPEND extra_libraries -lc++)
    elseif(UNIX)
        list(APPEND extra_libraries -lstdc++ -lm)
    endif()
    set(failure_binary "${TEST_DIR}/faults")
    if(WIN32)
        string(APPEND failure_binary ".exe")
    endif()
    foreach(optimization -O0 -O2)
        execute_process(COMMAND "${CLANG}" "${optimization}" "${TEST_DIR}/faults.ll" "${TEST_DIR}/faults.c" "${RUNTIME}" ${extra_libraries} -o "${failure_binary}" RESULT_VARIABLE linked OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
        if(NOT linked EQUAL 0)
            message(FATAL_ERROR "Cannot link UCD allocation gate: ${output}${error}")
        endif()
        execute_process(COMMAND "${failure_binary}" RESULT_VARIABLE ran OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
        if(NOT ran EQUAL 0)
            message(FATAL_ERROR "UCD allocation cleanup failed: ${ran}/${output}${error}")
        endif()
        message(STATUS "${optimization}: ${output}")
    endforeach()
else()
    message(STATUS "UCD allocation execution requires CLANG and RUNTIME; no allocation-failure claim is made for this run.")
endif()
file(SHA256 "${AEVIXC}" final_compiler)
if(NOT original_compiler STREQUAL final_compiler)
    message(FATAL_ERROR "Compiler binary changed during Unicode property gate")
endif()
message(STATUS "Pinned Unicode 17 full scalar property/case/name/alias oracles and applicable ownership/allocation gates passed")
