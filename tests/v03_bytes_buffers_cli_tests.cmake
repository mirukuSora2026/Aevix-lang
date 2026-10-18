# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_bytes_buffers_cli_tests.cmake
#
# Description :
#     소유 바이트 버퍼의 변경·대여·실패 보존 계약을 실제 소스로 검사한다.
#     Check real-source byte owner mutation, borrowed views, and failure preservation.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
file(SHA256 "${AEVIXC}" compiler_sha)
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version TIMEOUT 10)
message(STATUS "Bytes owning-buffer gate uses compiler SHA256 ${compiler_sha}")
function(buffer_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 120)
    if(NOT "${status}" STREQUAL "${expected}" OR
       (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
configure_file("${CMAKE_CURRENT_LIST_DIR}/v03_library_bytes_buffers.avx" "${TEST_DIR}/buffer_usage.avx" COPYONLY)
buffer_run("buffer source check" 0 "ownership checked" --check "${TEST_DIR}/buffer_usage.avx")
foreach(optimization -O0 -O2)
    buffer_run("buffer AOT ${optimization}" 0 "" --run "${optimization}" "${TEST_DIR}/buffer_usage.avx")
    if(version MATCHES "ORC JIT enabled")
        buffer_run("buffer JIT ${optimization}" 0 "" --jit "${optimization}" "${TEST_DIR}/buffer_usage.avx")
    endif()
endforeach()
set(header [=[
import bytes;
T success<T,E>(Result<T,E> input) { switch (move input) { case Result<T,E>.Ok { T value = value }: return value; break; default: Never.unreachable(); break; } }
]=])
function(buffer_reject name text pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${header}\n${text}\n")
    buffer_run("${name} rejected" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()
buffer_reject(grow_live_read [=[i32 main(){mut Vector<byte> owner=success(bytes.allocate(2));Slice<byte> read=Slice.from_vector(&owner);bytes.push(&mut owner,Byte.from_u8(1u8));return Byte.to_u8(read[0])==0u8?0:1;}]=] "borrow|loan|reference")
buffer_reject(copy_conflicting_alias [=[i32 main(){mut byte values[2]=[Byte.from_u8(1u8),Byte.from_u8(2u8)];Slice<byte> source=Slice.from_array(&values);bytes.copy_from(Slice.from_array_mut(&mut values),source);return 0;}]=] "borrow|loan|reference")
buffer_reject(append_same_owner [=[i32 main(){mut Vector<byte> owner=success(bytes.allocate(2));bytes.append(&mut owner,&mut owner);return 0;}]=] "borrow|loan|reference")
buffer_reject(slice_local_escape [=[Result<Slice<byte>,bytes.ByteError> escape(){byte values[1]=[Byte.from_u8(1u8)];return bytes.slice(Slice.from_array(&values),Range.new(0usize,1usize));}i32 main(){return 0;}]=] "borrow|loan|reference|local")
buffer_reject(slice_mut_local_escape [=[Result<MutSlice<byte>,bytes.ByteError> escape(){mut byte values[1]=[Byte.from_u8(1u8)];return bytes.slice_mut(Slice.from_array_mut(&mut values),Range.new(0usize,1usize));}i32 main(){return 0;}]=] "borrow|loan|reference|local")
buffer_reject(carried_mut_view [=[i32 main(){mut byte values[2]=[Byte.from_u8(1u8),Byte.from_u8(2u8)];Result<MutSlice<byte>,bytes.ByteError> view=bytes.slice_mut(Slice.from_array_mut(&mut values),Range.new(0usize,1usize));values[0]=Byte.from_u8(3u8);mut MutSlice<byte> child=success(view);return Byte.to_u8(child[0])==1u8?0:1;}]=] "borrow|loan|reference")
buffer_reject(byte_distinct_value [=[i32 main(){mut Vector<byte> owner=success(bytes.allocate(0));bytes.push(&mut owner,1u8);return 0;}]=] "match|requires|received|compatible")

if(DEFINED CLANG AND EXISTS "${CLANG}" AND DEFINED RUNTIME AND EXISTS "${RUNTIME}")
    file(WRITE "${TEST_DIR}/buffer_failures.avx" "${header}\n" [=[
extern "C" void buffer_fault_point(u32 attempt);
extern "C" u32 buffer_outstanding();
extern "C" u32 buffer_attempts();
bool allocation<T>(Result<T,bytes.ByteError> input) {
    switch (move input) {
        case Result<T,bytes.ByteError>.Err { bytes.ByteError reason = error }:
            switch (reason) { case bytes.ByteError.AllocationFailure: return true; break; default: return false; break; }
            break;
        default: return false; break;
    }
}
bool sized<T>(Result<T,bytes.ByteError> input) {
    switch (move input) {
        case Result<T,bytes.ByteError>.Err { bytes.ByteError reason = error }:
            switch (reason) { case bytes.ByteError.SizeOverflow: return true; break; default: return false; break; }
            break;
        default: return false; break;
    }
}
i32 failed_mutation(u32 operation) {
    buffer_fault_point(0);
    byte original[4] = [Byte.from_u8(1u8),Byte.from_u8(2u8),Byte.from_u8(3u8),Byte.from_u8(4u8)];
    byte incoming[3] = [Byte.from_u8(9u8),Byte.from_u8(10u8),Byte.from_u8(11u8)];
    mut Vector<byte> destination=success(bytes.from_slice(Slice.from_array(&original)));
    mut Vector<byte> source=success(bytes.from_slice(Slice.from_array(&incoming)));
    usize capacity=bytes.capacity(&destination);
    usize source_capacity=bytes.capacity(&source);
    success(bytes.resize(&mut destination,capacity,Byte.from_u8(7u8)));
    bytes.fill(Slice.from_vector_mut(&mut destination),Byte.from_u8(7u8));
    u32 before=buffer_outstanding();
    buffer_fault_point(1);
    mut bool refused=false;
    if(operation==0u32) refused=allocation(bytes.push(&mut destination,Byte.from_u8(8u8)));
    else if(operation==1u32) refused=allocation(bytes.reserve(&mut destination,capacity+1));
    else if(operation==2u32) refused=allocation(bytes.insert(&mut destination,1,Byte.from_u8(8u8)));
    else if(operation==3u32) refused=allocation(bytes.extend(&mut destination,Slice.from_array(&incoming)));
    else if(operation==4u32) refused=allocation(bytes.resize(&mut destination,capacity+2,Byte.from_u8(8u8)));
    else if(operation==5u32) refused=allocation(bytes.append(&mut destination,&mut source));
    else refused=allocation(bytes.split_off(&mut destination,1));
    if(!refused || buffer_attempts()!=1 || buffer_outstanding()!=before) return 1;
    if(Vector.len(&destination)!=capacity || bytes.capacity(&destination)!=capacity || Vector.len(&source)!=3 || bytes.capacity(&source)!=source_capacity) return 2;
    mut usize index=0;
    while(index<capacity){if(Byte.to_u8(destination[index])!=7u8)return 3;index++;}
    if(Byte.to_u8(source[0])!=9u8 || Byte.to_u8(source[2])!=11u8)return 4;
    buffer_fault_point(1);
    if(!sized(bytes.reserve(&mut destination,18446744073709551615usize)) || buffer_attempts()!=0 || Vector.len(&destination)!=capacity || bytes.capacity(&destination)!=capacity)return 5;
    Result<unit,bytes.ByteError> invalid_insert=bytes.insert(&mut destination,capacity+1,Byte.from_u8(8u8));
    Result<byte,bytes.ByteError> invalid_remove=bytes.remove(&mut destination,capacity);
    Result<unit,bytes.ByteError> invalid_truncate=bytes.truncate(&mut destination,capacity+1);
    Result<Vector<byte>,bytes.ByteError> invalid_split=bytes.split_off(&mut destination,capacity+1);
    if(!Result.is_err(&invalid_insert) || !Result.is_err(&invalid_remove) || !Result.is_err(&invalid_truncate) || !Result.is_err(&invalid_split) || buffer_attempts()!=0)return 6;
    if(Vector.len(&destination)!=capacity || bytes.capacity(&destination)!=capacity)return 7;
    return 0;
}
i32 successful_mutation() {
    buffer_fault_point(0);
    mut Vector<byte> first=success(bytes.allocate(4));
    mut Vector<byte> second=success(bytes.allocate(3));
    bytes.fill(Slice.from_vector_mut(&mut first),Byte.from_u8(7u8));
    bytes.fill(Slice.from_vector_mut(&mut second),Byte.from_u8(9u8));
    success(bytes.append(&mut first,&mut second));
    Vector<byte> tail=success(bytes.split_off(&mut first,2));
    if(Vector.len(&first)!=2 || Vector.len(&second)!=0 || Vector.len(&tail)!=5 || Byte.to_u8(tail[0])!=7u8 || Byte.to_u8(tail[4])!=9u8)return 1;
    return 0;
}
i32 main() {
    byte input[2]=[Byte.from_u8(1u8),Byte.from_u8(2u8)];
    buffer_fault_point(1);
    if(!allocation(bytes.allocate(3)) || buffer_attempts()!=1 || buffer_outstanding()!=0)return 1;
    buffer_fault_point(1);
    if(!allocation(bytes.with_capacity(3)) || buffer_attempts()!=1 || buffer_outstanding()!=0)return 2;
    buffer_fault_point(1);
    if(!allocation(bytes.from_slice(Slice.from_array(&input))) || buffer_attempts()!=1 || buffer_outstanding()!=0 || Byte.to_u8(input[0])!=1u8)return 3;
    mut u32 operation=0;
    while(operation<7u32){if(failed_mutation(operation)!=0 || buffer_outstanding()!=0)return 4;operation++;}
    if(successful_mutation()!=0 || buffer_outstanding()!=0)return 5;
    buffer_fault_point(1);
    {
        Vector<byte> empty=success(bytes.allocate(0));
        Vector<byte> capacity_zero=success(bytes.with_capacity(0));
        byte nothing[0]=[];
        Vector<byte> copied=success(bytes.from_slice(Slice.from_array(&nothing)));
        if(Vector.len(&empty)!=0 || Vector.len(&capacity_zero)!=0 || Vector.len(&copied)!=0)return 6;
    }
    if(buffer_attempts()!=0 || buffer_outstanding()!=0)return 7;
    return 0;
}
]=])
    buffer_run("buffer fault source check" 0 "" --check "${TEST_DIR}/buffer_failures.avx")
    execute_process(COMMAND "${AEVIXC}" --emit-llvm "${TEST_DIR}/buffer_failures.avx"
        RESULT_VARIABLE emitted OUTPUT_VARIABLE ir ERROR_VARIABLE error TIMEOUT 120)
    if(NOT "${emitted}" STREQUAL "0")
        message(FATAL_ERROR "Buffer failure IR emission failed: ${error}")
    endif()
    # 실제로 검사된 소스의 할당 심벌만 바꾸어 실패를 주입한다.
    # Instrument only allocation symbols in the actually checked source IR.
    string(REPLACE "@malloc(" "@buffer_test_allocate(" ir "${ir}")
    string(REPLACE "@free(" "@buffer_test_free(" ir "${ir}")
    file(WRITE "${TEST_DIR}/buffer_failures.ll" "${ir}")
    file(WRITE "${TEST_DIR}/buffer_failures.c" [=[
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
static uint32_t fail_at, calls, outstanding;
void buffer_fault_point(uint32_t point) { fail_at=point; calls=0; }
uint32_t buffer_outstanding(void) { return outstanding; }
uint32_t buffer_attempts(void) { return calls; }
void *buffer_test_allocate(size_t size) {
    if (++calls==fail_at) return NULL;
    void *value=malloc(size); if(value) outstanding++; return value;
}
void buffer_test_free(void *value) { if(value) outstanding--; free(value); }
]=])
    set(extra_libraries -pthread)
    if(APPLE)
        list(APPEND extra_libraries -lc++)
    elseif(UNIX)
        list(APPEND extra_libraries -lstdc++ -lm)
    endif()
    foreach(optimization -O0 -O2)
        set(binary "${TEST_DIR}/buffer_failures${optimization}")
        if(WIN32)
            string(APPEND binary ".exe")
        endif()
        execute_process(COMMAND "${CLANG}" "${optimization}" "${TEST_DIR}/buffer_failures.ll" "${TEST_DIR}/buffer_failures.c"
            "${RUNTIME}" ${extra_libraries} -o "${binary}" RESULT_VARIABLE linked OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
        if(NOT "${linked}" STREQUAL "0")
            message(FATAL_ERROR "Buffer fault fixture link failed: ${output}${error}")
        endif()
        execute_process(COMMAND "${binary}" RESULT_VARIABLE ran OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
        if(NOT "${ran}" STREQUAL "0")
            message(FATAL_ERROR "Buffer allocation preservation failed (${ran}): ${output}${error}")
        endif()
    endforeach()
else()
    message(STATUS "Buffer deterministic allocation gate requires CLANG and RUNTIME")
endif()
message(STATUS "Bytes owning-buffer gates passed; lazy iterators, native endian and UTF wrappers remain separate.")
