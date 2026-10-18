# ========================
#
# Created By : miruku_sora2026
#
# File name : native_boundary_cli_tests.cmake
#
# Description :
#     사용자 extern과 유사 이름·모듈이 네이티브 경계의 권한을 얻지 못하는지 검사한다.
#     Check that source extern declarations and lookalike names or modules cannot obtain native privileges.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
function(check_boundary name expected pattern text)
    file(WRITE "${TEST_DIR}/${name}.avx" "${text}\n")
    execute_process(COMMAND "${AEVIXC}" --check "${TEST_DIR}/${name}.avx"
        RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
    if(NOT "${code}" STREQUAL "${expected}" OR NOT "${output}${error}" MATCHES "${pattern}")
        message(FATAL_ERROR "${name}: expected ${expected}/${pattern}, got ${code}\n${output}${error}")
    endif()
endfunction()

foreach(operation IN ITEMS release read_bytes write_bytes)
    check_boundary("reserved_${operation}" 1 "compiler-managed"
        "extern \"C\" i32 aevix_native_${operation}(i32 token); i32 main() { return 0; }")
endforeach()
check_boundary(unknown_reserved 1 "reserved external"
    "extern \"C\" i32 aevix_native_user_operation(i32 token); i32 main() { return 0; }")
check_boundary(borrowed_buffer 1 "C FFI: parameters"
    "extern \"C\" i32 native_read(MutSlice<byte> output); i32 main() { return 0; }")
check_boundary(shared_buffer 1 "C FFI: parameters"
    "extern \"C\" i32 native_write(Slice<byte> input); i32 main() { return 0; }")
check_boundary(borrowed_reference 1 "C FFI: parameters"
    "extern \"C\" i32 native_get(&i32 value); i32 main() { return 0; }")
check_boundary(forged_owner 1 "C FFI: parameters"
    "struct NativeOwner { public u64 token; }; extern \"C\" i32 native_close(NativeOwner owner); i32 main() { return 0; }")
check_boundary(returned_borrow 1 "C FFI: return type"
    "extern \"C\" &i32 native_view(); i32 main() { return 0; }")
check_boundary(source_attribute 1 "bench|attribute|annotation|Expected"
    "@native i32 privileged() { return 0; } i32 main() { return privileged(); }")
check_boundary(ordinary_scalar_ffi 0 "Type checking succeeded"
    "extern \"C\" i32 native_scalar(i32 number); i32 main() { return 0; }")

# 모듈의 이름이나 CompilerNative 같은 타입 이름도 승인된 내부 바인딩이 아니다.
# A module path or a type named CompilerNative is not an approved internal binding.
file(WRITE "${TEST_DIR}/native.avx"
    "public struct CompilerNative { public u64 token; }; public extern \"C\" i32 native_read_buffer(MutSlice<byte> output);\n")
check_boundary(module_lookalike 1 "C FFI: parameters"
    "import native { CompilerNative }; i32 main() { CompilerNative owner = CompilerNative { token = 0 }; return 0; }")
message(STATUS "Native boundary source restrictions passed")
