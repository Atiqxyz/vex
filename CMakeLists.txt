cmake_minimum_required(VERSION 3.20)
project(VEX VERSION 0.1.0 LANGUAGES CXX)
include(CTest)
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
)
target_include_directories(vex_frontend PUBLIC include)
target_compile_features(vex_frontend PUBLIC cxx_std_20)
target_link_libraries(vex_frontend PRIVATE vex_build_options)
add_executable(vex src/main.cpp)
target_link_libraries(vex PRIVATE vex_frontend vex_build_options)
target_compile_definitions(vex PRIVATE VEX_VERSION="${PROJECT_VERSION}")
if(BUILD_TESTING)
    add_executable(vex_tests
        tests/test_main.cpp
        tests/source_tests.cpp
        tests/lexer_tests.cpp
        tests/parser_tests.cpp
        tests/semantic_tests.cpp
    )
    target_link_libraries(vex_tests PRIVATE vex_frontend vex_build_options)
    add_test(NAME vex_frontend_tests COMMAND vex_tests)
    set_tests_properties(vex_frontend_tests PROPERTIES TIMEOUT 15)
endif()
