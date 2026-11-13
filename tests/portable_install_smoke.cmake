if(NOT DEFINED VEX_EXECUTABLE OR NOT DEFINED SOURCE_FILE OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "Missing portable VEX installation smoke-test argument")
endif()

file(MAKE_DIRECTORY "${WORK_DIR}")
if(WIN32)
    set(program_executable "${WORK_DIR}/hello.exe")
else()
    set(program_executable "${WORK_DIR}/hello")
endif()

get_filename_component(install_bin "${VEX_EXECUTABLE}" DIRECTORY)
get_filename_component(install_root "${install_bin}" DIRECTORY)
if(WIN32)
    set(bundled_zig "${install_root}/toolchain/zig.exe")
else()
    set(bundled_zig "${install_root}/toolchain/zig")
endif()
set(bundled_vsix "${install_root}/share/vex/vex-language-0.1.0.vsix")
if(NOT EXISTS "${bundled_zig}")
    message(FATAL_ERROR "The installed package is missing its bundled Zig toolchain: ${bundled_zig}")
endif()
if(NOT EXISTS "${bundled_vsix}")
    message(FATAL_ERROR "The installed package is missing the VS Code extension: ${bundled_vsix}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        --unset=VEX_LLVM_CC
        --unset=VEX_LLVM_ZIG
        --unset=LLVM_CC
        --unset=VEX_HOME
        "${VEX_EXECUTABLE}" --version
    RESULT_VARIABLE version_result
    OUTPUT_VARIABLE version_output
    ERROR_VARIABLE version_error
)
if(NOT version_result EQUAL 0 OR NOT version_output MATCHES "^VEX compiler ")
    message(FATAL_ERROR "Installed VEX version check failed: ${version_output}${version_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        --unset=VEX_LLVM_CC
        --unset=VEX_LLVM_ZIG
        --unset=LLVM_CC
        --unset=VEX_HOME
        "${VEX_EXECUTABLE}" run "${SOURCE_FILE}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "Installed VEX run failed (${run_result}): ${run_output}${run_error}")
endif()

string(REPLACE "\r\n" "\n" run_output "${run_output}")
if(NOT "${run_output}" STREQUAL "Hello, VEX!\n")
    message(FATAL_ERROR "Installed VEX run output mismatch. Actual:\n${run_output}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        --unset=VEX_LLVM_CC
        --unset=VEX_LLVM_ZIG
        --unset=LLVM_CC
        --unset=VEX_HOME
        "${VEX_EXECUTABLE}" build "${SOURCE_FILE}" -o "${program_executable}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "Installed VEX build failed (${build_result}): ${build_output}${build_error}")
endif()

execute_process(
    COMMAND "${program_executable}"
    RESULT_VARIABLE program_result
    OUTPUT_VARIABLE program_output
    ERROR_VARIABLE program_error
)
if(NOT program_result EQUAL 0)
    message(FATAL_ERROR "Installed VEX executable failed (${program_result}): ${program_output}${program_error}")
endif()

string(REPLACE "\r\n" "\n" program_output "${program_output}")
if(NOT "${program_output}" STREQUAL "Hello, VEX!\n")
    message(FATAL_ERROR "Installed VEX executable output mismatch. Actual:\n${program_output}")
endif()
