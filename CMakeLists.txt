# ========================
#
# Created By : miruku_sora2026
#
# File name : CMakeLists.txt
#
# Description :
#     컴파일러·런타임·검사 대상과 공개 배포물 설치를 구성한다.
#     Configure compiler, runtime, test targets, and installation of public distribution files.
#
# ========================

cmake_minimum_required(VERSION 3.20)
project(Aevix VERSION 0.3.0 LANGUAGES C CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
include(GNUInstallDirs)
find_package(Threads REQUIRED)
option(AEVIX_ENABLE_JIT "Enable LLVM ORC JIT when LLVM 22 or newer is available" ON)
option(AEVIX_PORTABLE_PACKAGE "Avoid machine-specific fallback paths in distributable compilers" OFF)
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
        src/Frontend/Sema/NativeContract.cpp
        src/Frontend/Ownership/BorrowChecker.cpp
        src/Frontend/Ownership/ThreadSafetyAnalysis.cpp
        src/IR/AIR/Module.cpp
        src/IR/Lowering/ASTLowering.cpp
        src/Optimizer/ProofRangeAnalysis.cpp
        src/Backend/LLVM/LLVMCodeGen.cpp
        src/Backend/LLVM/SimdCodeGen.cpp
        src/Backend/AOT/AOTCompiler.cpp
)

