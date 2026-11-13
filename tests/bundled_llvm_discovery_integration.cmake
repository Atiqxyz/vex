if(NOT DEFINED VEX_EXECUTABLE OR NOT DEFINED TOOLCHAIN_STUB OR
   NOT DEFINED SOURCE_FILE OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "Missing bundled LLVM discovery integration-test argument")
endif()

if(WIN32)
    set(vex_name "vex.exe")
    set(stub_name "vex_toolchain_stub.exe")
    set(clang_name "clang.exe")
else()
    set(vex_name "vex")
    set(stub_name "vex_toolchain_stub")
    set(clang_name "clang")
endif()

set(package_root "${WORK_DIR}/portable-vex")
set(package_bin "${package_root}/bin")
set(toolchain_bin "${package_root}/toolchain/bin")
file(MAKE_DIRECTORY "${package_bin}" "${toolchain_bin}")
file(COPY "${VEX_EXECUTABLE}" DESTINATION "${package_bin}")
file(COPY "${TOOLCHAIN_STUB}" DESTINATION "${toolchain_bin}")
file(RENAME "${toolchain_bin}/${stub_name}" "${toolchain_bin}/${clang_name}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        --unset=VEX_LLVM_CC
        --unset=LLVM_CC
        --unset=VEX_HOME
        "${package_bin}/${vex_name}" run-llvm "${SOURCE_FILE}"
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "Packaged VEX run-llvm failed (${run_result}): ${run_output}${run_error}")
endif()

string(REPLACE "\r\n" "\n" run_output "${run_output}")
if(NOT "${run_output}" STREQUAL "VEX_BUNDLED_CLANG_FOUND\n")
    message(FATAL_ERROR
        "Packaged VEX did not invoke its sibling toolchain/bin/clang. Output was:\n${run_output}")
endif()
