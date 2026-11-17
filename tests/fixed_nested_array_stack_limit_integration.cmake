if(NOT DEFINED VEX_EXECUTABLE OR NOT DEFINED SOURCE_FILE OR
   NOT DEFINED BACKEND OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "VEX_EXECUTABLE, SOURCE_FILE, BACKEND, and WORK_DIR are required")
endif()

file(MAKE_DIRECTORY "${WORK_DIR}")
set(output_path "${WORK_DIR}/oversized_nested_fixed${OUTPUT_SUFFIX}")

if(BACKEND STREQUAL "c")
    execute_process(
        COMMAND "${VEX_EXECUTABLE}" build-c "${SOURCE_FILE}" -o "${output_path}"
        RESULT_VARIABLE build_status
        OUTPUT_VARIABLE build_output
        ERROR_VARIABLE build_error
    )
    if(build_status EQUAL 0)
        message(FATAL_ERROR "C backend accepted nested fixed storage beyond a declared budget")
    endif()
    set(compiler_diagnostic "${build_output}\n${build_error}")
    if(NOT compiler_diagnostic MATCHES "nested fixed aggregate")
        message(FATAL_ERROR "C backend failed for an unexpected reason:\n${compiler_diagnostic}")
    endif()
elseif(BACKEND STREQUAL "llvm")
    if(NOT DEFINED LLVM_CLANG)
        message(FATAL_ERROR "LLVM_CLANG is required for the LLVM stack-limit test")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "VEX_LLVM_CC=${LLVM_CLANG}"
            "${VEX_EXECUTABLE}" build-llvm "${SOURCE_FILE}" -o "${output_path}"
        RESULT_VARIABLE build_status
        OUTPUT_VARIABLE build_output
        ERROR_VARIABLE build_error
    )
    if(NOT build_status EQUAL 0)
        message(FATAL_ERROR "LLVM backend failed to build the guarded program:\n${build_output}\n${build_error}")
    endif()
    execute_process(
        COMMAND "${output_path}"
        RESULT_VARIABLE run_status
        OUTPUT_VARIABLE run_output
        ERROR_VARIABLE run_error
    )
    if("${run_status}" STREQUAL "0")
        message(FATAL_ERROR "LLVM backend did not trap before allocating oversized nested fixed storage")
    endif()
else()
    message(FATAL_ERROR "Unsupported BACKEND: ${BACKEND}")
endif()
