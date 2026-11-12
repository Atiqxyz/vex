cmake_minimum_required(VERSION 3.20)

project(VEX VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_library(vex_frontend
    src/lexer.cpp
    src/token.cpp
)

target_include_directories(vex_frontend PUBLIC include)

if(MSVC)
    target_compile_options(vex_frontend PRIVATE /W4 /permissive-)
else()
    target_compile_options(vex_frontend PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

add_executable(vex src/main.cpp)
target_link_libraries(vex PRIVATE vex_frontend)

if(MSVC)
    target_compile_options(vex PRIVATE /W4 /permissive-)
else()
    target_compile_options(vex PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

if(BUILD_TESTING)
    add_executable(vex_tests tests/lexer_tests.cpp)
    target_link_libraries(vex_tests PRIVATE vex_frontend)

    if(MSVC)
        target_compile_options(vex_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(vex_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    endif()

    add_test(NAME vex_lexer_tests COMMAND vex_tests)
endif()
