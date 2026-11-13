if(NOT DEFINED VEX_EXECUTABLE OR NOT DEFINED TOOLCHAIN_STUB OR
   NOT DEFINED SOURCE_FILE OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "Missing bundled LLVM discovery integration-test argument")
endif()

if(WIN32)
    set(vex_name "vex.exe")
    set(stub_name "vex_toolchain_stub.exe")
    set(clang_name "clang.exe")
    set(zig_name "zig.exe")
else()
    set(vex_name "vex")
    set(stub_name "vex_toolchain_stub")
    set(clang_name "clang")
    set(zig_name "zig")
endif()

set(package_root "${WORK_DIR}/portable-vex")
set(package_bin "${package_root}/bin")
set(toolchain_root "${package_root}/toolchain")
set(toolchain_bin "${toolchain_root}/bin")
file(MAKE_DIRECTORY "${package_bin}" "${toolchain_bin}")
file(COPY "${VEX_EXECUTABLE}" DESTINATION "${package_bin}")
file(COPY "${TOOLCHAIN_STUB}" DESTINATION "${toolchain_root}")
file(RENAME "${toolchain_root}/${stub_name}" "${toolchain_root}/${zig_name}")

# First prove that the portable layout discovers the Zig driver and invokes
# its "clang" subcommand without any environment overrides.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        --unset=VEX_LLVM_CC
        --unset=VEX_LLVM_ZIG
        --unset=LLVM_CC
        --unset=VEX_HOME
        "${package_bin}/${vex_name}" run-llvm "${SOURCE_FILE}"
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE zig_result
    OUTPUT_VARIABLE zig_output
    ERROR_VARIABLE zig_error
)
if(NOT zig_result EQUAL 0)
    message(FATAL_ERROR "Packaged VEX did not run through sibling toolchain/zig (${zig_result}): ${zig_output}${zig_error}")
endif()

string(REPLACE "\r\n" "\n" zig_output "${zig_output}")
if(NOT "${zig_output}" STREQUAL "VEX_BUNDLED_CLANG_FOUND\n")
    message(FATAL_ERROR "Packaged VEX did not invoke sibling toolchain/zig clang. Output was:\n${zig_output}")
endif()

# Also retain coverage for the previous portable clang layout.
file(REMOVE "${toolchain_root}/${zig_name}")
file(COPY "${TOOLCHAIN_STUB}" DESTINATION "${toolchain_bin}")
file(RENAME "${toolchain_bin}/${stub_name}" "${toolchain_bin}/${clang_name}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        --unset=VEX_LLVM_CC
        --unset=VEX_LLVM_ZIG
        --unset=LLVM_CC
        --unset=VEX_HOME
        "${package_bin}/${vex_name}" run-llvm "${SOURCE_FILE}"
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE clang_result
    OUTPUT_VARIABLE clang_output
    ERROR_VARIABLE clang_error
)
if(NOT clang_result EQUAL 0)
    message(FATAL_ERROR "Packaged VEX did not run through sibling toolchain/bin/clang (${clang_result}): ${clang_output}${clang_error}")
endif()

string(REPLACE "\r\n" "\n" clang_output "${clang_output}")
if(NOT "${clang_output}" STREQUAL "VEX_BUNDLED_CLANG_FOUND\n")
    message(FATAL_ERROR "Packaged VEX did not invoke sibling toolchain/bin/clang. Output was:\n${clang_output}")
endif()