target_include_directories(aevix_frontend PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(aevix_frontend PUBLIC cxx_std_20)
target_compile_definitions(aevix_frontend PRIVATE
    AEVIX_INSTALL_LANGUAGE_DIR="${CMAKE_INSTALL_DATADIR}/aevix/library")
target_link_libraries(aevix_frontend PUBLIC aevix_support)
set_target_properties(aevix_frontend PROPERTIES CXX_EXTENSIONS OFF)

add_library(aevix_runtime STATIC runtime/Initialization/PersistRuntime.cpp runtime/Text/TextRuntime.cpp runtime/Profiling/BenchRuntime.cpp runtime/Threading/ParallelRuntime.cpp runtime/Numeric/Integer128Runtime.cpp runtime/Numeric/ComplexRuntime.cpp runtime/Native/NativeBoundary.cpp runtime/System/SystemRuntime.cpp)
target_include_directories(aevix_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(aevix_runtime PUBLIC cxx_std_20)
target_link_libraries(aevix_runtime PUBLIC Threads::Threads)
target_sources(aevix_runtime PRIVATE runtime/Text/ParseRuntime.cpp)
target_sources(aevix_runtime PRIVATE runtime/System/OsRuntime.cpp)
target_sources(aevix_runtime PRIVATE runtime/Memory/SharedRuntime.cpp)
target_sources(aevix_runtime PRIVATE runtime/Threading/AtomicRuntime.cpp)
target_sources(aevix_runtime PRIVATE runtime/Threading/LockRuntime.cpp)
target_sources(aevix_runtime PRIVATE runtime/Threading/TaskRuntime.cpp)
if(UNIX AND NOT APPLE)
    target_link_libraries(aevix_runtime PUBLIC m)
endif()

# 생성 프로그램에는 컴파일러의 sanitizer 설정을 전파하지 않는 별도 런타임을 연결한다.
# Link generated programs to a separate runtime without propagating compiler sanitizers.
add_library(aevix_target_runtime STATIC runtime/Initialization/PersistRuntime.cpp runtime/Text/TextRuntime.cpp runtime/Profiling/BenchRuntime.cpp runtime/Threading/ParallelRuntime.cpp runtime/Numeric/Integer128Runtime.cpp runtime/Numeric/ComplexRuntime.cpp runtime/Native/NativeBoundary.cpp runtime/System/SystemRuntime.cpp)
target_include_directories(aevix_target_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(aevix_target_runtime PRIVATE cxx_std_20)
target_sources(aevix_target_runtime PRIVATE runtime/Text/ParseRuntime.cpp)
target_sources(aevix_target_runtime PRIVATE runtime/System/OsRuntime.cpp)
target_sources(aevix_target_runtime PRIVATE runtime/Memory/SharedRuntime.cpp)
target_sources(aevix_target_runtime PRIVATE runtime/Threading/AtomicRuntime.cpp)
target_sources(aevix_target_runtime PRIVATE runtime/Threading/LockRuntime.cpp)
target_sources(aevix_target_runtime PRIVATE runtime/Threading/TaskRuntime.cpp)
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
target_compile_definitions(aevixc PRIVATE
    AEVIX_RUNTIME_FILENAME="$<TARGET_FILE_NAME:aevix_target_runtime>"
    AEVIX_VERSION="${PROJECT_VERSION}" AEVIX_INSTALL_RUNTIME_DIR="${CMAKE_INSTALL_LIBDIR}/aevix")
if(NOT AEVIX_PORTABLE_PACKAGE)
    target_compile_definitions(aevixc PRIVATE AEVIX_DEFAULT_RUNTIME="$<TARGET_FILE:aevix_target_runtime>")
endif()
set_target_properties(aevixc PROPERTIES CXX_EXTENSIONS OFF)
set_target_properties(aevixc PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)
if(MINGW)
    target_link_options(aevixc PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

install(TARGETS aevixc RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "$<TARGET_FILE:aevix_target_runtime>" DESTINATION "${CMAKE_INSTALL_LIBDIR}/aevix")
install(FILES LICENSE CONTRIBUTING.md DESTINATION "${CMAKE_INSTALL_DATADIR}/aevix")
install(FILES design/logo/aevix-logo.png DESTINATION "${CMAKE_INSTALL_DATADIR}/aevix")
# 유니코드 데이터의 전체 라이선스를 모든 지원 대상의 설치물에 포함한다.
# Include the complete Unicode data license in installations for every supported target.
install(FILES third_party/unicode/17.0.0/LICENSE-UNICODE.txt third_party/unicode/17.0.0/NOTICE.txt
    DESTINATION "${CMAKE_INSTALL_DATADIR}/aevix/licenses")
if(MINGW)
    # 정적 MinGW 배포물에도 도구체인의 저작권 고지와 런타임 예외를 함께 보관한다.
    # Keep toolchain notices and the runtime exception in static MinGW distributions.
    install(DIRECTORY packaging/licenses/ DESTINATION "${CMAKE_INSTALL_DATADIR}/aevix/licenses"
        FILES_MATCHING PATTERN "*.txt")
endif()
# 명시적으로 가져오는 언어 모듈을 실행 파일 기준 경로에도 함께 배치한다.
# Stage explicitly imported language modules beside the executable and in the install prefix.
file(GLOB_RECURSE AEVIX_LANGUAGE_MODULES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/library/*.avx")
foreach(module IN LISTS AEVIX_LANGUAGE_MODULES)
    file(RELATIVE_PATH relative_module "${CMAKE_CURRENT_SOURCE_DIR}/library" "${module}")
    get_filename_component(module_directory "${relative_module}" DIRECTORY)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/share/aevix/library/${module_directory}")
    configure_file("${module}" "share/aevix/library/${relative_module}" COPYONLY)
    install(FILES "${module}" DESTINATION "${CMAKE_INSTALL_DATADIR}/aevix/library/${module_directory}")
endforeach()
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
if(AEVIX_CLANG_EXECUTABLE AND NOT AEVIX_PORTABLE_PACKAGE)
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
    add_executable(aevix_proof_range_tests tests/proof_range_tests.cpp)
    target_link_libraries(aevix_proof_range_tests PRIVATE aevix_frontend)
    add_test(NAME proof_ranges COMMAND aevix_proof_range_tests)
    add_test(NAME proof_optimization_cli COMMAND "${CMAKE_COMMAND}"
        -DAEVIXC=$<TARGET_FILE:aevixc>
        -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/proof-optimization-cli
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/proof_optimization_cli_tests.cmake")
    set_tests_properties(proof_optimization_cli PROPERTIES TIMEOUT 600)
    add_executable(aevix_v03_box_extract_air_tests tests/v03_box_extract_air_tests.cpp)
    target_link_libraries(aevix_v03_box_extract_air_tests PRIVATE aevix_frontend)
    add_test(NAME v03_box_extract_air COMMAND aevix_v03_box_extract_air_tests)
    add_executable(aevix_v03_box_extract_jit_tests tests/v03_box_extract_fault_harness.cpp)
    target_compile_definitions(aevix_v03_box_extract_jit_tests PRIVATE AEVIX_BOX_JIT_TEST)
    target_link_libraries(aevix_v03_box_extract_jit_tests PRIVATE aevix_jit aevix_support)
    add_test(NAME v03_box_extract_cli COMMAND "${CMAKE_COMMAND}"
        -DAEVIXC=$<TARGET_FILE:aevixc> -DCLANG=${AEVIX_CLANG_EXECUTABLE}
        -DRUNTIME=$<TARGET_FILE:aevix_target_runtime>
        -DJIT_PROBE=$<TARGET_FILE:aevix_v03_box_extract_jit_tests>
        -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-box-extract-cli
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_box_extract_cli_tests.cmake")
    set_tests_properties(v03_box_extract_cli PROPERTIES TIMEOUT 300)
    add_executable(aevix_v03_cancellation_runtime_tests tests/v03_cancellation_runtime_tests.cpp
        runtime/Threading/TaskRuntime.cpp runtime/Native/NativeBoundary.cpp)
    target_include_directories(aevix_v03_cancellation_runtime_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(aevix_v03_cancellation_runtime_tests PRIVATE cxx_std_20)
    target_compile_definitions(aevix_v03_cancellation_runtime_tests PRIVATE AEVIX_TASK_RUNTIME_TESTING)
    target_compile_options(aevix_v03_cancellation_runtime_tests PRIVATE
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fno-exceptions;-fno-rtti>")
    target_link_libraries(aevix_v03_cancellation_runtime_tests PRIVATE Threads::Threads)
    add_test(NAME v03_cancellation_runtime COMMAND aevix_v03_cancellation_runtime_tests)
    set_tests_properties(v03_cancellation_runtime PROPERTIES TIMEOUT 90)
    add_executable(aevix_v03_cancellation_air_tests tests/v03_cancellation_air_tests.cpp)
    target_link_libraries(aevix_v03_cancellation_air_tests PRIVATE aevix_frontend)
    add_test(NAME v03_cancellation_air COMMAND aevix_v03_cancellation_air_tests)
    add_test(NAME v03_cancellation_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-cancellation-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_cancellation_cli_tests.cmake")
    set_tests_properties(v03_cancellation_cli PROPERTIES TIMEOUT 600)
    add_executable(aevix_v03_task_scope_runtime_tests tests/v03_task_scope_runtime_tests.cpp
        runtime/Threading/TaskRuntime.cpp runtime/Native/NativeBoundary.cpp)
    target_include_directories(aevix_v03_task_scope_runtime_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(aevix_v03_task_scope_runtime_tests PRIVATE cxx_std_20)
    target_compile_definitions(aevix_v03_task_scope_runtime_tests PRIVATE AEVIX_TASK_RUNTIME_TESTING)
    target_compile_options(aevix_v03_task_scope_runtime_tests PRIVATE
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fno-exceptions;-fno-rtti>")
    target_link_libraries(aevix_v03_task_scope_runtime_tests PRIVATE Threads::Threads)
    add_test(NAME v03_task_scope_runtime COMMAND aevix_v03_task_scope_runtime_tests)
    set_tests_properties(v03_task_scope_runtime PROPERTIES TIMEOUT 90)
    add_executable(aevix_v03_task_identity_runtime_tests tests/v03_task_identity_runtime_tests.cpp
        runtime/Threading/TaskRuntime.cpp runtime/Native/NativeBoundary.cpp)
    target_include_directories(aevix_v03_task_identity_runtime_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(aevix_v03_task_identity_runtime_tests PRIVATE cxx_std_20)
    target_compile_definitions(aevix_v03_task_identity_runtime_tests PRIVATE AEVIX_TASK_RUNTIME_TESTING)
    target_compile_options(aevix_v03_task_identity_runtime_tests PRIVATE
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fno-exceptions;-fno-rtti>")
    target_link_libraries(aevix_v03_task_identity_runtime_tests PRIVATE Threads::Threads)
    add_test(NAME v03_task_identity_runtime COMMAND aevix_v03_task_identity_runtime_tests)
    set_tests_properties(v03_task_identity_runtime PROPERTIES TIMEOUT 90)
    add_executable(aevix_v03_task_identity_exhaustion_tests tests/v03_task_identity_runtime_tests.cpp
        runtime/Threading/TaskRuntime.cpp runtime/Native/NativeBoundary.cpp)
    target_include_directories(aevix_v03_task_identity_exhaustion_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(aevix_v03_task_identity_exhaustion_tests PRIVATE cxx_std_20)
    target_compile_definitions(aevix_v03_task_identity_exhaustion_tests PRIVATE AEVIX_TASK_RUNTIME_TESTING AEVIX_TASK_TEST_ID_MAX=3)
    target_compile_options(aevix_v03_task_identity_exhaustion_tests PRIVATE
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fno-exceptions;-fno-rtti>")
    target_link_libraries(aevix_v03_task_identity_exhaustion_tests PRIVATE Threads::Threads)
    add_test(NAME v03_task_identity_exhaustion COMMAND aevix_v03_task_identity_exhaustion_tests)
    set_tests_properties(v03_task_identity_exhaustion PROPERTIES TIMEOUT 90)
    add_executable(aevix_v03_task_identity_layout_tests tests/v03_task_identity_layout_tests.cpp)
    target_link_libraries(aevix_v03_task_identity_layout_tests PRIVATE aevix_frontend)
    add_test(NAME v03_task_identity_layout COMMAND aevix_v03_task_identity_layout_tests)
    add_executable(aevix_v03_task_identity_schema_tests tests/v03_task_identity_schema_tests.cpp)
    target_link_libraries(aevix_v03_task_identity_schema_tests PRIVATE aevix_frontend aevix_support)
    add_dependencies(aevix_v03_task_identity_schema_tests aevixc)
    add_test(NAME v03_task_identity_schema COMMAND aevix_v03_task_identity_schema_tests)
    set_tests_properties(v03_task_identity_schema PROPERTIES TIMEOUT 90)
    add_test(NAME v03_task_identity_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DRUNTIME=$<TARGET_FILE:aevix_target_runtime>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-task-identity-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_task_identity_cli_tests.cmake")
    set_tests_properties(v03_task_identity_cli PROPERTIES TIMEOUT 600)
    add_executable(aevix_v03_task_ready_runtime_tests tests/v03_task_ready_runtime_tests.cpp
        runtime/Threading/TaskRuntime.cpp runtime/Native/NativeBoundary.cpp)
    target_include_directories(aevix_v03_task_ready_runtime_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(aevix_v03_task_ready_runtime_tests PRIVATE cxx_std_20)
    target_compile_definitions(aevix_v03_task_ready_runtime_tests PRIVATE AEVIX_TASK_RUNTIME_TESTING)
    target_compile_options(aevix_v03_task_ready_runtime_tests PRIVATE
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fno-exceptions;-fno-rtti>")
    target_link_libraries(aevix_v03_task_ready_runtime_tests PRIVATE Threads::Threads)
    add_test(NAME v03_task_ready_runtime COMMAND aevix_v03_task_ready_runtime_tests)
    set_tests_properties(v03_task_ready_runtime PROPERTIES TIMEOUT 90)
    add_executable(aevix_v03_task_ready_exhaustion_tests tests/v03_task_ready_runtime_tests.cpp
        runtime/Threading/TaskRuntime.cpp runtime/Native/NativeBoundary.cpp)
    target_include_directories(aevix_v03_task_ready_exhaustion_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(aevix_v03_task_ready_exhaustion_tests PRIVATE cxx_std_20)
    target_compile_definitions(aevix_v03_task_ready_exhaustion_tests PRIVATE AEVIX_TASK_RUNTIME_TESTING AEVIX_TASK_TEST_ID_MAX=1)
    target_compile_options(aevix_v03_task_ready_exhaustion_tests PRIVATE
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fno-exceptions;-fno-rtti>")
    target_link_libraries(aevix_v03_task_ready_exhaustion_tests PRIVATE Threads::Threads)
    add_test(NAME v03_task_ready_exhaustion COMMAND aevix_v03_task_ready_exhaustion_tests)
    set_tests_properties(v03_task_ready_exhaustion PROPERTIES TIMEOUT 90)
    add_test(NAME v03_task_ready_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DRUNTIME=$<TARGET_FILE:aevix_target_runtime>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-task-ready-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_task_ready_cli_tests.cmake")
    set_tests_properties(v03_task_ready_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_task_ready_fault_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-task-ready-fault-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_task_ready_fault_cli_tests.cmake")
    set_tests_properties(v03_task_ready_fault_cli PROPERTIES TIMEOUT 600)
    add_executable(aevix_v03_task_ready_jit_tests tests/v03_task_ready_jit_tests.cpp)
    target_link_libraries(aevix_v03_task_ready_jit_tests PRIVATE aevix_frontend aevix_jit aevix_support)
    add_test(NAME v03_task_ready_jit COMMAND aevix_v03_task_ready_jit_tests)
    set_tests_properties(v03_task_ready_jit PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
    add_executable(aevix_v03_task_runtime_tests tests/v03_task_runtime_tests.cpp
        runtime/Threading/TaskRuntime.cpp runtime/Native/NativeBoundary.cpp)
    target_include_directories(aevix_v03_task_runtime_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(aevix_v03_task_runtime_tests PRIVATE cxx_std_20)
    target_compile_definitions(aevix_v03_task_runtime_tests PRIVATE AEVIX_TASK_RUNTIME_TESTING)
    target_compile_options(aevix_v03_task_runtime_tests PRIVATE
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fno-exceptions;-fno-rtti>")
    target_link_libraries(aevix_v03_task_runtime_tests PRIVATE Threads::Threads)
    add_test(NAME v03_task_runtime COMMAND aevix_v03_task_runtime_tests)
    set_tests_properties(v03_task_runtime PROPERTIES TIMEOUT 90)
    add_executable(aevix_v03_lock_runtime_tests tests/v03_lock_runtime_tests.cpp
        runtime/Threading/LockRuntime.cpp)
    target_include_directories(aevix_v03_lock_runtime_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(aevix_v03_lock_runtime_tests PRIVATE cxx_std_20)
    target_compile_definitions(aevix_v03_lock_runtime_tests PRIVATE AEVIX_LOCK_RUNTIME_TESTING)
    target_link_libraries(aevix_v03_lock_runtime_tests PRIVATE Threads::Threads)
    add_test(NAME v03_lock_runtime COMMAND aevix_v03_lock_runtime_tests)
    set_tests_properties(v03_lock_runtime PROPERTIES TIMEOUT 60)
    add_executable(aevix_v03_atomic_runtime_tests tests/v03_atomic_runtime_tests.cpp
        runtime/Threading/AtomicRuntime.cpp)
    target_include_directories(aevix_v03_atomic_runtime_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(aevix_v03_atomic_runtime_tests PRIVATE cxx_std_20)
    target_link_libraries(aevix_v03_atomic_runtime_tests PRIVATE Threads::Threads)
    add_test(NAME v03_atomic_runtime COMMAND aevix_v03_atomic_runtime_tests)
    set_tests_properties(v03_atomic_runtime PROPERTIES TIMEOUT 60)
    add_executable(aevix_v03_atomic_air_tests tests/v03_atomic_air_tests.cpp)
    target_link_libraries(aevix_v03_atomic_air_tests PRIVATE aevix_frontend)
    add_test(NAME v03_atomic_air COMMAND aevix_v03_atomic_air_tests)
    add_test(NAME v03_atomic_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-atomic-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_atomic_cli_tests.cmake")
    set_tests_properties(v03_atomic_cli PROPERTIES TIMEOUT 300)
    add_executable(aevix_v03_shared_runtime_tests tests/v03_shared_runtime_tests.cpp
        runtime/Memory/SharedRuntime.cpp runtime/Native/NativeBoundary.cpp)
    target_include_directories(aevix_v03_shared_runtime_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(aevix_v03_shared_runtime_tests PRIVATE cxx_std_20)
    target_compile_definitions(aevix_v03_shared_runtime_tests PRIVATE AEVIX_SHARED_RUNTIME_TESTING)
    target_link_libraries(aevix_v03_shared_runtime_tests PRIVATE Threads::Threads)
    add_test(NAME v03_shared_runtime COMMAND aevix_v03_shared_runtime_tests)
    add_executable(aevix_v03_shared_air_tests tests/v03_shared_air_tests.cpp)
    target_link_libraries(aevix_v03_shared_air_tests PRIVATE aevix_frontend)
    add_test(NAME v03_shared_air COMMAND aevix_v03_shared_air_tests)
    add_test(NAME v03_shared_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-shared-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_shared_cli_tests.cmake")
    set_tests_properties(v03_shared_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_vector_mutation_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-vector-mutation-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_vector_mutation_cli_tests.cmake")
    set_tests_properties(v03_vector_mutation_cli PROPERTIES TIMEOUT 300)
    add_executable(aevix_v03_vector_mutation_air_tests tests/v03_vector_mutation_air_tests.cpp)
    target_link_libraries(aevix_v03_vector_mutation_air_tests PRIVATE aevix_frontend)
    add_test(NAME v03_vector_mutation_air COMMAND aevix_v03_vector_mutation_air_tests)
    add_test(NAME v03_ai_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-ai-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_ai_cli_tests.cmake")
    set_tests_properties(v03_ai_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_env_time_process_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-env-time-process-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_env_time_process_cli_tests.cmake")
    set_tests_properties(v03_env_time_process_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_path_fs_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-path-fs-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_path_fs_cli_tests.cmake")
    set_tests_properties(v03_path_fs_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_hash_map_set_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-hash-map-set-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_hash_map_set_cli_tests.cmake")
    set_tests_properties(v03_hash_map_set_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_ordered_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DRUNTIME=$<TARGET_FILE:aevix_target_runtime>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-ordered-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_ordered_cli_tests.cmake")
    set_tests_properties(v03_ordered_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_math_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-math-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_math_cli_tests.cmake")
    set_tests_properties(v03_math_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_float_math_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-float-math-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_float_math_cli_tests.cmake")
    set_tests_properties(v03_float_math_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_floating_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-floating-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_floating_cli_tests.cmake")
    set_tests_properties(v03_floating_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_encoding_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DRUNTIME=$<TARGET_FILE:aevix_target_runtime>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-encoding-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_encoding_cli_tests.cmake")
    set_tests_properties(v03_encoding_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_encoding_bytes_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DRUNTIME=$<TARGET_FILE:aevix_target_runtime>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-encoding-bytes-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_encoding_bytes_cli_tests.cmake")
    set_tests_properties(v03_encoding_bytes_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_ascii_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-ascii-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_ascii_cli_tests.cmake")
    set_tests_properties(v03_ascii_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_boolean_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DRUNTIME=$<TARGET_FILE:aevix_target_runtime>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-boolean-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_boolean_cli_tests.cmake")
    set_tests_properties(v03_boolean_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_integer_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-integer-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_integer_cli_tests.cmake")
    set_tests_properties(v03_integer_cli PROPERTIES TIMEOUT 900)
    add_test(NAME v03_text_views_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DRUNTIME=$<TARGET_FILE:aevix_target_runtime>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-text-views-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_text_views_cli_tests.cmake")
    set_tests_properties(v03_text_views_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_sync_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-sync-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_sync_cli_tests.cmake")
    set_tests_properties(v03_sync_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_generic_owner_contract_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-generic-owner-contract-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_generic_owner_contract_cli_tests.cmake")
    set_tests_properties(v03_generic_owner_contract_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_nested_reference_effects_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-nested-reference-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_nested_reference_effects_cli_tests.cmake")
    set_tests_properties(v03_nested_reference_effects_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_owned_capability_effects_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-owned-capability-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_owned_capability_effects_cli_tests.cmake")
    set_tests_properties(v03_owned_capability_effects_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_nested_overload_inference_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-nested-overload-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_nested_overload_inference_cli_tests.cmake")
    set_tests_properties(v03_nested_overload_inference_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_builtin_result_inference_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-builtin-result-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_builtin_result_inference_cli_tests.cmake")
    set_tests_properties(v03_builtin_result_inference_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_task_generic_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-task-generic-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_task_generic_cli_tests.cmake")
    set_tests_properties(v03_task_generic_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_task_ownership_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-task-ownership-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_task_ownership_cli_tests.cmake")
    set_tests_properties(v03_task_ownership_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_task_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-task-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_task_cli_tests.cmake")
    set_tests_properties(v03_task_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_task_system_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-task-system-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_task_system_cli_tests.cmake")
    set_tests_properties(v03_task_system_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_task_resources_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-task-resources-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_task_resources_cli_tests.cmake")
    set_tests_properties(v03_task_resources_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_task_observation_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-task-observation-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_task_observation_cli_tests.cmake")
    set_tests_properties(v03_task_observation_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_task_scope_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-task-scope-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_task_scope_cli_tests.cmake")
    set_tests_properties(v03_task_scope_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_task_convenience_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-task-convenience-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_task_convenience_cli_tests.cmake")
    set_tests_properties(v03_task_convenience_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_task_fault_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-task-fault-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_task_fault_cli_tests.cmake")
    set_tests_properties(v03_task_fault_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_bytes_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DRUNTIME=$<TARGET_FILE:aevix_target_runtime>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-bytes-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_bytes_cli_tests.cmake")
    set_tests_properties(v03_bytes_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_bytes_buffers_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DRUNTIME=$<TARGET_FILE:aevix_target_runtime>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-bytes-buffer-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_bytes_buffers_cli_tests.cmake")
    set_tests_properties(v03_bytes_buffers_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_bytes_utf8_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DRUNTIME=$<TARGET_FILE:aevix_target_runtime>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-bytes-utf8-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_bytes_utf8_cli_tests.cmake")
    set_tests_properties(v03_bytes_utf8_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_float_bits_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-float-bits-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_float_bits_cli_tests.cmake")
    set_tests_properties(v03_float_bits_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_range_inspect_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-range-inspect-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_range_inspect_cli_tests.cmake")
    set_tests_properties(v03_range_inspect_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_range_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-range-source-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_range_cli_tests.cmake")
    set_tests_properties(v03_range_cli PROPERTIES TIMEOUT 900)
    add_test(NAME v03_range_materialization_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DAEVIX_RUNTIME_ARCHIVE=$<TARGET_FILE:aevix_target_runtime>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-range-materialization-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_range_materialization_cli_tests.cmake")
    set_tests_properties(v03_range_materialization_cli PROPERTIES TIMEOUT 900)
    add_test(NAME v03_char_scalar_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-char-scalar-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_char_scalar_cli_tests.cmake")
    set_tests_properties(v03_char_scalar_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_unicode_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DRUNTIME=$<TARGET_FILE:aevix_target_runtime>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-unicode-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_unicode_cli_tests.cmake")
    set_tests_properties(v03_unicode_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_unicode_properties_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DRUNTIME=$<TARGET_FILE:aevix_target_runtime>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-unicode-properties-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_unicode_properties_cli_tests.cmake")
    set_tests_properties(v03_unicode_properties_cli PROPERTIES TIMEOUT 900)
    add_test(NAME v03_variadic_provenance_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-variadic-provenance-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_variadic_provenance_cli_tests.cmake")
    set_tests_properties(v03_variadic_provenance_cli PROPERTIES TIMEOUT 300)
    add_executable(aevix_v03_thread_safety_analysis_tests tests/v03_thread_safety_analysis_tests.cpp)
    target_link_libraries(aevix_v03_thread_safety_analysis_tests PRIVATE aevix_frontend)
    add_test(NAME v03_thread_safety_analysis COMMAND aevix_v03_thread_safety_analysis_tests)
    add_executable(aevix_v03_task_air_tests tests/v03_task_air_tests.cpp)
    target_link_libraries(aevix_v03_task_air_tests PRIVATE aevix_frontend)
    add_test(NAME v03_task_air COMMAND aevix_v03_task_air_tests)
    add_executable(aevix_v03_float_bits_air_tests tests/v03_float_bits_air_tests.cpp)
    target_link_libraries(aevix_v03_float_bits_air_tests PRIVATE aevix_frontend)
    add_test(NAME v03_float_bits_air COMMAND aevix_v03_float_bits_air_tests)
    add_executable(aevix_v03_range_inspect_air_tests tests/v03_range_inspect_air_tests.cpp)
    target_link_libraries(aevix_v03_range_inspect_air_tests PRIVATE aevix_frontend)
    add_test(NAME v03_range_inspect_air COMMAND aevix_v03_range_inspect_air_tests)
    add_executable(aevix_v03_char_scalar_air_tests tests/v03_char_scalar_air_tests.cpp)
    target_link_libraries(aevix_v03_char_scalar_air_tests PRIVATE aevix_frontend)
    add_test(NAME v03_char_scalar_air COMMAND aevix_v03_char_scalar_air_tests)
    add_executable(aevix_v03_closure_consume_air_tests tests/v03_closure_consume_air_tests.cpp)
    target_link_libraries(aevix_v03_closure_consume_air_tests PRIVATE aevix_frontend)
    add_test(NAME v03_closure_consume_air COMMAND aevix_v03_closure_consume_air_tests)
    add_test(NAME v03_closure_consume_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DRUNTIME=$<TARGET_FILE:aevix_target_runtime>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-closure-consume-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_closure_consume_cli_tests.cmake")
    set_tests_properties(v03_closure_consume_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_map_entry_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DRUNTIME=$<TARGET_FILE:aevix_target_runtime>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-map-entry-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_map_entry_cli_tests.cmake")
    set_tests_properties(v03_map_entry_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_io_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-io-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_io_cli_tests.cmake")
    set_tests_properties(v03_io_cli PROPERTIES TIMEOUT 300)
    add_executable(aevix_v03_os_runtime_tests tests/v03_os_runtime_tests.cpp
        runtime/System/OsRuntime.cpp runtime/Native/NativeBoundary.cpp)
    target_include_directories(aevix_v03_os_runtime_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(aevix_v03_os_runtime_tests PRIVATE cxx_std_20)
    target_compile_definitions(aevix_v03_os_runtime_tests PRIVATE AEVIX_OS_RUNTIME_TESTING)
    target_link_libraries(aevix_v03_os_runtime_tests PRIVATE Threads::Threads)
    add_test(NAME v03_os_runtime COMMAND aevix_v03_os_runtime_tests)
    add_executable(aevix_v03_memory_air_tests tests/v03_memory_air_tests.cpp)
    target_link_libraries(aevix_v03_memory_air_tests PRIVATE aevix_frontend)
    add_test(NAME v03_memory_air COMMAND aevix_v03_memory_air_tests)
    add_test(NAME v03_memory_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-memory-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_memory_cli_tests.cmake")
    set_tests_properties(v03_memory_cli PROPERTIES TIMEOUT 240)
    add_executable(aevix_v03_library_generic_pressure_tests tests/v03_library_generic_pressure_tests.cpp)
    target_link_libraries(aevix_v03_library_generic_pressure_tests PRIVATE aevix_frontend)
    add_test(NAME v03_library_generic_pressure COMMAND aevix_v03_library_generic_pressure_tests "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME v03_library_generic_pressure_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-generic-pressure-fixtures"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_library_generic_pressure_cli_tests.cmake")
    set_tests_properties(v03_library_generic_pressure v03_library_generic_pressure_cli PROPERTIES TIMEOUT 240)
    add_executable(aevix_v03_system_air_tests tests/v03_system_air_tests.cpp)
    target_link_libraries(aevix_v03_system_air_tests PRIVATE aevix_frontend)
    add_test(NAME v03_system_air COMMAND aevix_v03_system_air_tests)
    add_test(NAME v03_system_safety_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-system-safety-fixtures" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_system_safety_cli_tests.cmake")
    set_tests_properties(v03_system_safety_cli PROPERTIES TIMEOUT 300)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME library_api_lifecycle COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/docs/library_api_tests")
        add_test(NAME library_error_reference COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/docs/error_api_tests")
        add_test(NAME library_api_inventory_current COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/docs/library_api" --root "${CMAKE_CURRENT_SOURCE_DIR}" check)
        add_test(NAME library_error_inventory_current COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/docs/error_api" --root "${CMAKE_CURRENT_SOURCE_DIR}" check)
        set_tests_properties(library_api_inventory_current library_error_inventory_current PROPERTIES TIMEOUT 180)
        add_test(NAME v03_library_manifest COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_library_manifest_tests" "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    add_test(NAME v03_reference_payload_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-reference-payload-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_reference_payload_cli_tests.cmake")
    set_tests_properties(v03_reference_payload_cli PROPERTIES TIMEOUT 240)
    add_test(NAME v03_system_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-system-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_system_cli_tests.cmake")
    set_tests_properties(v03_system_cli PROPERTIES TIMEOUT 600)
    add_test(NAME v03_enum_cleanup_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-enum-cleanup-fixtures" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_enum_cleanup_cli_tests.cmake")
    set_tests_properties(v03_enum_cleanup_cli PROPERTIES TIMEOUT 300)
    add_test(NAME v03_parse_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-parse-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_parse_cli_tests.cmake")
    add_test(NAME library_errors COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/library-error-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/library_error_tests.cmake")
    add_test(NAME v03_source_libraries COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-library-fixtures"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_library_cli_tests.cmake")
    add_test(NAME v03_text_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-text-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_text_cli_tests.cmake")
    set_tests_properties(v03_parse_cli library_errors v03_source_libraries v03_text_cli PROPERTIES TIMEOUT 240)
    add_executable(aevix_v03_system_runtime_tests tests/v03_system_runtime_tests.cpp)
    target_link_libraries(aevix_v03_system_runtime_tests PRIVATE aevix_runtime)
    add_test(NAME v03_system_runtime COMMAND aevix_v03_system_runtime_tests)
    add_executable(aevix_v03_parse_runtime_tests tests/v03_parse_runtime_tests.cpp)
    target_link_libraries(aevix_v03_parse_runtime_tests PRIVATE aevix_runtime)
    add_test(NAME v03_parse_runtime COMMAND aevix_v03_parse_runtime_tests)
    add_executable(aevix_native_boundary_tests tests/native_boundary_tests.cpp)
    target_link_libraries(aevix_native_boundary_tests PRIVATE aevix_runtime)
    add_test(NAME native_boundary COMMAND aevix_native_boundary_tests)
    add_executable(aevix_native_contract_tests tests/native_contract_tests.cpp)
    target_link_libraries(aevix_native_contract_tests PRIVATE aevix_frontend)
    add_test(NAME native_contract COMMAND aevix_native_contract_tests)
    add_test(NAME native_boundary_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/native-boundary-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_boundary_cli_tests.cmake")
    set_tests_properties(native_boundary_cli PROPERTIES TIMEOUT 60)
    add_executable(aevix_v03_raw_literal_tests tests/v03_raw_literal_tests.cpp)
    target_link_libraries(aevix_v03_raw_literal_tests PRIVATE aevix_frontend)
    add_test(NAME v03_raw_literals COMMAND aevix_v03_raw_literal_tests)
    add_executable(aevix_v03_parser_tests tests/v03_parser_tests.cpp)
    target_link_libraries(aevix_v03_parser_tests PRIVATE aevix_frontend)
    add_test(NAME v03_parser COMMAND aevix_v03_parser_tests)
    foreach(feature IN ITEMS return raw initializer drop drop_semantic drop_ownership)
        add_test(NAME v03_${feature}_cli
            COMMAND "${CMAKE_COMMAND}" "-DAEVIXC=$<TARGET_FILE:aevixc>"
                "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
                "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/v03-${feature}-fixtures"
                "-DRAW_FIXTURE_TOOL=$<TARGET_FILE:aevix_v03_raw_literal_tests>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/v03_${feature}_cli_tests.cmake")
        set_tests_properties(v03_${feature}_cli PROPERTIES TIMEOUT 180)
    endforeach()
    add_executable(aevix_v02_syntax_tests tests/v02_syntax_tests.cpp)
    target_link_libraries(aevix_v02_syntax_tests PRIVATE aevix_frontend)
    add_test(NAME v02_syntax COMMAND aevix_v02_syntax_tests)
    add_executable(aevix_v02_numeric_tests tests/v02_numeric_tests.cpp)
    target_link_libraries(aevix_v02_numeric_tests PRIVATE aevix_frontend aevix_jit)
    add_test(NAME v02_numeric COMMAND aevix_v02_numeric_tests "${AEVIX_CLANG_EXECUTABLE}")
    set_tests_properties(v02_numeric PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    add_executable(aevix_integer128_runtime_tests tests/integer128_runtime_tests.cpp)
    target_link_libraries(aevix_integer128_runtime_tests PRIVATE aevix_runtime)
    add_test(NAME integer128_runtime COMMAND aevix_integer128_runtime_tests)
    set_tests_properties(integer128_runtime PROPERTIES TIMEOUT 60)
    add_test(NAME integer128_cli
        COMMAND "${CMAKE_COMMAND}"
            "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/integer128-cli-fixtures"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/i128_cli_tests.cmake")
    set_tests_properties(integer128_cli PROPERTIES TIMEOUT 240)
    add_test(NAME const_generics_cli
        COMMAND "${CMAKE_COMMAND}"
            "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/const-generics-cli-fixtures"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/const_generics_cli_tests.cmake")
    add_test(NAME closure_cli
        COMMAND "${CMAKE_COMMAND}"
            "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/closure-cli-fixtures"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/closure_cli_tests.cmake")
    set_tests_properties(const_generics_cli closure_cli PROPERTIES TIMEOUT 180)
    add_test(NAME closure_parser
        COMMAND "${CMAKE_COMMAND}"
            "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/closure-parser-fixtures"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/closure_parser_tests.cmake")
    set_tests_properties(closure_parser PROPERTIES TIMEOUT 180)
    add_executable(aevix_complex_runtime_tests tests/complex_runtime_tests.cpp)
    target_link_libraries(aevix_complex_runtime_tests PRIVATE aevix_runtime)
    add_test(NAME complex_runtime COMMAND aevix_complex_runtime_tests)
    add_test(NAME complex_cli
        COMMAND "${CMAKE_COMMAND}"
            "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/complex-cli-fixtures"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/complex_cli_tests.cmake")
    set_tests_properties(complex_runtime complex_cli PROPERTIES TIMEOUT 180)
    add_executable(aevix_bench_runtime_tests tests/bench_runtime_tests.cpp)
    target_link_libraries(aevix_bench_runtime_tests PRIVATE aevix_runtime aevix_support)
    add_test(NAME bench_runtime COMMAND aevix_bench_runtime_tests)
    set_tests_properties(bench_runtime PROPERTIES TIMEOUT 60)
    add_test(NAME bench_cli COMMAND "${CMAKE_COMMAND}"
        "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/bench-cli-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/bench_cli_tests.cmake")
    set_tests_properties(bench_cli PROPERTIES TIMEOUT 120)
    add_executable(aevix_v02_values_tests tests/v02_values_tests.cpp)
    target_link_libraries(aevix_v02_values_tests PRIVATE aevix_frontend aevix_jit)
    target_compile_definitions(aevix_v02_values_tests PRIVATE AEVIX_TEST_RUNTIME_ARCHIVE="$<TARGET_FILE:aevix_target_runtime>")
    add_test(NAME v02_values COMMAND aevix_v02_values_tests "${AEVIX_CLANG_EXECUTABLE}")
    set_tests_properties(v02_values PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    add_executable(aevix_parallel_runtime_tests tests/parallel_runtime_tests.cpp)
    target_link_libraries(aevix_parallel_runtime_tests PRIVATE aevix_runtime)
    add_test(NAME parallel_runtime COMMAND aevix_parallel_runtime_tests)
    add_executable(aevix_parallel_fallback_tests tests/parallel_runtime_tests.cpp runtime/Threading/ParallelRuntime.cpp)
    target_include_directories(aevix_parallel_fallback_tests PRIVATE include)
    target_compile_features(aevix_parallel_fallback_tests PRIVATE cxx_std_20)
    target_compile_definitions(aevix_parallel_fallback_tests PRIVATE AEVIX_PARALLEL_TEST_CREATE_FAILURE=1)
    target_link_libraries(aevix_parallel_fallback_tests PRIVATE Threads::Threads)
    add_test(NAME parallel_fallback COMMAND aevix_parallel_fallback_tests)
    foreach(profile IN ITEMS simd parallel slice)
        add_test(NAME ${profile}_cli COMMAND "${CMAKE_COMMAND}"
            "-DAEVIXC=$<TARGET_FILE:aevixc>" "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/${profile}-cli-fixtures"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/${profile}_cli_tests.cmake")
        set_tests_properties(${profile}_cli PROPERTIES TIMEOUT 180)
    endforeach()
    if(UNIX)
        add_test(NAME installer COMMAND /bin/sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer_tests.sh")
        set_tests_properties(installer PROPERTIES TIMEOUT 60)
    endif()
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
    add_executable(aevix_v03_task_jit_lifetime_tests tests/v03_task_jit_lifetime_tests.cpp)
    target_link_libraries(aevix_v03_task_jit_lifetime_tests PRIVATE aevix_frontend aevix_jit aevix_support)
    add_test(NAME v03_task_jit_lifetime COMMAND aevix_v03_task_jit_lifetime_tests)
    set_tests_properties(v03_task_jit_lifetime PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
    add_executable(aevix_v03_task_scope_jit_prepare_tests tests/v03_task_scope_jit_prepare_tests.cpp)
    target_link_libraries(aevix_v03_task_scope_jit_prepare_tests PRIVATE aevix_frontend aevix_jit aevix_support)
    add_test(NAME v03_task_scope_jit_prepare COMMAND aevix_v03_task_scope_jit_prepare_tests)
    set_tests_properties(v03_task_scope_jit_prepare PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)

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
    add_test(NAME dependency_cli
        COMMAND "${CMAKE_COMMAND}"
            "-DAEVIXC=$<TARGET_FILE:aevixc>"
            "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/dependency-cli-fixtures"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/dependency_cli_tests.cmake")
    set_tests_properties(dependency_cli PROPERTIES TIMEOUT 120)
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
                "-DCONFIG=$<CONFIG>"
                "-DAEVIX_VERSION=${PROJECT_VERSION}"
                "-DCLANG=${AEVIX_CLANG_EXECUTABLE}"
                "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/installed-fixtures"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_tests.cmake")
        set_tests_properties(runtime_cli installed_package PROPERTIES TIMEOUT 180)
    endif()
endif()
