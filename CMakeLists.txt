cmake_minimum_required(VERSION 3.20)

# Statically link the MSVC runtime so VEX itself does not require users to
# install a separate Visual C++ Redistributable. CMake must know this before
# project() enables compiler languages; the setting is ignored by other compilers.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Build the VEX CLI for an older macOS baseline instead of inheriting the
# current GitHub runner's deployment target.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0")
endif()

project(VEX VERSION 0.1.0 LANGUAGES CXX)
include(CTest)
include(CheckLanguage)
check_language(C)
if(CMAKE_C_COMPILER)
    enable_language(C)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_library(vex_build_options INTERFACE)
if(MSVC)
    target_compile_options(vex_build_options INTERFACE /W4 /permissive- /Zc:__cplusplus)
else()
    target_compile_options(vex_build_options INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        -Werror=return-type
    )
endif()
add_library(vex_frontend STATIC
    src/source.cpp
    src/diagnostic.cpp
    src/token.cpp
    src/lexer.cpp
    src/ast.cpp
    src/parser.cpp
    src/semantic.cpp
    src/ir.cpp
    src/lowering.cpp
    src/backend.cpp
    src/c_backend.cpp
    src/llvm_backend.cpp
    src/process.cpp
    src/toolchain.cpp
)
target_include_directories(vex_frontend PUBLIC include)
target_compile_features(vex_frontend PUBLIC cxx_std_20)
target_link_libraries(vex_frontend PRIVATE vex_build_options)
add_executable(vex src/main.cpp)
target_link_libraries(vex PRIVATE vex_frontend vex_build_options)
target_compile_definitions(vex PRIVATE VEX_VERSION="${PROJECT_VERSION}")

# Keep the Linux CLI independent of the distro's libstdc++ package version.
if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(vex PRIVATE -static-libstdc++ -static-libgcc)
endif()

