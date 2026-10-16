# ========================
#
# Created By : miruku_sora2026
#
# File name : CMakeLists.txt
#
# Description :
#     컴파일러·런타임·검사 대상과 선택적 예제 설치를 구성한다.
#     Configure compiler, runtime, test targets, and optional example installation.
#
# ========================

cmake_minimum_required(VERSION 3.20)
project(Aevix VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
include(GNUInstallDirs)
find_package(Threads REQUIRED)
option(AEVIX_ENABLE_JIT "Enable LLVM ORC JIT when LLVM 22 or newer is available" ON)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Aevix targets require 64-bit pointers")
endif()
if(MSVC)
    add_compile_options(/utf-8)
endif()

# 프로세스 실행은 프런트엔드와 LLVM 런타임에서 독립된 운영체제 지원 계층이다.
# Process execution is an OS support layer independent of the frontend and LLVM runtime.
add_library(aevix_support src/Support/Process.cpp)
target_include_directories(aevix_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(aevix_support PUBLIC cxx_std_20)
target_link_libraries(aevix_support PUBLIC Threads::Threads)

# 프런트엔드는 LLVM 없이 독립적으로 빌드한다.
# Build the frontend independently without LLVM.
add_library(aevix_frontend
        src/Frontend/Lexer/Lexer.cpp
        src/Frontend/Parser/Parser.cpp
        src/Frontend/Modules/ModuleLoader.cpp
        src/Frontend/Sema/NameResolver.cpp
        src/Frontend/Sema/Type.cpp
        src/Frontend/Sema/TypeChecker.cpp
        src/Frontend/Sema/ControlFlowChecker.cpp
        src/Frontend/Sema/ConstEvaluator.cpp
        src/Frontend/Sema/TypeLayout.cpp
        src/Frontend/Sema/GenericResolver.cpp
        src/Frontend/Sema/CoreErrorTypes.cpp
        src/Frontend/Sema/ForeignValidator.cpp
        src/Frontend/Ownership/BorrowChecker.cpp
        src/IR/AIR/Module.cpp
        src/IR/Lowering/ASTLowering.cpp
        src/Backend/LLVM/LLVMCodeGen.cpp
        src/Backend/AOT/AOTCompiler.cpp
)

target_include_directories(aevix_frontend PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(aevix_frontend PUBLIC cxx_std_20)
target_link_libraries(aevix_frontend PUBLIC aevix_support)
set_target_properties(aevix_frontend PROPERTIES CXX_EXTENSIONS OFF)

add_library(aevix_runtime STATIC runtime/Initialization/PersistRuntime.cpp)
target_include_directories(aevix_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(aevix_runtime PUBLIC cxx_std_20)
target_link_libraries(aevix_runtime PUBLIC Threads::Threads)

# 생성 프로그램에는 컴파일러의 sanitizer 설정을 전파하지 않는 별도 런타임을 연결한다.
# Link generated programs to a separate runtime without propagating compiler sanitizers.
add_library(aevix_target_runtime STATIC runtime/Initialization/PersistRuntime.cpp)
target_include_directories(aevix_target_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(aevix_target_runtime PRIVATE cxx_std_20)
set_target_properties(aevix_target_runtime PROPERTIES OUTPUT_NAME aevix_runtime
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime-payload" POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(aevix_target_runtime PRIVATE -fno-sanitize=all -fno-exceptions -fno-rtti)
endif()

add_library(aevix_jit STATIC src/JIT/ORC/ORCJIT.cpp)
target_include_directories(aevix_jit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(aevix_jit PUBLIC cxx_std_20)
target_link_libraries(aevix_jit PRIVATE aevix_runtime)
if(AEVIX_ENABLE_JIT)
    find_package(LLVM CONFIG QUIET HINTS /opt/homebrew/opt/llvm/lib/cmake/llvm)
endif()
if(AEVIX_ENABLE_JIT AND LLVM_FOUND AND LLVM_VERSION_MAJOR GREATER_EQUAL 22)
    target_compile_definitions(aevix_jit PRIVATE AEVIX_HAS_LLVM_ORC=1)
    target_include_directories(aevix_jit SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    if(TARGET LLVM)
        target_link_libraries(aevix_jit PRIVATE LLVM)
    else()
        llvm_map_components_to_libnames(AEVIX_LLVM_LIBS core irreader orcjit native passes)
        target_link_libraries(aevix_jit PRIVATE ${AEVIX_LLVM_LIBS})
    endif()
    message(STATUS "Aevix ORC JIT: LLVM ${LLVM_PACKAGE_VERSION}")
else()
    message(STATUS "Aevix ORC JIT unavailable: install LLVM >= 22 or set LLVM_DIR")
endif()

add_executable(aevixc tools/aevixc/main.cpp)
target_link_libraries(aevixc PRIVATE aevix_frontend aevix_jit aevix_runtime)
add_dependencies(aevixc aevix_target_runtime)
target_compile_definitions(aevixc PRIVATE AEVIX_DEFAULT_RUNTIME="$<TARGET_FILE:aevix_target_runtime>"
    AEVIX_RUNTIME_FILENAME="$<TARGET_FILE_NAME:aevix_target_runtime>"
    AEVIX_VERSION="${PROJECT_VERSION}" AEVIX_INSTALL_RUNTIME_DIR="${CMAKE_INSTALL_LIBDIR}/aevix")
set_target_properties(aevixc PROPERTIES CXX_EXTENSIONS OFF)
set_target_properties(aevixc PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)
if(MINGW)
    target_link_options(aevixc PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

install(TARGETS aevixc RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "$<TARGET_FILE:aevix_target_runtime>" DESTINATION "${CMAKE_INSTALL_LIBDIR}/aevix")
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/examples")
    install(DIRECTORY examples/ DESTINATION "${CMAKE_INSTALL_DATADIR}/aevix/examples" FILES_MATCHING PATTERN "*.avx")
endif()
set(CPACK_PACKAGE_NAME Aevix)
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_GENERATOR TGZ)
if(WIN32)
    set(CPACK_GENERATOR ZIP)
endif()
set(CPACK_PACKAGE_FILE_NAME "aevix-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
include(CPack)

find_program(AEVIX_CLANG_EXECUTABLE NAMES clang
    HINTS /opt/homebrew/opt/llvm/bin /usr/lib/llvm-22/bin /usr/lib/llvm-21/bin "C:/Program Files/LLVM/bin")
if(AEVIX_CLANG_EXECUTABLE)
    target_compile_definitions(aevixc PRIVATE AEVIX_DEFAULT_CLANG="${AEVIX_CLANG_EXECUTABLE}")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(aevix_frontend PRIVATE
            -Wall -Wextra -Wpedantic
    )
    target_compile_options(aevixc PRIVATE
            -Wall -Wextra -Wpedantic
    )
endif()

include(CTest)

if(BUILD_TESTING)
    add_executable(aevix_frontend_tests tests/frontend_tests.cpp)
    target_link_libraries(aevix_frontend_tests PRIVATE aevix_frontend)
    add_test(NAME frontend COMMAND aevix_frontend_tests)

    add_executable(aevix_frontend_fuzz tests/fuzz_frontend.cpp)
    target_link_libraries(aevix_frontend_fuzz PRIVATE aevix_frontend)
    add_test(NAME frontend_random COMMAND aevix_frontend_fuzz)

    add_test(NAME frontend_cli
        COMMAND "${CMAKE_COMMAND}"
            "-DAEVIXC=$<TARGET_FILE:aevixc>"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-fixtures"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_tests.cmake"
    )
    set_tests_properties(frontend frontend_random frontend_cli PROPERTIES TIMEOUT 30)
    add_executable(aevix_name_resolution_tests tests/name_resolution_tests.cpp)
    target_link_libraries(aevix_name_resolution_tests PRIVATE aevix_frontend)
    add_test(NAME name_resolution COMMAND aevix_name_resolution_tests)
    add_test(NAME module_cli
        COMMAND "${CMAKE_COMMAND}"
            "-DAEVIXC=$<TARGET_FILE:aevixc>"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/module-cli-fixtures"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/module_cli_tests.cmake"
    )
    set_tests_properties(name_resolution module_cli PROPERTIES TIMEOUT 60)

    add_executable(aevix_type_checker_tests tests/type_checker_tests.cpp)
    target_link_libraries(aevix_type_checker_tests PRIVATE aevix_frontend)
    add_test(NAME type_checker COMMAND aevix_type_checker_tests)
    add_test(NAME type_cli
        COMMAND "${CMAKE_COMMAND}"
            "-DAEVIXC=$<TARGET_FILE:aevixc>"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/type-cli-fixtures"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/type_cli_tests.cmake"
    )
    set_tests_properties(type_checker type_cli PROPERTIES TIMEOUT 60)

    add_executable(aevix_persist_runtime_tests tests/persist_runtime_tests.cpp)
    target_link_libraries(aevix_persist_runtime_tests PRIVATE aevix_runtime aevix_support)
    add_test(NAME persist_runtime COMMAND aevix_persist_runtime_tests)
    set_tests_properties(persist_runtime PROPERTIES TIMEOUT 60)

    add_executable(aevix_persist_language_tests tests/persist_language_tests.cpp)
    target_link_libraries(aevix_persist_language_tests PRIVATE aevix_frontend aevix_jit)
    target_compile_definitions(aevix_persist_language_tests PRIVATE AEVIX_TEST_RUNTIME_ARCHIVE="$<TARGET_FILE:aevix_target_runtime>")
    add_test(NAME persist_language COMMAND aevix_persist_language_tests "${AEVIX_CLANG_EXECUTABLE}")
    set_tests_properties(persist_language PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)

    add_executable(aevix_jit_tests tests/jit_tests.cpp)
    target_link_libraries(aevix_jit_tests PRIVATE aevix_jit aevix_support)
    add_test(NAME jit COMMAND aevix_jit_tests "${AEVIX_CLANG_EXECUTABLE}")
    set_tests_properties(jit PROPERTIES TIMEOUT 120)

    add_test(NAME range_api COMMAND "${CMAKE_COMMAND}" -DCOMPILER=$<TARGET_FILE:aevixc> -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/range-api-cases -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/range_api_tests.cmake")
    set_tests_properties(range_api PROPERTIES TIMEOUT 180)

    add_executable(aevix_dynamic_array_tests tests/dynamic_array_tests.cpp)
    target_link_libraries(aevix_dynamic_array_tests PRIVATE aevix_frontend aevix_jit)
    target_compile_definitions(aevix_dynamic_array_tests PRIVATE AEVIX_TEST_RUNTIME_ARCHIVE="$<TARGET_FILE:aevix_target_runtime>")
    add_test(NAME dynamic_arrays COMMAND aevix_dynamic_array_tests "${AEVIX_CLANG_EXECUTABLE}")
    set_tests_properties(dynamic_arrays PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)

    add_executable(aevix_error_handling_tests tests/error_handling_tests.cpp)
    target_link_libraries(aevix_error_handling_tests PRIVATE aevix_frontend)
    target_compile_definitions(aevix_error_handling_tests PRIVATE AEVIX_TEST_RUNTIME_ARCHIVE="$<TARGET_FILE:aevix_target_runtime>")
    add_test(NAME error_handling COMMAND aevix_error_handling_tests "${AEVIX_CLANG_EXECUTABLE}")
    set_tests_properties(error_handling PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)

    add_executable(aevix_constant_layout_tests tests/constant_layout_tests.cpp)
    target_link_libraries(aevix_constant_layout_tests PRIVATE aevix_frontend)
    add_test(NAME constant_layout COMMAND aevix_constant_layout_tests)
    set_tests_properties(constant_layout PROPERTIES TIMEOUT 60)

    add_executable(aevix_ownership_tests tests/ownership_tests.cpp)
    target_link_libraries(aevix_ownership_tests PRIVATE aevix_frontend)
    add_test(NAME ownership COMMAND aevix_ownership_tests)
    set_tests_properties(ownership PROPERTIES TIMEOUT 60)

    add_executable(aevix_ir_aot_tests tests/ir_aot_tests.cpp)
    target_link_libraries(aevix_ir_aot_tests PRIVATE aevix_frontend)
    target_compile_definitions(aevix_ir_aot_tests PRIVATE AEVIX_TEST_RUNTIME_ARCHIVE="$<TARGET_FILE:aevix_target_runtime>")
    add_test(NAME ir_aot COMMAND aevix_ir_aot_tests "${AEVIX_CLANG_EXECUTABLE}")
    set_tests_properties(ir_aot PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)

    add_executable(aevix_dynamic_interface_tests tests/dynamic_interface_tests.cpp)
    target_link_libraries(aevix_dynamic_interface_tests PRIVATE aevix_frontend aevix_jit)
    target_compile_definitions(aevix_dynamic_interface_tests PRIVATE AEVIX_TEST_RUNTIME_ARCHIVE="$<TARGET_FILE:aevix_target_runtime>")
    add_test(NAME dynamic_interfaces COMMAND aevix_dynamic_interface_tests "${AEVIX_CLANG_EXECUTABLE}")
    set_tests_properties(dynamic_interfaces PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    add_executable(aevix_generic_tests tests/generic_tests.cpp)
    target_link_libraries(aevix_generic_tests PRIVATE aevix_frontend)
    add_test(NAME generics_interfaces COMMAND aevix_generic_tests)
    set_tests_properties(generics_interfaces PROPERTIES TIMEOUT 90)
    add_test(NAME enums
        COMMAND "${CMAKE_COMMAND}"
            "-DAEVIXC=$<TARGET_FILE:aevixc>"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/enum-cli-fixtures"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/enum_cli_tests.cmake")
    set_tests_properties(enums PROPERTIES TIMEOUT 60)
    add_test(NAME range_variadic
        COMMAND "${CMAKE_COMMAND}"
            "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/range-variadic-fixtures"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/range_variadic_tests.cmake")
    set_tests_properties(range_variadic PROPERTIES TIMEOUT 180)
    add_executable(aevix_process_tests tests/process_tests.cpp)
    target_link_libraries(aevix_process_tests PRIVATE aevix_support)
    if(MINGW)
        target_link_options(aevix_process_tests PRIVATE -municode)
    endif()
    add_test(NAME process COMMAND aevix_process_tests)
    set_tests_properties(process PROPERTIES TIMEOUT 30)
    if(AEVIX_CLANG_EXECUTABLE)
        add_test(NAME compiler_cli
            COMMAND "${CMAKE_COMMAND}"
                "-DAEVIXC=$<TARGET_FILE:aevixc>"
                "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
                "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/compiler-cli-fixtures"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_cli_tests.cmake")
        set_tests_properties(compiler_cli PROPERTIES TIMEOUT 120)
        add_test(NAME runtime_cli
            COMMAND "${CMAKE_COMMAND}"
                "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
                "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/runtime-cli-fixtures"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_cli_tests.cmake")
        add_test(NAME installed_package
            COMMAND "${CMAKE_COMMAND}"
                "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
                "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/installed-fixtures"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_tests.cmake")
        set_tests_properties(runtime_cli installed_package PROPERTIES TIMEOUT 180)
    endif()
endif()
