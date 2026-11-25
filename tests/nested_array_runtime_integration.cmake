if(NOT DEFINED VEX_EXECUTABLE OR NOT DEFINED SOURCE_FILE OR NOT DEFINED BACKEND)
    message(FATAL_ERROR "VEX_EXECUTABLE, SOURCE_FILE, and BACKEND are required")
endif()

set(expected_output "2\n2\n15\n3\n10\n20\n30\n3\ntrue\n3\n4\n7\n2\n48\n3\n11\n2\n0\n9\n3\n40\n60\n60\n7\n1\n5\n2\n8\n6\n1\n2\n30\n4\n1\n4\n37\n5\n6\n7\n90\n8\n9\n10\n99\n11\n22\n2\n11\n22\n")
if(BACKEND STREQUAL "llvm")
    if(NOT DEFINED LLVM_CLANG)
        message(FATAL_ERROR "LLVM_CLANG is required for the LLVM nested-array runtime test")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" run-llvm "${SOURCE_FILE}"
        RESULT_VARIABLE run_status
        OUTPUT_VARIABLE run_output
        ERROR_VARIABLE run_error
    )
elseif(BACKEND STREQUAL "c")
    execute_process(
        COMMAND "${VEX_EXECUTABLE}" run-c "${SOURCE_FILE}"
        RESULT_VARIABLE run_status
        OUTPUT_VARIABLE run_output
        ERROR_VARIABLE run_error
    )
else()
    message(FATAL_ERROR "Unsupported BACKEND: ${BACKEND}")
endif()

if(NOT run_status EQUAL 0)
    message(FATAL_ERROR "${BACKEND} nested-array program failed (${run_status}):\n${run_output}\n${run_error}")
endif()
if(NOT "${run_output}" STREQUAL "${expected_output}")
    message(FATAL_ERROR "${BACKEND} nested-array output mismatch.\nExpected:\n${expected_output}\nActual:\n${run_output}\nError:\n${run_error}")
endif()