include(GNUInstallDirs)
install(TARGETS vex RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES docs/installation.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

set(VEX_LLVM_TOOLCHAIN_DIR "" CACHE PATH
    "Optional LLVM installation root to bundle beside the VEX executable")
set(VEX_ZIG_TOOLCHAIN_DIR "" CACHE PATH
    "Optional Zig installation root to bundle as the portable LLVM toolchain")
if(VEX_LLVM_TOOLCHAIN_DIR AND VEX_ZIG_TOOLCHAIN_DIR)
    message(FATAL_ERROR "Choose either VEX_LLVM_TOOLCHAIN_DIR or VEX_ZIG_TOOLCHAIN_DIR, not both")
endif()
if(VEX_ZIG_TOOLCHAIN_DIR)
    if(NOT IS_DIRECTORY "${VEX_ZIG_TOOLCHAIN_DIR}")
        message(FATAL_ERROR "VEX_ZIG_TOOLCHAIN_DIR is not a directory: ${VEX_ZIG_TOOLCHAIN_DIR}")
    endif()
    if(WIN32)
        set(VEX_BUNDLED_ZIG "${VEX_ZIG_TOOLCHAIN_DIR}/zig.exe")
    else()
        set(VEX_BUNDLED_ZIG "${VEX_ZIG_TOOLCHAIN_DIR}/zig")
    endif()
    if(NOT EXISTS "${VEX_BUNDLED_ZIG}")
        message(FATAL_ERROR "VEX_ZIG_TOOLCHAIN_DIR must contain zig or zig.exe at its root")
    endif()
    install(DIRECTORY "${VEX_ZIG_TOOLCHAIN_DIR}/"
        DESTINATION toolchain
        USE_SOURCE_PERMISSIONS)
endif()

if(VEX_LLVM_TOOLCHAIN_DIR)
    if(NOT IS_DIRECTORY "${VEX_LLVM_TOOLCHAIN_DIR}")
        message(FATAL_ERROR "VEX_LLVM_TOOLCHAIN_DIR is not a directory: ${VEX_LLVM_TOOLCHAIN_DIR}")
    endif()
    if(WIN32)
        set(VEX_BUNDLED_CLANG "${VEX_LLVM_TOOLCHAIN_DIR}/bin/clang.exe")
    else()
        set(VEX_BUNDLED_CLANG "${VEX_LLVM_TOOLCHAIN_DIR}/bin/clang")
    endif()
    if(NOT EXISTS "${VEX_BUNDLED_CLANG}")
        message(FATAL_ERROR
            "VEX_LLVM_TOOLCHAIN_DIR must contain bin/clang or bin/clang.exe")
    endif()
    install(DIRECTORY "${VEX_LLVM_TOOLCHAIN_DIR}/"
        DESTINATION toolchain
        USE_SOURCE_PERMISSIONS)
endif()
if(CMAKE_C_COMPILER)
    file(TO_CMAKE_PATH "${CMAKE_C_COMPILER}" VEX_DEFAULT_C_COMPILER_PATH)
    target_compile_definitions(vex_frontend PRIVATE
        VEX_DEFAULT_C_COMPILER="${VEX_DEFAULT_C_COMPILER_PATH}"
        VEX_DEFAULT_C_COMPILER_ID="${CMAKE_C_COMPILER_ID}"
    )
endif()
if(BUILD_TESTING)
    add_executable(vex_tests
        tests/test_main.cpp
        tests/source_tests.cpp
        tests/lexer_tests.cpp
        tests/parser_tests.cpp
        tests/semantic_tests.cpp
        tests/ir_tests.cpp
        tests/backend_tests.cpp
        tests/c_backend_tests.cpp
        tests/llvm_backend_tests.cpp
        tests/toolchain_discovery_tests.cpp
    )
    target_link_libraries(vex_tests PRIVATE vex_frontend vex_build_options)
    add_test(NAME vex_frontend_tests COMMAND vex_tests)
    set_tests_properties(vex_frontend_tests PROPERTIES TIMEOUT 15)

    add_executable(vex_toolchain_stub tests/toolchain_stub.cpp)
    target_link_libraries(vex_toolchain_stub PRIVATE vex_build_options)
    add_test(NAME vex_bundled_llvm_discovery_integration
        COMMAND ${CMAKE_COMMAND}
            -DVEX_EXECUTABLE=$<TARGET_FILE:vex>
            -DTOOLCHAIN_STUB=$<TARGET_FILE:vex_toolchain_stub>
            -DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/c_backend.vex
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/bundled LLVM discovery integration"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bundled_llvm_discovery_integration.cmake
    )
    set_tests_properties(vex_bundled_llvm_discovery_integration PROPERTIES TIMEOUT 30)

    find_program(VEX_LLVM_CLANG_EXECUTABLE NAMES clang clang-21 clang-20 clang-19 clang-18 clang-17 clang-16 clang-15)
    if(VEX_LLVM_CLANG_EXECUTABLE)
        add_test(NAME vex_llvm_backend_integration
            COMMAND ${CMAKE_COMMAND}
                -DVEX_EXECUTABLE=$<TARGET_FILE:vex>
                -DLLVM_CLANG=${VEX_LLVM_CLANG_EXECUTABLE}
                -DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/c_backend.vex
                -DARRAY_BOUNDS_SOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/array_out_of_bounds.vex
                -DREMAINDER_ZERO_SOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/remainder_by_zero.vex
                -DDYNAMIC_ARRAY_POP_EMPTY_SOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dynamic_array_pop_empty.vex
                -DDYNAMIC_ARRAY_BOUNDS_SOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dynamic_array_out_of_bounds.vex
                -DSTRUCT_SOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/struct_values.vex
                -DSTRUCT_LAYOUT_SOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/struct_layout.vex
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/llvm backend integration"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/llvm_backend_integration.cmake
        )
        set_tests_properties(vex_llvm_backend_integration PROPERTIES TIMEOUT 60)
    endif()

    if(CMAKE_C_COMPILER)
        add_test(NAME vex_c_backend_integration
            COMMAND ${CMAKE_COMMAND}
                -DVEX_EXECUTABLE=$<TARGET_FILE:vex>
                -DC_COMPILER=${CMAKE_C_COMPILER}
                -DC_COMPILER_ID=${CMAKE_C_COMPILER_ID}
                -DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/c_backend.vex
                -DARRAY_BOUNDS_SOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/array_out_of_bounds.vex
                -DREMAINDER_ZERO_SOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/remainder_by_zero.vex
                -DDYNAMIC_ARRAY_POP_EMPTY_SOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dynamic_array_pop_empty.vex
                -DDYNAMIC_ARRAY_BOUNDS_SOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dynamic_array_out_of_bounds.vex
                -DSTRUCT_SOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/struct_values.vex
                -DSTRUCT_LAYOUT_SOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/struct_layout.vex
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/c_backend_integration
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/c_backend_integration.cmake
        )
        set_tests_properties(vex_c_backend_integration PROPERTIES TIMEOUT 30)
    endif()
endif()
