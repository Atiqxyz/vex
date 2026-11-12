cmake_minimum_required(VERSION 3.20)
project(VEX VERSION 0.1.0 LANGUAGES CXX)
include(CTest)
include(CheckLanguage)
include(GNUInstallDirs)
check_language(C)
if(CMAKE_C_COMPILER)
    enable_language(C)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(VEX_EMBED_DEFAULT_C_COMPILER
    "Embed the C compiler configured by CMake as a runtime fallback" ON)
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
    src/c_backend.cpp
    src/process.cpp
    src/toolchain.cpp
)
target_include_directories(vex_frontend PUBLIC include)
target_compile_features(vex_frontend PUBLIC cxx_std_20)
target_link_libraries(vex_frontend PRIVATE vex_build_options)
add_executable(vex src/main.cpp)
target_link_libraries(vex PRIVATE vex_frontend vex_build_options)
target_compile_definitions(vex PRIVATE VEX_VERSION="${PROJECT_VERSION}")
if(CMAKE_C_COMPILER AND VEX_EMBED_DEFAULT_C_COMPILER)
    file(TO_CMAKE_PATH "${CMAKE_C_COMPILER}" VEX_DEFAULT_C_COMPILER_PATH)
    target_compile_definitions(vex_frontend PRIVATE
        VEX_DEFAULT_C_COMPILER="${VEX_DEFAULT_C_COMPILER_PATH}"
        VEX_DEFAULT_C_COMPILER_ID="${CMAKE_C_COMPILER_ID}"
    )
endif()

install(TARGETS vex
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES README.md LICENSE docs/install.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

if(BUILD_TESTING)
    add_executable(vex_tests
        tests/test_main.cpp
        tests/source_tests.cpp
        tests/lexer_tests.cpp
        tests/parser_tests.cpp
        tests/semantic_tests.cpp
        tests/ir_tests.cpp
        tests/c_backend_tests.cpp
    )
    target_link_libraries(vex_tests PRIVATE vex_frontend vex_build_options)
    add_test(NAME vex_frontend_tests COMMAND vex_tests)
    set_tests_properties(vex_frontend_tests PROPERTIES TIMEOUT 15)

    if(CMAKE_C_COMPILER)
        add_test(NAME vex_c_backend_integration
            COMMAND ${CMAKE_COMMAND}
                -DVEX_EXECUTABLE=$<TARGET_FILE:vex>
                -DC_COMPILER=${CMAKE_C_COMPILER}
                -DC_COMPILER_ID=${CMAKE_C_COMPILER_ID}
                -DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/c_backend.vex
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/c_backend_integration
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/c_backend_integration.cmake
        )
        set_tests_properties(vex_c_backend_integration PROPERTIES TIMEOUT 30)
    endif()
endif()
